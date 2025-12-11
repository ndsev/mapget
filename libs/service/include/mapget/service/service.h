#pragma once

#include "cache.h"
#include "datasource.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/layer.h"
#include "memcache.h"

#include <condition_variable>
#include <mutex>
#include <utility>
#include <chrono>

namespace mapget
{

enum class RequestStatus {
    Open = 0x0,
    Success = 0x1, /** The request has been fully satisfied. */
    NoDataSource = 0x2, /** No data source could provide the requested map + layer. */
    Unauthorized = 0x3, /** The user is not authorized to access the requested data source. */
    Aborted = 0x4 /** Canceled, e.g. because a bundled request cannot be fulfilled. */
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
     * The map tile ids for which this request is dedicated.
     * Must not be empty. Result tiles will be processed in the given order.
     */
    std::vector<TileId> tiles_;

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

protected:
    virtual void notifyResult(TileLayer::Ptr);
    void setStatus(RequestStatus s);
    void notifyStatus();
    nlohmann::json toJson();

private:
    /**
     * The callback functions which are called when a result tile is available.
     */
    std::function<void(TileFeatureLayer::Ptr)> onFeatureLayer_;
    std::function<void(TileSourceDataLayer::Ptr)> onSourceDataLayer_;

    // So the service can track which tileId index from tiles_
    // is next in line to be processed.
    size_t nextTileIndex_ = 0;

    // So the requester can track how many results have been received.
    size_t resultCount_ = 0;

    // Mutex/condition variable for reading/setting request status.
    std::mutex statusMutex_;
    std::condition_variable statusConditionVariable_;
    RequestStatus status_ = RequestStatus::Open;
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
     * Get Statistics about the operation of this service.
     * Returns the following values:
     * - `workers`: Number of active workers.
     * - `datasources`: Number of active data sources.
     * - `active-requests`: Number of in-flight requests.
     */
    [[nodiscard]] nlohmann::json getStatistics() const;

    /** Get the Cache which this service was constructed with. */
    [[nodiscard]] Cache::Ptr cache();

private:
    struct Impl;
    struct Worker;
    struct Controller;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mapget
