#pragma once

#include "cache.h"
#include "datasource.h"
#include "mapget/model/featurelayer-search.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/layer.h"
#include "memcache.h"

#include <condition_variable>
#include <mutex>
#include <utility>
#include <chrono>
#include <set>
#include <vector>
#include <atomic>

namespace mapget
{

enum class RequestStatus {
    Open = 0x0,
    Success = 0x1, /** The request has been fully satisfied. */
    NoDataSource = 0x2, /** No data source could provide the requested map + layer. */
    Unauthorized = 0x3, /** The user is not authorized to access the requested data source. */
    Aborted = 0x4 /** Canceled, e.g. because a bundled request cannot be fulfilled. */
};

enum class NoDataSourceReason {
    None = 0x0,
    EmptySources = 0x1,
    AllSourcesDisabled = 0x2,
    DatasourceInitializationFailed = 0x3,
    MissingMapOrLayer = 0x4,
    NoConfig = 0x5,
};

struct LayerRequestContext {
    RequestStatus status_ = RequestStatus::NoDataSource;
    NoDataSourceReason noDataSourceReason_ = NoDataSourceReason::MissingMapOrLayer;
    LayerType layerType_ = LayerType::Features;
    uint32_t stages_ = 1;
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

public:
    using Ptr = std::shared_ptr<LayerTilesRequest>;

    /** Construct a request for tiles with the relevant parameters. */
    LayerTilesRequest(
        std::string mapId,
        std::string layerId,
        std::vector<TileId> tiles);

    /** Construct an unstaged request with foreground tile IDs prioritized for scheduling. */
    LayerTilesRequest(
        std::string mapId,
        std::string layerId,
        std::vector<TileId> tiles,
        std::vector<TileId> const& priorityTileIds);

    /** Construct a staged request with tile IDs grouped by next missing stage. */
    LayerTilesRequest(
        std::string mapId,
        std::string layerId,
        std::vector<std::vector<TileId>> tileIdsByNextStage);

    /** Construct a staged request with foreground tile IDs prioritized for scheduling. */
    LayerTilesRequest(
        std::string mapId,
        std::string layerId,
        std::vector<std::vector<TileId>> tileIdsByNextStage,
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

    /**
     * The map tile IDs for this request, grouped by next missing stage.
     * Bucket index N means: send stage N and all higher stages for these IDs.
     */
    std::vector<std::vector<TileId>> tileIdsByNextStage_;

    /**
     * Tile IDs within this request that should be scheduled before regular
     * tiles. This is a scheduling hint only; it does not add tiles to the
     * request by itself.
     */
    std::set<TileId> priorityTileIds_;

    /**
     * True iff the request explicitly uses `tileIdsByNextStage`.
     * Legacy `tileIds` requests keep unstaged semantics and must not be
     * expanded into one backend request per advertised stage.
     */
    bool usesStageBuckets_ = false;

    /**
     * The callback function which is called when all tiles have been processed.
     */
    std::function<void(RequestStatus)> onDone_;

    /**
     * The callback function which is called when all tiles have been processed.
     */
    template <class Fun>
    LayerTilesRequest& onFeatureLayer(Fun&& callback) { onFeatureLayer_ = std::forward<Fun>(callback); return *this; }

    template <class Fun>
    LayerTilesRequest& onSourceDataLayer(Fun&& callback) { onSourceDataLayer_ = std::forward<Fun>(callback); return *this; }

    /**
     * Callback for per-tile load-state changes.
     */
    template <class Fun>
    LayerTilesRequest& onLayerLoadStateChanged(Fun&& callback) { onLoadStateChanged_ = std::forward<Fun>(callback); return *this; }

protected:
    virtual void notifyResult(TileLayer::Ptr);
    void notifyLoadState(MapTileKey const& key, TileLayer::LoadState state) const;
    void setStatus(RequestStatus s);
    void notifyStatus();
    nlohmann::json toJson();

private:
    /** Resolve staged tile IDs into concrete stage-qualified tile keys. */
    void prepareResolvedLayer(LayerType layerType, uint32_t stages);

    /**
     * The callback functions which are called when a result tile is available.
     */
    std::function<void(TileFeatureLayer::Ptr)> onFeatureLayer_;
    std::function<void(TileSourceDataLayer::Ptr)> onSourceDataLayer_;
    std::function<void(MapTileKey const&, TileLayer::LoadState)> onLoadStateChanged_;

    // So the service can track which tile index from resolvedTileKeys_
    // is next in line to be processed.
    size_t nextTileIndex_ = 0;

    // Resolved staged tile keys in scheduling order.
    std::vector<MapTileKey> resolvedTileKeys_;

    /**
     * Search needs complete staged tiles before it can evaluate a predicate.
     * This internal scheduling hint keeps normal staged frontend requests
     * stage-major, but lets search load all stages of one tile before moving
     * on to the next tile.
     */
    bool preferCompleteStagedTiles_ = false;

    // Track which resolved tile keys still need to be scheduled/served.
    std::set<MapTileKey> tileKeysNotStarted_;

    // So the requester can track how many results have been received.
    size_t resultCount_ = 0;

    // Mutex/condition variable for reading/setting request status.
    std::mutex statusMutex_;
    std::condition_variable statusConditionVariable_;
    std::atomic<RequestStatus> status_ = RequestStatus::Open;
};

/**
 * Client request for server-side search-as-map evaluation.
 *
 * The service loads the source feature tile stages through the normal tile
 * scheduler/cache path, assembles a transient full tile where needed, then
 * runs SIMFIL evaluation as a scheduled service job.
 */
class FeatureLayerSearchTilesRequest
{
    friend class Service;
    friend class HttpClient;

public:
    using Ptr = std::shared_ptr<FeatureLayerSearchTilesRequest>;

