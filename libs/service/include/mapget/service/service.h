#pragma once

#include "cache.h"
#include "datasource.h"
#include "memcache.h"

#include <condition_variable>
#include <mutex>

namespace mapget
{

enum RequestStatus {
    Open = 0x0,
    Done = 0x1, /** The request has been fully satisfied. */
    NoDataSource = 0x2 /** No data source could provide the requested map + layer. */
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

    /** Construct a request with the relevant parameters. */
    LayerTilesRequest(
        std::string mapId,
        std::string layerId,
        std::vector<TileId> tiles,
        std::function<void(TileFeatureLayer::Ptr)> onResult);

    /** Get the current status of the request. */
    RequestStatus getStatus();

    /** Wait for the request to be done. */
    void wait();

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
     * The callback function which is called when a result tile is available.
     */
    std::function<void(TileFeatureLayer::Ptr)> onResult_;

    /**
     * The callback function which is called when all tiles have been processed.
     */
    std::function<void()> onDone_;

protected:
    virtual void notifyResult(TileFeatureLayer::Ptr);
    void setStatus(RequestStatus s);
    void notifyStatus();
    nlohmann::json toJson();

private:
    // So the service can track which tileId index from tiles_
    // is next in line to be processed.
    size_t nextTileIndex_ = 0;

    // So the requester can track how many results have been received.
    size_t resultCount_ = 0;

    // Mutex/condition variable for reading/setting request status.
    std::mutex statusMutex_;
    std::condition_variable statusConditionVariable_;
    RequestStatus status_ = Open;
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
     */
    explicit Service(Cache::Ptr cache = std::make_shared<MemCache>());

    /** Destructor. Stops all workers of the present data sources. */
    ~Service();

    /**
     * Add a data source. Worker threads will be launched as needed,
     * and incoming/present requests for the data source will start to be
     * processed. Note, that the map layer versions for all layers of the
     * given source must be compatible with present one's, if existing.
     */
    void add(DataSource::Ptr const& dataSource);

    /**
     * Remove a data source from the service. Requests for data which
     * can only be satisfied by the given source will not be processed anymore.
     * TODO: Any such ongoing requests should be forcefully marked as done.
     */
    void remove(DataSource::Ptr const& dataSource);

    /**
     * Request some map data tiles. Will throw an exception if
     * there is no worker which is able to process the request.
     * Note: The same request object should only ever be passed
     *  to one service. Otherwise, there is undefined behavior.
     * @return The `r` parameter value is returned.
     */
    LayerTilesRequest::Ptr request(LayerTilesRequest::Ptr r);

    /**
     * Abort the given request. The request will be removed from
     * the processing queue, and forcefully marked as done.
     */
    void abort(LayerTilesRequest::Ptr const& r);

    /** DataSourceInfo for all data sources which have been added to this Service. */
    std::vector<DataSourceInfo> info();

    /** Checks if a DataSource can serve the requested map+layer combination. */
    bool canProcess(std::string const& mapId, std::string const& layerId);

    /** Get the Cache which this service was constructed with. */
    [[nodiscard]] Cache::Ptr cache();

private:
    struct Impl;
    struct Worker;
    struct Controller;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mapget
