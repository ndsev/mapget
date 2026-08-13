#pragma once

#include "cache.h"
#include "config.h"
#include "datasource.h"
#include "mapget/model/featurelayer-filter.h"
#include "mapget/model/layer.h"
#include "mapget/model/sourcedatalayer.h"
#include "memcache.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace mapget
{

namespace detail
{
class AttachmentRequestExecution;
class FilterEvaluationJob;
class FilterRequestExecution;
class LocateRequestExecution;
class ServiceScheduler;
class TileLoadJob;
}  // namespace detail

enum class RequestStatus {
    Open = 0x0,
    Success = 0x1,      /** The request has been fully satisfied. */
    NoDataSource = 0x2, /** No data source could provide the requested map + layer. */
    Unauthorized = 0x3, /** The user is not authorized to access the requested data source. */
    Aborted = 0x4       /** Canceled, e.g. because a bundled request cannot be fulfilled. */
};

enum class NoDataSourceReason {
    None = 0x0,
    EmptySources = 0x1,
    AllSourcesDisabled = 0x2,
    DatasourceInitializationFailed = 0x3,
    MissingMapOrLayer = 0x4,
    NoConfig = 0x5,
};

/** Lifecycle state for one config-backed datasource catalog row. */
enum class DataSourceCatalogStatus {
    /** Construction is running or waiting for a construction slot. */
    Initializing,
    /** Construction succeeded and `info`/`dataSource` are usable. */
    Ready,
    /** Construction failed; `statusMessage` contains diagnostics. */
    Failed
};

/** Service-owned datasource catalog row used by `/sources` and interactive invalidation. */
struct DataSourceCatalogEntry
{
    /** Cheap static config facts available before construction. */
    DataSourceDescriptor descriptor;

    /** Current lifecycle state serialized through `/sources`. */
    DataSourceCatalogStatus status = DataSourceCatalogStatus::Initializing;

    /** Human-readable progress/failure detail for UI and diagnostics. */
    std::string statusMessage;

    /** Optional constructor progress percentage in the inclusive range 0..100. */
    std::optional<float> progress;

    /** Ready datasource instance; needed for worker registration and config reload/removal. */
    DataSource::Ptr dataSource;

    /** Shared immutable metadata snapshot used by workers and ordered `/sources` serialization. */
    std::shared_ptr<DataSourceInfo const> info;
};

/** Lightweight per-source delta embedded into interactive catalog-change messages. */
struct DataSourceCatalogSourceUpdate
{
    /** Static config facts; `configIndex` identifies the row and auth rules scope visibility. */
    DataSourceDescriptor descriptor;

    /** Current lifecycle state after the change. */
    DataSourceCatalogStatus status = DataSourceCatalogStatus::Initializing;

    /** Current human-readable progress/failure detail; empty clears the previous UI text. */
    std::string statusMessage;

    /** Current optional progress percentage; null clears the previous UI progress value. */
    std::optional<float> progress;

    /** Ready datasource instance used only for auth filtering; it is never serialized. */
    DataSource::Ptr dataSource;
};

/** Ordered datasource catalog snapshot for one authorized caller. */
struct DataSourceCatalogSnapshot
{
    /** Monotonic catalog revision used for ETag generation and WebSocket invalidation. */
    uint64_t revision = 0;

    /** Global config status (`ok` or `error`) because parse failures are not tied to one source
     * row. */
    std::string configStatus = "ok";

    /** Human-readable config parse/validation error; empty when `configStatus == "ok"`. */
    std::string configStatusMessage;

    /** Ordered catalog entries visible to the authorized caller. */
    std::vector<DataSourceCatalogEntry> sources;
};

/** Lightweight notification emitted whenever the datasource catalog changes. */
struct DataSourceCatalogChange
{
    /** Revision after the change was applied. */
    uint64_t revision = 0;

    /** Coarse reason string for clients that want to debounce or classify reloads. */
    std::string reason;

    /** Optional per-source status delta, omitted for full reload/add/remove invalidations. */
    std::optional<DataSourceCatalogSourceUpdate> sourceUpdate;
};

struct LayerRequestContext
{
    RequestStatus status_ = RequestStatus::NoDataSource;
    NoDataSourceReason noDataSourceReason_ = NoDataSourceReason::MissingMapOrLayer;
    LayerType layerType_ = LayerType::Features;
};

/**
 * Result of synchronous attachment admission and production.
 *
 * `Success` with no response means that the selected datasource or source
 * tile has no matching attachment.
 */
struct AttachmentResult
{
    RequestStatus status_ = RequestStatus::NoDataSource;
    std::optional<AttachmentResponse> response_;
};

/**
 * Client request for map data, which consists of a map id,
 * a map layer id, an array of tile ids, and a callback function
 * which signals results.
 */
class LayerTilesRequest
{
    friend class Service;
    friend class HttpClient;
    friend class detail::AttachmentRequestExecution;
    friend class detail::FilterRequestExecution;
    friend class detail::LocateRequestExecution;
    friend class detail::ServiceScheduler;
    friend class detail::TileLoadJob;

public:
    using Ptr = std::shared_ptr<LayerTilesRequest>;

    /** Construct a request for tiles with the relevant parameters. */
    LayerTilesRequest(std::string mapId, std::string layerId, std::vector<TileId> tiles);

    /** Construct a request with foreground tile IDs prioritized for scheduling. */
    LayerTilesRequest(
        std::string mapId,
        std::string layerId,
        std::vector<TileId> tiles,
        std::vector<TileId> const& priorityTileIds);

    /** Get the current status of the request. */
    RequestStatus getStatus();

    /** Wait for the request to be done. */
    void wait();

    /** Check whether the request is done or still running. */
    bool isDone();

    /** The map id for which this request is dedicated. */
    std::string mapId_;

    /** The map layer id for which this request is dedicated. */
    std::string layerId_;

    /** Optional catalog source selector/assertion; never part of MapTileKey. */
    std::optional<std::string> sourceId_;

    /** Map tile IDs for this request, in caller-specified processing order. */
    std::vector<TileId> tileIds_;

    /**
     * Tile IDs within this request that should be scheduled before regular
     * tiles. This is a scheduling hint only; it does not add tiles to the
     * request by itself.
     */
    std::set<TileId> priorityTileIds_;

    /**
     * Optional canonical feature IDs to retain in each returned feature tile.
     * Source loading and caching remain tile-granular; restriction is applied
     * only to this request's immutable response value.
     */
    std::map<TileId, std::vector<std::string>> featureIdsByTile_;

    /**
     * The callback function which is called when all tiles have been processed.
     */
    std::function<void(RequestStatus)> onDone_;

    /**
     * The callback function which is called when all tiles have been processed.
     */
    template <class Fun>
    LayerTilesRequest& onFeatureLayer(Fun&& callback)
    {
        onFeatureLayer_ = std::forward<Fun>(callback);
        return *this;
    }

    template <class Fun>
    LayerTilesRequest& onSourceDataLayer(Fun&& callback)
    {
        onSourceDataLayer_ = std::forward<Fun>(callback);
        return *this;
    }

    /**
     * Callback for per-tile load-state changes.
     */
    template <class Fun>
    LayerTilesRequest& onLayerLoadStateChanged(Fun&& callback)
    {
        onLoadStateChanged_ = std::forward<Fun>(callback);
        return *this;
    }

protected:
    virtual void notifyResult(TileLayer::Ptr);
    void notifyLoadState(MapTileKey const& key, TileLayer::LoadState state) const;
    void setStatus(RequestStatus s);
    void notifyStatus();
    nlohmann::json toJson();

private:
    /** Resolve tile IDs into concrete keys while preserving priority/order. */
    void prepareResolvedLayer(LayerType layerType);

    /**
     * The callback functions which are called when a result tile is available.
     */
    std::function<void(TileFeatureLayer::Ptr)> onFeatureLayer_;
    std::function<void(TileSourceDataLayer::Ptr)> onSourceDataLayer_;
    std::function<void(MapTileKey const&, TileLayer::LoadState)> onLoadStateChanged_;

    // So the service can track which tile index from resolvedTileKeys_
    // is next in line to be processed.
    size_t nextTileIndex_ = 0;

    // Resolved tile keys in scheduling order.
    std::vector<MapTileKey> resolvedTileKeys_;

    // Track which resolved tile keys still need to be scheduled/served.
    std::set<MapTileKey> tileKeysNotStarted_;

    // So the requester can track how many results have been received.
    std::atomic_size_t resultCount_ = 0;

    // Mutex/condition variable for reading/setting request status.
    std::mutex statusMutex_;
    std::condition_variable statusConditionVariable_;
    std::atomic<RequestStatus> status_ = RequestStatus::Open;
};

/**
 * Client request for server-side multi-channel feature filtering.
 *
 * The service loads source feature tiles through the normal scheduler/cache
 * path, then runs SIMFIL evaluation as a scheduled service job.
 */
class FeatureLayerFilterTilesRequest
{
    friend class Service;
    friend class HttpClient;
    friend class detail::FilterEvaluationJob;
    friend class detail::FilterRequestExecution;
    friend class detail::ServiceScheduler;

public:
    using Ptr = std::shared_ptr<FeatureLayerFilterTilesRequest>;

    /** Construct a filter request over a set of source feature tile IDs. */
    FeatureLayerFilterTilesRequest(
        std::string mapId,
        std::string layerId,
        std::vector<TileId> tiles,
        FeatureLayerFilterRequest filter);

    /** Construct a filter request with foreground tile IDs prioritized for source loads. */
    FeatureLayerFilterTilesRequest(
        std::string mapId,
        std::string layerId,
        std::vector<TileId> tiles,
        FeatureLayerFilterRequest filter,
        std::vector<TileId> const& priorityTileIds);

    /** Get the current status of the filter request. */
    RequestStatus getStatus();

    /** Wait for the request to reach a terminal state. */
    void wait();

    /** Check whether the request is done or still running. */
    bool isDone();

    /** Check whether the request has been cancelled by the owning transport. */
    [[nodiscard]] bool isCancelled() const;

    /** The source map id for this filter execution. */
    std::string mapId_;

    /** The source layer id for this filter execution. */
    std::string layerId_;

    /** Optional catalog source selector/assertion; never part of subset identity. */
    std::optional<std::string> sourceId_;

    /** Requested output/source tile IDs, retained in client order. */
    std::vector<TileId> tileIds_;

    /** Source tile IDs that should be scheduled first. */
    std::set<TileId> priorityTileIds_;

    /** Ordered channel bundle, scalar bindings, and transport identity. */
    FeatureLayerFilterRequest filter_;

    /** Per-output delivery versions, used when one subscription renews tiles independently. */
    std::map<TileId, uint64_t> deliveryEpochs_;

    /** Optional exact relation roots grouped by their requested origin tile. */
    std::vector<FeatureLayerFilterRoot> exactRoots_;

    /** Callback for each emitted TileSubsetLayer. */
    template <class Fun>
    FeatureLayerFilterTilesRequest& onFilterResult(Fun&& callback)
    {
        onFilterResult_ = std::forward<Fun>(callback);
        return *this;
    }

    /** Callback for JSON status/progress updates. */
    template <class Fun>
    FeatureLayerFilterTilesRequest& onStatus(Fun&& callback)
    {
        onStatus_ = std::forward<Fun>(callback);
        return *this;
    }

    /** Callback fired once the request reaches a terminal state. */
    std::function<void(RequestStatus)> onDone_;

protected:
    virtual void notifyResult(TileSubsetLayer::Ptr);
    void notifyProgress(nlohmann::json const& status);
    void setStatus(RequestStatus s);
    void notifyStatus();
    void cancel();

private:
    std::function<void(TileSubsetLayer::Ptr)> onFilterResult_;
    std::function<void(nlohmann::json const&)> onStatus_;
    std::mutex childRequestsMutex_;
    std::vector<LayerTilesRequest::Ptr> childRequests_;
    std::atomic_bool cancelled_{false};
    std::mutex statusMutex_;
    std::condition_variable statusConditionVariable_;
    std::atomic<RequestStatus> status_ = RequestStatus::Open;
};

/**
 * Class which serves to unify multiple data sources for multiple maps,
 * and a cache which may store/restore the output of any of these sources.
 * The service runs tile loading and derived model work on one homogeneous,
 * bounded worker pool. Each primary datasource's maxParallelJobs_ remains an
 * independent concurrency limit; add-ons execute within their primary tile
 * jobs without reserving another worker.
 */
class Service
{
public:
    using DataSourceCatalogCallback = std::function<void(DataSourceCatalogChange const&)>;

    class DataSourceCatalogSubscription
    {
    public:
        ~DataSourceCatalogSubscription();
        DataSourceCatalogSubscription(DataSourceCatalogSubscription const&) = delete;
        DataSourceCatalogSubscription& operator=(DataSourceCatalogSubscription const&) = delete;
        DataSourceCatalogSubscription(DataSourceCatalogSubscription&&) noexcept;
        DataSourceCatalogSubscription& operator=(DataSourceCatalogSubscription&&) noexcept;

    private:
        DataSourceCatalogSubscription(Service* service, uint64_t id);
        Service* service_ = nullptr;
        uint64_t id_ = 0;

        friend class Service;
    };

    /**
     * Choose the default global worker cap from the available CPU count.
     *
     * The floor keeps slow or blocking datasources from starving unrelated
     * CPU work, while the ceiling bounds thread-stack and scheduler overhead.
     */
    [[nodiscard]] static size_t defaultWorkerCount() noexcept;

    /**
     * Construct a service with a shared Cache instance. Note: The Cache must not
     * be null. For a simple default cache implementation, you can use the
     * MemCache.
     * @param cache Cache instance to use.
     * @param useDataSourceConfig Instruct this service instance to makeDataSource its datasource
     *  backends based on a subscription to the YAML datasource config file.
     * @param defaultTtl Default time-to-live for tiles returned by the service. May be
     *  overridden by datasource or tile-specific TTL.
     * @param workerCount Maximum number of tile-loading and derived-evaluation
     *  jobs that may execute concurrently across the whole service.
     */
    explicit Service(
        Cache::Ptr cache = std::make_shared<MemCache>(),
        bool useDataSourceConfig = false,
        std::optional<std::chrono::milliseconds> defaultTtl = std::chrono::milliseconds{0},
        size_t workerCount = defaultWorkerCount());

    /** Destructor. Stops the global worker pool and datasource construction threads. */
    ~Service();

    /**
     * Add a data source. Incoming and queued requests for the data source will
     * become eligible for the global worker pool. Note, that the map layer versions for all layers
     * of the given source must be compatible with present one's, if existing.
     *
     * Thread safety: This method should not be called in parallel with itself or remove().
     */
    void add(DataSource::Ptr const& dataSource);

    /**
     * Remove a data source from the service. Requests for data which
     * can only be satisfied by the given source will not be processed anymore.
     * TODO: Any such ongoing requests should be forcefully marked as done.
     *
     * Thread safety: This method should not be called in parallel with itself or add().
     */
    void remove(DataSource::Ptr const& dataSource);

    /**
     * Request some map data tiles. If the requested map+layer
     * combination is available, will schedule a job to retrieve
     * the tiles. A request object should only ever be passed
     * to one service. Otherwise, there is undefined behavior.
     * @return false if the requested map+layer is not available
     * from any connected DataSource, true otherwise.
     */
    bool request(
        std::vector<LayerTilesRequest::Ptr> const& requests,
        std::optional<AuthHeaders> const& clientHeaders = {});

    /**
     * Request server-side filtering over source feature tiles.
     *
     * The returned binary chunks are TileSubsetLayer instances produced
     * via FeatureLayerFilterTilesRequest::onFilterResult.
     */
    bool request(
        FeatureLayerFilterTilesRequest::Ptr const& request,
        std::optional<AuthHeaders> const& clientHeaders = {});

    /** Request multiple server-side filter bundles. */
    bool request(
        std::vector<FeatureLayerFilterTilesRequest::Ptr> const& requests,
        std::optional<AuthHeaders> const& clientHeaders = {});

    /**
     * Load the source feature tile through the ordinary scheduler/cache path,
     * then obtain its separately transferred named attachment.
     */
    AttachmentResult attachment(
        AttachmentRequest const& request,
        std::optional<AuthHeaders> const& clientHeaders = {});

    /**
     * Asynchronous attachment form used by non-blocking transports.
     * The callback is invoked exactly once with a terminal result.
     */
    void requestAttachment(
        AttachmentRequest request,
        std::function<void(AttachmentResult)> callback,
        std::optional<AuthHeaders> const& clientHeaders = {});

    /**
     * Trigger queries to all connected data sources to check
     * for a feature matching the given typeId and idParts.
     * Returns the list of MapTileKeys received from data sources.
     */
    std::vector<LocateResponse> locate(LocateRequest const& req);

    /**
     * Abort the given request. The request will be removed from
     * the processing queue, and forcefully marked as done.
     */
    void abort(LayerTilesRequest::Ptr const& r);

    /** Abort a server-side filter request. */
    void abort(FeatureLayerFilterTilesRequest::Ptr const& r);

    /** DataSourceInfo for all data sources which have been added to this Service. */
    std::vector<DataSourceInfo> info(std::optional<AuthHeaders> const& clientHeaders = {});

    /**
     * Ordered datasource catalog including initializing/failed config entries.
     * Set `waitUntilReloadDone` when serving legacy callers that expect `/sources`
     * to block until the current config-backed datasource reload has completed.
     */
    [[nodiscard]] DataSourceCatalogSnapshot sourceCatalog(
        std::optional<AuthHeaders> const& clientHeaders = {},
        bool waitUntilReloadDone = false) const;

    /** Current datasource catalog revision without cloning catalog metadata. */
    [[nodiscard]] uint64_t sourceCatalogRevision() const;

    /** True if an interactive client may receive the optional source delta in this catalog change.
     */
    [[nodiscard]] bool isSourceCatalogChangeVisible(
        DataSourceCatalogChange const& change,
        std::optional<AuthHeaders> const& clientHeaders = {}) const;

    /** Subscribe to datasource catalog changes such as config reloads or startup status
     * transitions. */
    [[nodiscard]] DataSourceCatalogSubscription
    subscribeToSourceCatalogChanges(DataSourceCatalogCallback callback);

    /**
     * Checks if any DataSource can serve the requested map+layer combination,
     * then returns Success. Otherwise returns NoDataSource, or Unauthorized
     * if clientHeaders is passed and does not validate against the datasource's
     * auth requirements.
     */
    [[nodiscard]] RequestStatus hasLayerAndCanAccess(
        std::string const& mapId,
        std::string const& layerId,
        std::optional<AuthHeaders> const& clientHeaders) const;

    /**
     * Resolve request context (status and layer type) for one map+layer.
     */
    [[nodiscard]] LayerRequestContext resolveLayerRequest(
        std::string const& mapId,
        std::string const& layerId,
        std::optional<AuthHeaders> const& clientHeaders,
        std::optional<std::string> const& sourceId = {}) const;

    /**
     * Abort stale work and clear every cached tile layer for one ready primary map.
     * If headers are supplied, the map's ordinary datasource authorization also applies.
     * Returns false when no matching authorized primary map is currently ready.
     */
    [[nodiscard]] bool resetMapCache(
        std::string const& mapId,
        std::optional<AuthHeaders> const& clientHeaders = {});

    /**
     * Get Statistics about the operation of this service.
     * Returns the following values:
     * - `workers`: Configured and currently running global worker counts.
     * - `datasources`: Number of active data sources.
     * - `active-requests`: Number of in-flight requests.
     * - `filter-evaluation`: Queued and running derived filter jobs.
     */
    [[nodiscard]] nlohmann::json getStatistics() const;

    /**
     * Variant of getStatistics() with optional expensive analyses:
     * - includeCachedFeatureTreeBytes: Parse cached feature tiles and aggregate
     *   detailed subtree sizes.
     * - includeTileSizeDistribution: Build cached feature-tile size histogram
     *   and percentiles.
     */
    [[nodiscard]] nlohmann::json
    getStatistics(bool includeCachedFeatureTreeBytes, bool includeTileSizeDistribution) const;

    /**
     * Snapshot process controls and capacity-based mapget ownership estimates.
     *
     * Values below `mapget` are additive lower bounds unless explicitly marked
     * inclusive. Datasource values are cooperative self-reported estimates.
     */
    [[nodiscard]] nlohmann::json getMemoryStatistics() const;

    /** Get the Cache which this service was constructed with. */
    [[nodiscard]] Cache::Ptr cache();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class detail::FilterRequestExecution;
    friend class detail::LocateRequestExecution;
};

}  // namespace mapget