    /** Construct a search request over a set of source feature tile IDs. */
    FeatureLayerSearchTilesRequest(
        std::string mapId,
        std::string layerId,
        std::vector<TileId> tiles,
        FeatureLayerSearchRequest search);

    /** Construct a search request with foreground tile IDs prioritized for source loads. */
    FeatureLayerSearchTilesRequest(
        std::string mapId,
        std::string layerId,
        std::vector<TileId> tiles,
        FeatureLayerSearchRequest search,
        std::vector<TileId> const& priorityTileIds);

    /** Get the current status of the search request. */
    RequestStatus getStatus();

    /** Wait for the request to reach a terminal state. */
    void wait();

    /** Check whether the request is done or still running. */
    bool isDone();

    /** Check whether the request has been cancelled by the owning transport. */
    [[nodiscard]] bool isCancelled() const;

    /** The source map id for this search. */
    std::string mapId_;

    /** The source layer id for this search. */
    std::string layerId_;

    /** Source tile IDs to search. */
    std::vector<TileId> tileIds_;

    /** Source tile IDs that should be scheduled first. */
    std::set<TileId> priorityTileIds_;

    /** Search predicate, result-field expressions, and result identity. */
    FeatureLayerSearchRequest search_;

    /** Callback for each emitted TileSearchResultLayer. */
    template <class Fun>
    FeatureLayerSearchTilesRequest& onSearchResult(Fun&& callback)
    {
        onSearchResult_ = std::forward<Fun>(callback);
        return *this;
    }

    /** Callback for JSON status/progress updates. */
    template <class Fun>
    FeatureLayerSearchTilesRequest& onStatus(Fun&& callback)
    {
        onStatus_ = std::forward<Fun>(callback);
        return *this;
    }

