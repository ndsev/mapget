#pragma once

#include "service-datasources.h"
#include "service-memory.h"

#include <condition_variable>
#include <deque>
#include <list>
#include <map>
#include <thread>

namespace mapget::detail
{

/** Scheduler-owned concurrency permit state for one primary datasource. */
struct SourceConcurrency
{
    /** Immutable datasource registration used by jobs after registry removal. */
    RegisteredDataSource::Ptr source;

    /** Maximum concurrent calls into this datasource. */
    size_t limit = 0;

    /** Calls currently executing in the homogeneous worker pool. */
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

/** Coarse job category used only for scheduling telemetry. */
enum class ServiceJobKind {
    TileLoad,
    FilterEvaluation,
};

/** One unit executable by any service worker. */
class ServiceJob
{
public:
    /** Allow destruction through the scheduler-owned interface. */
    virtual ~ServiceJob() = default;

    /** Execute the job and contain all exceptions at the job boundary. */
    virtual void run() noexcept = 0;

    /** Release queued ownership when the scheduler discards this job. */
    virtual void discard() noexcept = 0;

    /** Return true when execution no longer has an observable consumer. */
    [[nodiscard]] virtual bool cancelled() const = 0;

    /** Return the datasource permit consumed by this job, or null for CPU-only work. */
    [[nodiscard]] virtual std::shared_ptr<SourceConcurrency> sourceAffinity() const = 0;

    /** Return the affected map for invalidation of queued work. */
    [[nodiscard]] virtual std::string_view mapId() const = 0;

    /** Return the filter request owner when this is derived filter work. */
    [[nodiscard]] virtual FeatureLayerFilterTilesRequest::Ptr filterOwner() const { return {}; }

    /** Return the job category for fair scheduling and telemetry. */
    [[nodiscard]] virtual ServiceJobKind kind() const = 0;
};

/** Snapshot of global pool and queue pressure. */
struct ServiceSchedulerStatistics
{
    size_t workerCount = 0;
    size_t runningJobs = 0;
    size_t activeTileRequests = 0;
    size_t inFlightTileJobs = 0;
    size_t queuedFilterJobs = 0;
    size_t runningFilterJobs = 0;
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
 * Tile loading and CPU-only derived evaluation share one bounded worker pool.
 * Per-datasource permits constrain backend calls without reserving threads for
 * idle sources.
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

    /** Enqueue source-independent work for the same homogeneous workers. */
    void enqueueJob(std::unique_ptr<ServiceJob> job);

    /** Discard queued filter jobs owned by one request. */
    void abortFilterJobs(FeatureLayerFilterTilesRequest::Ptr const& request);

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

    /** Worker entry point; every thread consumes both tile and derived jobs. */
    void workerLoop();

    /** Return whether at least one job can make progress while the mutex is held. */
    [[nodiscard]] bool hasRunnableWorkLocked() const;

    /** Select one job, processing cache hits and joins inline as needed. */
    [[nodiscard]] std::unique_ptr<ServiceJob> takeNextJobLocked(std::unique_lock<std::mutex>& lock);

    /** Materialize the next source-affine tile job in round-robin source order. */
    [[nodiscard]] std::unique_ptr<ServiceJob>
    takeNextTileJobLocked(std::unique_lock<std::mutex>& lock);

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
    void completeTileJob(TileLoadState const& job, TileLayer::Ptr const& layer);

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
    std::deque<std::unique_ptr<ServiceJob>> queuedJobs_;
    std::vector<std::shared_ptr<SourceConcurrency>> sources_;
    std::vector<std::weak_ptr<FilterMemoryTracker>> filterMemoryTrackers_;
    std::map<std::string, uint64_t> mapEpochs_;
    std::vector<std::thread> workers_;
    size_t nextSourceIndex_ = 0;
    size_t runningJobs_ = 0;
    size_t runningFilterJobs_ = 0;
    bool preferDerivedJob_ = false;
    bool stopping_ = false;

    friend class TileLoadJob;
};

}  // namespace mapget::detail
