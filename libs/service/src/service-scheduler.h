#pragma once

#include "service-datasources.h"
#include "service-memory.h"

#include <condition_variable>
#include <list>
#include <map>
#include <thread>

namespace mapget::detail
{

class TileLoadJob;

/** Scheduler-owned concurrency permit state for one primary datasource. */
struct SourceConcurrency
{
    /** Immutable datasource registration used by jobs after registry removal. */
    RegisteredDataSource::Ptr source;

    /** Maximum concurrent calls into this datasource. */
    size_t limit = 0;

    /** Datasource calls currently holding this source's concurrency permit. */
    size_t running = 0;

    /** False after removal, preventing new jobs while existing calls drain. */
    bool enabled = true;
};

/** Shared state for one coalesced source-tile load. */
struct TileLoadState
{
    MapTileKey tileKey;
    std::vector<LayerTilesRequest::Ptr> waitingRequests;
    std::optional<std::chrono::system_clock::time_point> cacheExpiredAt;
    TileLayer::LoadState loadStatus = TileLayer::LoadState::LoadingQueued;
    uint64_t mapEpoch = 0;
};

/** Snapshot of global pool and queue pressure. */
struct ServiceSchedulerStatistics
{
    size_t workerCount = 0;
    size_t runningJobs = 0;
    size_t activeTileRequests = 0;
    size_t queuedTileWorkItems = 0;
    size_t inFlightTileJobs = 0;
};

/** Per-datasource permit telemetry for the status endpoint. */
struct SourceConcurrencyStatistics
{
    std::string sourceId;
    std::string mapId;
    size_t limit = 0;
    size_t running = 0;
};

/**
 * Global homogeneous job scheduler.
 *
 * Each worker owns one source tile through cache/backend loading and all
 * attached request processing. Per-datasource permits constrain only backend
 * access without reserving threads for idle sources or retaining loaded tiles
 * in a second evaluation queue.
 */
class ServiceScheduler
{
public:
    /** Start the bounded worker pool around one shared cache and source registry. */
    ServiceScheduler(
        DataSourceRegistry& dataSources,
        Cache::Ptr cache,
        std::optional<std::chrono::milliseconds> defaultTtl,
        size_t workerCount);
    /** Stop and join workers before releasing scheduler-owned state. */
    ~ServiceScheduler() noexcept;

    ServiceScheduler(ServiceScheduler const&) = delete;
    ServiceScheduler& operator=(ServiceScheduler const&) = delete;

    /** Make a primary datasource eligible for tile work. */
    void registerDataSource(RegisteredDataSource::Ptr const& source);

    /** Prevent new work for a datasource while allowing running calls to drain. */
    void unregisterDataSource(RegisteredDataSource::Ptr const& source);

    /** Enqueue a validated tile request. */
    void enqueueRequest(LayerTilesRequest::Ptr request);

    /** Abort and detach one tile request from queued and in-flight work. */
    void abortRequest(LayerTilesRequest::Ptr const& request);

    /** Detach removed outputs while preserving retained queued/in-flight work. */
    void retainRequestOutputs(
        LayerTilesRequest::Ptr const& request,
        std::set<TileId> const& retainedTileIds);

    /** Register an active request's cooperative memory tracker. */
    void addFilterMemoryTracker(std::shared_ptr<FilterMemoryTracker> const& tracker);

    /** Abort queued work and invalidate all cached/in-flight content for a map. */
    void invalidateMap(std::string const& mapId);

    /** Stop accepting jobs, discard queued work, and join all workers. */
    void stop() noexcept;

    /** Return immutable queue-pressure statistics. */
    [[nodiscard]] ServiceSchedulerStatistics statistics() const;

    /** Return one permit row per currently registered primary source. */
    [[nodiscard]] std::vector<SourceConcurrencyStatistics> sourceStatistics() const;

    /** Account scheduler containers and return active filter trackers for later sampling. */
    void collectMemoryUsage(
        MemoryUsageBreakdown& scheduler,
        MemoryUsageBreakdown& telemetry,
        std::vector<std::shared_ptr<FilterMemoryTracker>>& filterTrackers);

    /** Return the service cache shared with datasource calls. */
    [[nodiscard]] Cache::Ptr cache() const { return cache_; }

private:
    /** One schedulable request/tile selection retained across inline handling. */
    struct Candidate
    {
        std::list<LayerTilesRequest::Ptr>::const_iterator requestIt;
        LayerTilesRequest::Ptr request;
        MapTileKey tileKey;
        size_t nextTileIndex = 0;
    };

    /** Worker entry point; every thread processes complete source-tile jobs. */
    void workerLoop();

    /** Return whether at least one job can make progress while the mutex is held. */
    [[nodiscard]] bool hasRunnableWorkLocked() const;

    /** Materialize the next source-affine tile job in round-robin source order. */
    [[nodiscard]] std::unique_ptr<TileLoadJob>
    takeNextTileJobLocked(std::unique_lock<std::mutex>& lock);

    /** Release a datasource permit before the owning worker processes consumers. */
    void releaseSourcePermit(std::shared_ptr<SourceConcurrency> const& source) noexcept;

    /** Find the next unresolved tile in one matching request. */
    [[nodiscard]] std::optional<Candidate>
    nextCandidateLocked(SourceConcurrency const& source) const;

    /** Find the next pending key at or after a request's scheduling cursor. */
    [[nodiscard]] std::optional<size_t>
    nextPendingTileKeyLocked(LayerTilesRequest const& request) const;

    /** Check request status, source assertion, map, and layer compatibility. */
    [[nodiscard]] bool requestMatchesSourceLocked(
        LayerTilesRequest::Ptr const& request,
        SourceConcurrency const& source) const;

    /** Coalesce equivalent queued requests onto a newly materialized tile job. */
    void attachMatchingRequestsLocked(
        LayerTilesRequest::Ptr const& selectedRequest,
        SourceConcurrency const& source,
        MapTileKey const& tileKey,
        std::vector<LayerTilesRequest::Ptr>& waitingRequests) const;
    /** Remove terminal requests and requests with no unscheduled keys. */
    void removeCompletedRequestsLocked();

    /** Publish a successful tile only if its map epoch is still current. */
    void completeTileJob(
        TileLoadState const& job,
        TileLayer::Ptr const& layer,
        bool updateCache);

    /** Abort every request waiting on a failed tile load. */
    void failTileJob(TileLoadState const& job);

    /** Notify current waiters without invoking callbacks under the scheduler lock. */
    void notifyTileLoadState(TileLoadState& job, TileLayer::LoadState state);

    DataSourceRegistry& dataSources_;
    Cache::Ptr cache_;
    std::optional<std::chrono::milliseconds> defaultTtl_;

    mutable std::mutex mutex_;
    std::condition_variable jobsAvailable_;
    std::list<LayerTilesRequest::Ptr> requests_;
    std::map<MapTileKey, std::shared_ptr<TileLoadState>> inFlightTiles_;
    std::vector<std::shared_ptr<SourceConcurrency>> sources_;
    std::vector<std::weak_ptr<FilterMemoryTracker>> filterMemoryTrackers_;
    std::map<std::string, uint64_t> mapEpochs_;
    std::vector<std::thread> workers_;
    size_t nextSourceIndex_ = 0;
    size_t runningJobs_ = 0;
    bool stopping_ = false;

    friend class TileLoadJob;
};

}  // namespace mapget::detail
