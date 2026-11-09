#pragma once

#include "cache.h"
#include "config.h"
#include "datasource.h"
#include "mapget/model/featurelayer-filter.h"
#include "mapget/model/incoming-relations.h"
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
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace mapget
{

namespace detail
{
class AttachmentRequestExecution;
class FilterRequestExecution;
class IncomingRequestExecution;
class LocateRequestExecution;
class ServiceScheduler;
class TileLoadJob;
}  // namespace detail

/** Admission/terminal state; Success means delivery completed, even if a tile carries an error. */
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

    /** Recovery remains active through both the delay and the next construction attempt. */
    bool retrying = false;

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

    /** Explicit recovery state, independent of the construction lifecycle status. */
    bool retrying = false;

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

/** Authorized layer selection used before a request enters the scheduler. */
struct LayerRequestContext
{
    RequestStatus status_ = RequestStatus::NoDataSource;
    NoDataSourceReason noDataSourceReason_ = NoDataSourceReason::MissingMapOrLayer;
    LayerType layerType_ = LayerType::Features;
    PartitionKind partitionKind_ = PartitionKind::Tile;
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
 * One request for tile or object partitions in a single map layer. Configure
 * IDs, restrictions and callbacks before submission. Result callbacks run on
 * delivering workers and may overlap for different partitions; arrival order
 * can differ from the requested scheduling order.
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
    LayerTilesRequest(std::string mapId, std::string layerId, std::vector<PartitionId> tiles);

    /** Construct a request with foreground tile IDs prioritized for scheduling. */
    LayerTilesRequest(
        std::string mapId,
        std::string layerId,
        std::vector<PartitionId> tiles,
        std::vector<PartitionId> const& priorityPartitionIds);

    /** Adapt an existing tile-datasource request to generic partition scheduling. */
    template <class T>
    requires std::is_same_v<T, TileId>
    LayerTilesRequest(std::string map, std::string layer, std::vector<T> const& tiles)
        : LayerTilesRequest(
              std::move(map),
              std::move(layer),
              std::vector<PartitionId>(tiles.begin(), tiles.end()))
    {
    }

    /** Get the current status of the request. */
    RequestStatus getStatus() const;

    /** Block until a terminal status; do not call from a service worker awaiting its own pool. */
    void wait();

    /** Check whether the request is done or still running. */
    bool isDone() const;

    /**
     * Attach a shared gate controlling admission of new tile jobs.
     *
     * Set the gate before submitting the request. A false value pauses only
     * jobs originating from this request; running or shared jobs still publish.
     */
    LayerTilesRequest& setWorkAdmissionGate(std::shared_ptr<std::atomic_bool const> gate);

    /** The map id for which this request is dedicated. */
    std::string mapId_;

    /** The map layer id for which this request is dedicated. */
    std::string layerId_;

    /** Optional catalog source selector/assertion; never part of MapPartitionKey. */
    std::optional<std::string> sourceId_;

    /** Tile or object partition IDs in scheduling order; callbacks may arrive out of order. */
    std::vector<PartitionId> tileIds_;

    /**
     * Tile IDs within this request that should be scheduled before regular
     * tiles. This is a scheduling hint only; it does not add tiles to the
     * request by itself.
     */
    std::set<PartitionId> priorityPartitionIds_;

    /**
     * Optional canonical feature IDs to retain in each returned feature tile.
     * Source loading and caching remain tile-granular; restriction is applied
     * only to this request's immutable response value.
     */
    std::map<PartitionId, std::vector<std::string>> featureIdsByTile_;

    /**
     * The callback function which is called when all tiles have been processed.
     */
    std::function<void(RequestStatus)> onDone_;

    /** Register delivery of each feature partition; onDone_ reports request completion. */
    template <class Fun>
    LayerTilesRequest& onFeatureLayer(Fun&& callback)
    {
        onFeatureLayer_ = std::forward<Fun>(callback);
        return *this;
    }

    /** Register delivery of each source-data partition on its delivering worker. */
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
    virtual void notifyResult(PartitionLayer::Ptr);
    void notifyLoadState(MapPartitionKey const& key, PartitionLayer::LoadState state) const;
    void setStatus(RequestStatus s);
    void notifyStatus();
    nlohmann::json toJson();

private:
    /** Return whether this request may originate another tile job. */
    [[nodiscard]] bool admitsNewWork() const;

    /** Resolve tile IDs into concrete keys while preserving priority/order. */
    void prepareResolvedLayer(LayerType layerType);

    /** Restrict output membership and report live, complete, and changed state. */
    [[nodiscard]] std::tuple<bool, bool, bool>
    retainOutputPartitionIds(std::set<PartitionId> const& retained);

    /**
     * The callback functions which are called when a result tile is available.
     */
    std::function<void(PartitionFeatureLayer::Ptr)> onFeatureLayer_;
    std::function<void(PartitionSourceDataLayer::Ptr)> onSourceDataLayer_;
    std::function<void(MapPartitionKey const&, PartitionLayer::LoadState)> onLoadStateChanged_;
    std::shared_ptr<std::atomic_bool const> workAdmissionGate_;

    // So the service can track which tile index from resolvedTileKeys_
    // is next in line to be processed.
    size_t nextTileIndex_ = 0;

    // Resolved tile keys in scheduling order.
    std::vector<MapPartitionKey> resolvedTileKeys_;

    // Track which resolved tile keys still need to be scheduled/served.
    std::set<MapPartitionKey> tileKeysNotStarted_;

    // Output membership is separate from scheduler state so a callback copied
    // just before pruning can still be rejected safely.
    std::set<MapPartitionKey> liveTileKeys_;
    std::set<MapPartitionKey> claimedTileKeys_;
    std::set<MapPartitionKey> completedTileKeys_;

    // Mutex/condition variable for reading/setting request status.
    mutable std::mutex statusMutex_;
    std::condition_variable statusConditionVariable_;
    std::atomic<RequestStatus> status_ = RequestStatus::Open;
};

/**
 * Client request for server-side multi-channel feature filtering.
 *
 * The service loads source feature tiles through the normal scheduler/cache
 * path, then runs SIMFIL evaluation on the worker that delivered each tile.
 */
class FeatureLayerFilterTilesRequest
{
    friend class Service;
    friend class HttpClient;
    friend class detail::FilterRequestExecution;
    friend class detail::IncomingRequestExecution;
    friend class detail::ServiceScheduler;

public:
    using Ptr = std::shared_ptr<FeatureLayerFilterTilesRequest>;

    /** Construct a filter request over a set of source feature tile IDs. */
    FeatureLayerFilterTilesRequest(
        std::string mapId,
        std::string layerId,
        std::vector<PartitionId> tiles,
        FeatureLayerFilterRequest filter);

    /** Construct a filter request with foreground tile IDs prioritized for source loads. */
    FeatureLayerFilterTilesRequest(
        std::string mapId,
        std::string layerId,
        std::vector<PartitionId> tiles,
        FeatureLayerFilterRequest filter,
        std::vector<PartitionId> const& priorityPartitionIds);

    /** Adapt tile-only callers without introducing a second request implementation. */
    template <class T>
    requires std::is_same_v<T, TileId> FeatureLayerFilterTilesRequest(
        std::string map,
        std::string layer,
        std::vector<T> const& tiles,
        FeatureLayerFilterRequest filter)
        : FeatureLayerFilterTilesRequest(
              std::move(map),
              std::move(layer),
              std::vector<PartitionId>(tiles.begin(), tiles.end()),
              std::move(filter))
    {
    }

    /** Get the current status of the filter request. */
    RequestStatus getStatus();

    /** Wait for the request to reach a terminal state. */
    void wait();

    /** Check whether the request is done or still running. */
    bool isDone();

    /** Check whether the request has been cancelled by the owning transport. */
    [[nodiscard]] bool isCancelled() const;

    /**
     * Attach a shared gate controlling admission of new source-tile jobs.
     *
     * Set the gate before submitting the request. It is inherited by source
     * and relation-target child requests; already running work still completes.
     */
    FeatureLayerFilterTilesRequest&
    setWorkAdmissionGate(std::shared_ptr<std::atomic_bool const> gate);

    /** The source map id for this filter execution. */
    std::string mapId_;

    /** The source layer id for this filter execution. */
    std::string layerId_;

    /** Optional catalog source selector/assertion; never part of subset identity. */
    std::optional<std::string> sourceId_;

    /** Requested output/source tile IDs, retained in client order. */
    std::vector<PartitionId> tileIds_;

    /** Source tile IDs that should be scheduled first. */
    std::set<PartitionId> priorityPartitionIds_;

    /** Ordered channel bundle, scalar bindings, and transport identity. */
    FeatureLayerFilterRequest filter_;

    /** Optional exact relation roots grouped by their requested origin tile. */
    std::vector<FeatureLayerFilterRoot> exactRoots_;

    /**
     * Receive each emitted subset; callbacks for different outputs may overlap.
     * Local service success waits for these callbacks to return. Cancellation
     * and failure may finish the request while other callbacks are still running.
     */
    template <class Fun>
    FeatureLayerFilterTilesRequest& onFilterResult(Fun&& callback)
    {
        onFilterResult_ = std::forward<Fun>(callback);
        return *this;
    }

    /** Receive one immutable snapshot declared by an incoming inspection manifest. */
    template <class Fun> FeatureLayerFilterTilesRequest& onFeatureSnapshot(Fun&& callback)
    {
        onFeatureSnapshot_ = std::forward<Fun>(callback);
        return *this;
    }

    /** Receive the backend-assigned incoming collection after its snapshot frames. */
    template <class Fun> FeatureLayerFilterTilesRequest& onIncomingRelations(Fun&& callback)
    {
        onIncomingRelations_ = std::forward<Fun>(callback);
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
    virtual void notifyResult(PartitionSubsetLayer::Ptr);

    /** Deliver incoming payloads while the request is active. */
    void notifyFeatureSnapshot(PartitionFeatureLayer::Ptr const&);

    /** Deliver a typed incoming manifest while the request is active. */
    void notifyIncomingRelations(IncomingRelationCollection const&);
    void notifyProgress(nlohmann::json const& status);
    void setStatus(RequestStatus s);
    void notifyStatus();
    void cancel();

private:
    /** Restrict live output membership and report whether anything changed. */
    [[nodiscard]] std::pair<bool, bool>
    retainOutputPartitionIds(std::set<PartitionId> const& retained);

    /** Return whether a result tile still belongs to this request. */
    [[nodiscard]] bool acceptsOutputTile(PartitionId tileId) const;

    std::function<void(PartitionSubsetLayer::Ptr)> onFilterResult_;
    std::function<void(PartitionFeatureLayer::Ptr const&)> onFeatureSnapshot_;
    std::function<void(IncomingRelationCollection const&)> onIncomingRelations_;
    std::function<void(nlohmann::json const&)> onStatus_;
    std::shared_ptr<std::atomic_bool const> workAdmissionGate_;
    mutable std::mutex outputMembershipMutex_;
    std::set<PartitionId> liveOutputPartitionIds_;
    std::weak_ptr<detail::FilterRequestExecution> execution_;
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

    /** RAII callback registration; the Service must outlive this subscription. */
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

    /** Default upper bound for failed tile lifetimes, independent of successful tile TTLs. */
    static constexpr auto DEFAULT_ERROR_TILE_TTL = std::chrono::seconds{5};

    /**
     * Recheck successful empty source tiles/objects hourly unless their producer requests sooner.
     */
    static constexpr auto DEFAULT_EMPTY_TILE_TTL = std::chrono::seconds{3600};

    /**
     * Construct a service with a shared Cache instance. Note: The Cache must not
     * be null. For a simple default cache implementation, you can use the
     * MemCache.
     * @param cache Cache instance to use.
     * @param useDataSourceConfig Subscribe to YAML configuration changes and construct
     *  the configured datasources asynchronously.
     * @param defaultTtl Default time-to-live for tiles returned by the service. May be
     *  overridden by datasource or tile-specific TTL.
     * @param workerCount Maximum number of source-tile jobs that may load and
     *  process attached consumers concurrently across the whole service.
     * @param errorTileTtl Positive upper bound for error-tile lifetimes. A shorter
     *  positive tile-specific TTL takes precedence; successful tiles are unaffected.
     * @param emptyTileTtl Positive upper bound for successful source layers with no
     *  roots, including add-ons. Preserves shorter positive tile-specific TTLs;
     *  does not apply to empty filtered results from nonempty source layers.
     */
    explicit Service(
        Cache::Ptr cache = std::make_shared<MemCache>(),
        bool useDataSourceConfig = false,
        std::optional<std::chrono::milliseconds> defaultTtl = std::chrono::milliseconds{0},
        size_t workerCount = defaultWorkerCount(),
        std::chrono::milliseconds errorTileTtl = DEFAULT_ERROR_TILE_TTL,
        std::chrono::milliseconds emptyTileTtl = DEFAULT_EMPTY_TILE_TTL);

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
     * Withdraw a datasource and invalidate affected map caches and outstanding
     * work. Waiting requests become Aborted; a delivery already committed by a
     * worker can finish. Running datasource I/O stops only if it cooperates
     * with cancellation.
     *
     * Thread safety: This method should not be called in parallel with itself or add().
     */
    void remove(DataSource::Ptr const& dataSource);

    /**
     * Request map data for a bundle of tile or object partitions, admitting it
     * against the current source registrations.
     * A missing or unauthorized member rejects the bundle and aborts its otherwise
     * open siblings. False reports rejected admission, including shutdown/reload.
     * Configure each request once and submit it to only one service.
     * Omitted authorization means trusted in-process access; network adapters must
     * supply an explicit ClientAuthorization, even for anonymous callers.
     */
    bool request(
        std::vector<LayerTilesRequest::Ptr> const& requests,
        std::optional<ClientAuthorization> const& clientAuthorization = {});

    /**
     * Request server-side filtering over source feature tiles.
     *
     * Delivers PartitionSubsetLayer models through onFilterResult; transport
     * adapters serialize them. A false return reports failed admission.
     */
    bool request(
        FeatureLayerFilterTilesRequest::Ptr const& request,
        std::optional<ClientAuthorization> const& clientAuthorization = {});

    /** Submit each filter bundle independently; false means at least one was rejected. */
    bool request(
        std::vector<FeatureLayerFilterTilesRequest::Ptr> const& requests,
        std::optional<ClientAuthorization> const& clientAuthorization = {});

    /**
     * Load the source feature tile through the ordinary scheduler/cache path,
     * then obtain its separately transferred named attachment. This form blocks;
     * use requestAttachment from callbacks or event loops that must keep running.
     */
    AttachmentResult attachment(
        AttachmentRequest const& request,
        std::optional<ClientAuthorization> const& clientAuthorization = {});

    /**
     * Asynchronous attachment form used by non-blocking transports.
     * The callback is invoked exactly once with a terminal result.
     */
    void requestAttachment(
        AttachmentRequest request,
        std::function<void(AttachmentResult)> callback,
        std::optional<ClientAuthorization> const& clientAuthorization = {});

    /**
     * Locate a feature matching the request's typeId and idParts.
     * Ask authorized providers for cheap candidates, load those partitions through
     * the normal scheduler, then evaluate their selectors to return canonical IDs
     * and owning partition keys. Results are deduplicated and sorted. This call
     * blocks for tile work and must not run inside the service worker pool.
     */
    std::vector<LocateResponse> locate(
        LocateRequest const& req,
        std::optional<ClientAuthorization> const& authorization = {});

    /**
     * Discover associations without loading payloads, using the shared worker/permit budget.
     * A nonempty callback receives one result, possibly before this call returns: validation
     * or admission failures complete inline, accepted queries on a worker, and queued
     * cancellations on the invalidation/shutdown thread. Callbacks run outside the scheduler
     * mutex; keep them short, thread-safe, nonthrowing, and independent of the submitting stack.
     * A worker retains its datasource permit through the callback, so do not wait there for
     * further service work. An empty callback submits nothing.
     * There is no per-query cancellation handle. Map invalidation/shutdown fails queued work;
     * already running datasource calls finish normally and may still deliver results.
     */
    void discoverObjects(
        ObjectDiscoveryRequest request,
        std::function<void(ObjectDiscoveryResult)> callback,
        std::optional<ClientAuthorization> const& clientAuthorization = {});

    /**
     * Abort the given request. The request will be removed from
     * the processing queue, and forcefully marked as done.
     */
    void abort(LayerTilesRequest::Ptr const& r);

    /** Abort a server-side filter request. */
    void abort(FeatureLayerFilterTilesRequest::Ptr const& r);

    /**
     * Keep only the listed outputs of a live ordinary request.
     *
     * Work already running for removed outputs may still populate the shared
     * cache, but it can no longer invoke this request's result callback.
     */
    void retainOutputs(
        LayerTilesRequest::Ptr const& request,
        std::set<PartitionId> const& retainedPartitionIds);

    /**
     * Keep only the listed outputs of a live filter request.
     *
     * Source contributions shared by retained outputs remain reusable while
     * pruned output models and relation dependencies are released promptly.
     */
    void retainOutputs(
        FeatureLayerFilterTilesRequest::Ptr const& request,
        std::set<PartitionId> const& retainedPartitionIds);

    /** Wake workers after an external request-admission gate may have opened. */
    void notifyWorkAvailable();

    /**
     * Run bounded metadata/query preparation on the homogeneous worker pool.
     * Tasks must not wait for other service work. False means admission failed;
     * accepted tasks receive false on shutdown instead of being silently discarded.
     */
    [[nodiscard]] bool scheduleTask(std::function<void(bool)> task);

    /** DataSourceInfo for all data sources which have been added to this Service. */
    std::vector<DataSourceInfo> info(std::optional<ClientAuthorization> const& clientAuthorization = {});

    /**
     * Return the immutable schema currently bound to an authorized, ready source.
     * A supplied sourceId and schemaId must match exactly; an empty sourceId permits any
     * current authorized provider of the exact artifact. Missing, unauthorized or removed sources
     * return null; a shared content-cache hit never bypasses the source check.
     * This only acquires an initialized owner: no schema construction or network I/O.
     */
    [[nodiscard]] std::shared_ptr<MapSchema const> getSchema(
        std::string_view sourceId,
        std::string_view schemaId,
        std::optional<ClientAuthorization> const& clientAuthorization = {}) const;

    /**
     * Inventory authorized, currently registered schema artifacts, deduplicated by content ID.
     * Contains descriptive formats, layer roots, cheap retained-capacity statistics and
     * authorized consumers, never expanded domain graphs or tile data. An empty sourceId
     * in getSchema resolves an inventory artifact through any current authorized provider.
     * includeSource filters authorized sourceId/mapId pairs before visiting their owners;
     * rejected providers contribute no consumers, ownership counts or retained bytes.
     * include filters the remaining owners before allocating their inventory rows. checkpoint
     * runs before snapshotting and throughout source/layer/type/consumer traversal; it may
     * throw to abort without returning a partial inventory. No callback runs under the
     * registry lock. Snapshot changes throw resource_unavailable_try_again for a fresh retry.
     * Callbacks bound traversal, not individual authorization calls, snapshot copies or cached
     * metadata copies; callers must separately bound the serialized response size.
     */
    [[nodiscard]] nlohmann::json schemaInventory(
        std::optional<ClientAuthorization> const& clientAuthorization = {},
        std::function<bool(MapSchema const&)> const& include = {},
        std::function<void()> const& checkpoint = {},
        std::function<bool(std::string_view sourceId, std::string_view mapId)> const&
            includeSource = {}) const;

    /**
     * Ordered datasource catalog including initializing/failed config entries.
     * Set `waitUntilReloadDone` when serving legacy callers that expect `/sources`
     * to block until the current config-backed datasource reload has completed.
     */
    [[nodiscard]] DataSourceCatalogSnapshot sourceCatalog(
        std::optional<ClientAuthorization> const& clientAuthorization = {},
        bool waitUntilReloadDone = false) const;

    /** Current datasource catalog revision without cloning catalog metadata. */
    [[nodiscard]] uint64_t sourceCatalogRevision() const;

    /** True if an interactive client may receive the optional source delta in this catalog change.
     */
    [[nodiscard]] bool isSourceCatalogChangeVisible(
        DataSourceCatalogChange const& change,
        std::optional<ClientAuthorization> const& clientAuthorization = {}) const;

    /** Subscribe to datasource catalog changes such as config reloads or startup status
     * transitions. */
    [[nodiscard]] DataSourceCatalogSubscription
    subscribeToSourceCatalogChanges(DataSourceCatalogCallback callback);

    /**
     * Checks if any DataSource can serve the requested map+layer combination,
     * then returns Success. Otherwise returns NoDataSource, or Unauthorized
     * if clientAuthorization is passed and does not validate against the datasource's
     * auth requirements.
     */
    [[nodiscard]] RequestStatus hasLayerAndCanAccess(
        std::string const& mapId,
        std::string const& layerId,
        std::optional<ClientAuthorization> const& clientAuthorization) const;

    /**
     * Resolve request context (status and layer type) for one map+layer.
     */
    [[nodiscard]] LayerRequestContext resolveLayerRequest(
        std::string const& mapId,
        std::string const& layerId,
        std::optional<ClientAuthorization> const& clientAuthorization,
        std::optional<std::string> const& sourceId = {}) const;

    /**
     * Abort stale work and clear every cached tile layer for one ready primary map.
     * If authorization is supplied, both header and verified-claim gates apply.
     * Returns false when no matching authorized primary map is currently ready.
     */
    [[nodiscard]] bool resetMapCache(
        std::string const& mapId,
        std::optional<ClientAuthorization> const& clientAuthorization = {});

    /**
     * Get Statistics about the operation of this service.
     * Returns the following values:
     * - `workers`: Configured and currently running global worker counts.
     * - `datasources`: Number of active data sources.
     * - `active-requests`: Number of in-flight requests.
     * - `queued-tile-work-items`: Request tile keys not yet admitted to a worker.
     * - `in-flight-tile-jobs`: Coalesced source tiles awaiting cache/backend completion.
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
    friend class detail::IncomingRequestExecution;
    friend class detail::LocateRequestExecution;
};

}  // namespace mapget