    /** Callback fired once the request reaches a terminal state. */
    std::function<void(RequestStatus)> onDone_;

protected:
    virtual void notifyResult(TileSearchResultLayer::Ptr);
    void notifyProgress(nlohmann::json const& status);
    void setStatus(RequestStatus s);
    void notifyStatus();
    void cancel();

private:
    std::function<void(TileSearchResultLayer::Ptr)> onSearchResult_;
    std::function<void(nlohmann::json const&)> onStatus_;
    std::vector<LayerTilesRequest::Ptr> childRequests_;
    std::atomic_bool cancelled_{false};
    std::mutex statusMutex_;
    std::condition_variable statusConditionVariable_;
    std::atomic<RequestStatus> status_ = RequestStatus::Open;
};

/**
 * Class which serves to unify multiple data sources for multiple maps,
 * and a cache which may store/restore the output of any of these sources.
 * The service maintains a number of worker threads for each source, depending
 * on the source's maxParallelJobs_.
 */
class Service
{
public:
    /**
     * Construct a service with a shared Cache instance. Note: The Cache must not
     * be null. For a simple default cache implementation, you can use the
     * MemCache.
     * @param cache Cache instance to use.
     * @param useDataSourceConfig Instruct this service instance to makeDataSource its datasource
     *  backends based on a subscription to the YAML datasource config file.
     * @param defaultTtl Default time-to-live for tiles returned by the service. May be
     *  overridden by datasource or tile-specific TTL.
     */
    explicit Service(
        Cache::Ptr cache = std::make_shared<MemCache>(),
        bool useDataSourceConfig = false,
        std::optional<std::chrono::milliseconds> defaultTtl = std::chrono::milliseconds{0});

    /** Destructor. Stops all workers of the present data sources. */
    ~Service();

    /**
     * Add a data source. Worker threads will be launched as needed,
     * and incoming/present requests for the data source will start to be
     * processed. Note, that the map layer versions for all layers of the
     * given source must be compatible with present one's, if existing.
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
    bool request(std::vector<LayerTilesRequest::Ptr> const& requests, std::optional<AuthHeaders> const& clientHeaders = {});

    /**
     * Request server-side feature search over source feature tiles.
     *
     * The returned binary chunks are TileSearchResultLayer instances produced
     * via FeatureLayerSearchTilesRequest::onSearchResult.
     */
    bool request(FeatureLayerSearchTilesRequest::Ptr const& request, std::optional<AuthHeaders> const& clientHeaders = {});

    /** Request multiple server-side searches. */
    bool request(std::vector<FeatureLayerSearchTilesRequest::Ptr> const& requests, std::optional<AuthHeaders> const& clientHeaders = {});

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

    /** Abort a server-side search request. */
    void abort(FeatureLayerSearchTilesRequest::Ptr const& r);

    /** DataSourceInfo for all data sources which have been added to this Service. */
    std::vector<DataSourceInfo> info(std::optional<AuthHeaders> const& clientHeaders = {});

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
     * Resolve request context (status, layer type, stage count) for one map+layer.
     */
    [[nodiscard]] LayerRequestContext resolveLayerRequest(
        std::string const& mapId,
        std::string const& layerId,
        std::optional<AuthHeaders> const& clientHeaders) const;

    /**
     * Get Statistics about the operation of this service.
     * Returns the following values:
     * - `workers`: Number of active workers.
     * - `datasources`: Number of active data sources.
     * - `active-requests`: Number of in-flight requests.
     */
    [[nodiscard]] nlohmann::json getStatistics() const;

    /**
     * Variant of getStatistics() with optional expensive analyses:
     * - includeCachedFeatureTreeBytes: Parse cached feature tiles and aggregate
     *   detailed subtree sizes.
     * - includeTileSizeDistribution: Build cached feature-tile size histogram
     *   and percentiles.
     */
    [[nodiscard]] nlohmann::json getStatistics(
        bool includeCachedFeatureTreeBytes,
        bool includeTileSizeDistribution) const;

    /** Get the Cache which this service was constructed with. */
    [[nodiscard]] Cache::Ptr cache();

private:
    struct Impl;
    struct Worker;
    struct Controller;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mapget
