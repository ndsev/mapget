#include "service.h"

#include "fmt/format.h"
#include "locate.h"
#include "config.h"
#include "mapget/log.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/info.h"
#include "mapget/model/layer.h"

#include <memory>
#include <optional>
#include <set>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <list>
#include <chrono>

#include "simfil/types.h"

namespace mapget
{

LayerTilesRequest::LayerTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<TileId> tiles)
    : mapId_(std::move(mapId)),
      layerId_(std::move(layerId)),
      tiles_(std::move(tiles))
{
    if (tiles_.empty()) {
        // An empty request is always set to success, but the client/service
        // is responsible for triggering notifyStatus() in that case.
        status_ = RequestStatus::Success;
    }
}

void LayerTilesRequest::notifyResult(TileLayer::Ptr r) {
    const auto type = r->layerInfo()->type_;
    switch (type) {
    case mapget::LayerType::Features:
        if (onFeatureLayer_)
            onFeatureLayer_(std::move(std::static_pointer_cast<mapget::TileFeatureLayer>(r)));
        break;
    case mapget::LayerType::SourceData:
        if (onSourceDataLayer_)
            onSourceDataLayer_(std::move(std::static_pointer_cast<mapget::TileSourceDataLayer>(r)));
        break;
    default:
        mapget::log().error(fmt::format("Unhandled layer type {}, no matching callback!", static_cast<int>(type)));
        break;
    }

    ++resultCount_;
    if (resultCount_ == tiles_.size()) {
        setStatus(RequestStatus::Success);
    }
}

void LayerTilesRequest::setStatus(RequestStatus s)
{
    {
        std::unique_lock statusLock(statusMutex_);
        this->status_ = s;
    }
    notifyStatus();
}

void LayerTilesRequest::notifyStatus()
{
    if (isDone() && onDone_) {
        // Run the final callback function.
        onDone_(this->status_);
    }
    statusConditionVariable_.notify_all();
}

void LayerTilesRequest::wait()
{
    std::unique_lock doneLock(statusMutex_);
    // Extra doneness check is to avoid infinite locking, e.g.
    // because empty requests were not considered by calling method.
    if (!isDone()) {
        statusConditionVariable_.wait(doneLock, [this]{ return isDone(); });
    }
}

nlohmann::json LayerTilesRequest::toJson()
{
    auto tileIds = nlohmann::json::array();
    for (auto const& tid : tiles_)
        tileIds.emplace_back(tid.value_);
    return nlohmann::json::object({
        {"mapId", mapId_},
        {"layerId", layerId_},
        {"tileIds", tileIds}
    });
}

RequestStatus LayerTilesRequest::getStatus()
{
    return this->status_;
}

bool LayerTilesRequest::isDone()
{
    return status_ != RequestStatus::Open;
}

struct Service::Controller
{
    struct Job {
        MapTileKey tileKey;
        LayerTilesRequest::Ptr request;
        std::optional<std::chrono::system_clock::time_point> cacheExpiredAt;
    };

    std::set<MapTileKey> jobsInProgress_;    // Set of jobs currently in progress
    Cache::Ptr cache_;                       // The cache for the service
    std::optional<std::chrono::milliseconds> defaultTtl_; // Default TTL applied when datasource does not override
    std::list<LayerTilesRequest::Ptr> requests_;       // List of requests currently being processed
    std::condition_variable jobsAvailable_;  // Condition variable to signal job availability
    std::mutex jobsMutex_;  // Mutex used with the jobsAvailable_ condition variable

    explicit Controller(Cache::Ptr cache, std::optional<std::chrono::milliseconds> defaultTtl)
        : cache_(std::move(cache)),
          defaultTtl_(std::move(defaultTtl))
    {
        if (!cache_)
            raise("Cache must not be null!");
    }

    std::optional<Job> nextJob(DataSourceInfo const& i)
    {
        // Workers call the nextJob function when they are free.
        // Note: For thread safety, jobsMutex_ must be held
        //  when calling this function.

        std::optional<Job> result;

        // Return next job, if available.
        bool cachedTilesServed = false;
        do {
            cachedTilesServed = false;
            for (auto reqIt = requests_.begin(); reqIt != requests_.end(); ++reqIt) {
                auto const& request = *reqIt;
                auto layerIt = i.layers_.find(request->layerId_);

                // Are there tiles left to be processed in the request?
                if (request->mapId_ == i.mapId_ && layerIt != i.layers_.end()) {
                    if (request->nextTileIndex_ >= request->tiles_.size()) {
                        continue;
                    }

                    // Create result wrapper object.
                    auto tileId = request->tiles_[request->nextTileIndex_++];
                    result = Job{MapTileKey(), request, std::nullopt};
                    result->tileKey.layer_ = layerIt->second->type_;
                    result->tileKey.mapId_ = request->mapId_;
                    result->tileKey.layerId_ = request->layerId_;
                    result->tileKey.tileId_ = tileId;

                    // Cache lookup.
                    auto cachedResult = cache_->getTileLayer(result->tileKey, i);
                    if (cachedResult.tile) {
                        log().debug("Serving cached tile: {}", result->tileKey.toString());
                        request->notifyResult(cachedResult.tile);
                        result.reset();
                        cachedTilesServed = true;
                        continue;
                    }
                    result->cacheExpiredAt = cachedResult.expiredAt;

                    if (jobsInProgress_.find(result->tileKey) != jobsInProgress_.end()) {
                        // Don't work on something that is already being worked on.
                        // Wait for the work to finish, then send the (hopefully cached) result.
                        log().debug("Delaying tile with job in progress: {}",
                                    result->tileKey.toString());
                        --request->nextTileIndex_;
                        result.reset();
                        continue;
                    }

                    // Enter into the jobs-in-progress set.
                    jobsInProgress_.insert(result->tileKey);

                    // Move this request to the end of the list, so others gain priority.
                    requests_.splice(requests_.end(), requests_, reqIt);

                    log().debug("Working on tile: {}", result->tileKey.toString());
                    break;
                }
            }
        }
        while (cachedTilesServed && !result);

        // Clean up done requests.
        requests_.remove_if([](auto&& r) {return r->nextTileIndex_ == r->tiles_.size(); });

        return result;
    }

    virtual void loadAddOnTiles(TileFeatureLayer::Ptr const& baseTile, DataSource& baseDataSource) = 0;
};

struct Service::Worker
{
    using Ptr = std::shared_ptr<Worker>;

    DataSource::Ptr dataSource_;   // Data source the worker is responsible for
    DataSourceInfo info_;          // Information about the data source
    std::atomic_bool shouldTerminate_ = false; // Flag indicating whether the worker thread should terminate
    Controller& controller_;       // Reference to Service::Impl which owns this worker
    std::thread thread_;           // The worker thread

    Worker(
        DataSource::Ptr dataSource,
        DataSourceInfo info,
        Controller& controller)
        : dataSource_(std::move(dataSource)),
          info_(std::move(info)),
          controller_(controller)
    {
        thread_ = std::thread([this]{while (work()) {}});
    }

    bool work()
    {
        std::optional<Controller::Job> nextJob;

        {
            std::unique_lock<std::mutex> lock(controller_.jobsMutex_);
            controller_.jobsAvailable_.wait(
                lock,
                [&, this]()
                {
                    log().trace("Worker checking conditions.");
                    if (shouldTerminate_) {
                        log().trace("Terminating.");
                        // Set by the controller at shutdown or if a data source
                        // is removed. All worker instances are expected to terminate.
                        return true;
                    }
                    nextJob = controller_.nextJob(info_);
                    return nextJob.has_value();
                });
        }

        if (shouldTerminate_)
            return false;

        auto& job = *nextJob;

        try
        {
            if (job.cacheExpiredAt) {
                dataSource_->onCacheExpired(job.tileKey, *job.cacheExpiredAt);
            }

            auto layer = dataSource_->get(job.tileKey, controller_.cache_, info_);
            if (!layer)
                raise("DataSource::get() returned null.");

            // Special FeatureLayer handling
            if (layer->layerInfo()->type_ == LayerType::Features) {
                controller_.loadAddOnTiles(std::static_pointer_cast<TileFeatureLayer>(layer), *dataSource_);
            }

            // Apply TTL fallback (datasource-specific or service default).
            if (!layer->ttl())
            {
                auto ttlFallback = dataSource_->ttl();
                if (!ttlFallback)
                    ttlFallback = controller_.defaultTtl_;

                if (ttlFallback)
                    layer->setTtl(ttlFallback);
            }

            controller_.cache_->putTileLayer(layer);

            {
                std::unique_lock<std::mutex> lock(controller_.jobsMutex_);
                controller_.jobsInProgress_.erase(job.tileKey);
                job.request->notifyResult(layer);
                // As we entered a tile into the cache, notify other workers
                // that this tile can be served.
                controller_.jobsAvailable_.notify_all();
            }
        }
        catch (std::exception& e) {
            log().error("Could not load tile {}: {}",
                job.tileKey.toString(),
                e.what());
        }

        return true;
    }
};

struct Service::Impl : public Service::Controller
{
    std::map<DataSource::Ptr, DataSourceInfo> dataSourceInfo_;
    std::map<DataSource::Ptr, std::vector<Worker::Ptr>> dataSourceWorkers_;
    std::list<DataSource::Ptr> addOnDataSources_;

    std::unique_ptr<DataSourceConfigService::Subscription> configSubscription_;
    std::vector<DataSource::Ptr> dataSourcesFromConfig_;

    explicit Impl(
        Cache::Ptr cache,
        bool useDataSourceConfig,
        std::optional<std::chrono::milliseconds> defaultTtl)
        : Controller(std::move(cache), std::move(defaultTtl))
    {
        if (!useDataSourceConfig)
            return;
        configSubscription_ = DataSourceConfigService::get().subscribe(
            [this](auto&& dataSourceConfigNodes)
            {
                // Remove previous datasources.
                log().info("Config changed. Removing previous datasources.");
                for (auto const& datasource : dataSourcesFromConfig_) {
                    removeDataSource(datasource);
                }
                dataSourcesFromConfig_.clear();

                // Add datasources present in the new configuration.
                auto index = 0;
                for (const auto& configNode : dataSourceConfigNodes) {
                    if (auto dataSource = DataSourceConfigService::get().makeDataSource(configNode)) {
                        addDataSource(dataSource);
                        dataSourcesFromConfig_.push_back(dataSource);
                    }
                    else {
                        log().error(
                            "Failed to make datasource at index {}.", index);
                    }
                    ++index;
                }
            });
    }

    ~Impl()
    {
        // Ensure that no new datasources are added while we are cleaning up.
        configSubscription_.reset();

        for (auto& dataSourceAndWorkers : dataSourceWorkers_) {
            for (auto& worker : dataSourceAndWorkers.second) {
                worker->shouldTerminate_ = true;
            }
        }
        // Wake up all workers to check shouldTerminate_.
        jobsAvailable_.notify_all();

        for (auto& dataSourceAndWorkers : dataSourceWorkers_) {
            for (auto& worker : dataSourceAndWorkers.second) {
                if (worker->thread_.joinable()) {
                    worker->thread_.join();
                }
            }
        }
    }

    void addDataSource(DataSource::Ptr const& dataSource)
    {
        if (dataSource->info().nodeId_.empty()) {
            // Unique node IDs are required for the string pool offsets.
            raise("Tried to create service worker for an unnamed node!");
        }
        for (auto& existingSource : dataSourceInfo_) {
            if (existingSource.second.nodeId_ == dataSource->info().nodeId_) {
                // Unique node IDs are required for the string pool offsets.
                raise(
                    fmt::format("Data source with node ID '{}' already registered!",
                                dataSource->info().nodeId_));
            }
        }

        DataSourceInfo info = dataSource->info();
        dataSourceInfo_[dataSource] = info;

        // If the datasource is an add-on source, then it
        // does not have separate workers.
        if (info.isAddOn_) {
            addOnDataSources_.emplace_back(dataSource);
            return;
        }

        auto& workers = dataSourceWorkers_[dataSource];

        // Create workers for this DataSource
        for (auto i = 0; i < info.maxParallelJobs_; ++i)
            workers.emplace_back(std::make_shared<Worker>(
                dataSource,
                info,
                *this));
    }

    void removeDataSource(DataSource::Ptr const& dataSource)
    {
        dataSourceInfo_.erase(dataSource);
        addOnDataSources_.remove(dataSource);

        auto workers = dataSourceWorkers_.find(dataSource);
        if (workers != dataSourceWorkers_.end())
        {
            // Signal each worker thread to terminate.
            for (auto& worker : workers->second) {
                worker->shouldTerminate_ = true;
            }
            jobsAvailable_.notify_all();

            // Wait for each worker thread to terminate.
            for (auto& worker : workers->second) {
                if (worker->thread_.joinable()) {
                    worker->thread_.join();
                }
            }

            // Remove workers.
            dataSourceWorkers_.erase(workers);
        }
    }

    // All requests must be validated with canProcess before adding them!
    void addRequest(LayerTilesRequest::Ptr r)
    {
        if (!r)
            raise("Attempt to call Service::addRequest(nullptr).");
        if (r->isDone()) {
            // Nothing to do.
            r->notifyStatus();
            return;
        }

        {
            std::unique_lock lock(jobsMutex_);
            requests_.push_back(std::move(r));
        }
        jobsAvailable_.notify_all();
    }

    void abortRequest(LayerTilesRequest::Ptr const& r)
    {
        std::unique_lock lock(jobsMutex_);
        // Remove the request from the list of requests.
        auto numRemoved = requests_.remove_if([r](auto&& request) { return r == request; });
        // Clear its jobs to mark it as done.
        if (numRemoved) {
            r->setStatus(RequestStatus::Aborted);
        }
    }

    std::vector<DataSourceInfo> getDataSourceInfos(std::optional<AuthHeaders> const& clientHeaders)
    {
        std::vector<DataSourceInfo> infos;
        infos.reserve(dataSourceInfo_.size());
        for (const auto& [dataSource, info] : dataSourceInfo_) {
            if (!clientHeaders || dataSource->isDataSourceAuthorized(*clientHeaders)) {
                infos.push_back(info);
            }
        }
        return std::move(infos);
    }

    void loadAddOnTiles(TileFeatureLayer::Ptr const& baseTile, DataSource& baseDataSource) override {
        for (auto const& auxDataSource : addOnDataSources_) {
            if (auxDataSource->info().mapId_ == baseTile->mapId()) {
                auto auxTile = [&]() -> TileFeatureLayer::Ptr
                {
                    auto auxTile = auxDataSource->get(baseTile->id(), cache_, auxDataSource->info());
                    if (!auxTile) {
                        log().warn("auxDataSource returned null for {}", baseTile->id().toString());
                        return {};
                    }
                    if (auxTile->error()) {
                        log().warn("Error while fetching addon tile {}: {}", baseTile->id().toString(), *auxTile->error());
                        return {};
                    }
                    if (auxTile->layerInfo()->type_ != LayerType::Features) {
                        log().warn("Addon tile is not a feature layer");
                        return {};
                    }

                    return std::static_pointer_cast<TileFeatureLayer>(auxTile);
                }();

                if (!auxTile) {
                    // Error messages have been generated above.
                    continue;
                }

                // Re-encode the base tile in a common string namespace.
                // This is necessary, because the aux tile may introduce new strings
                // to the base tile. Since we cannot manipulate the original
                // node's string pool, we have to create a new one based on a new
                // artificial node id.
                auto auxBaseNodeId = baseTile->nodeId() + "|" + auxTile->nodeId();
                auto auxBaseStringPool = cache_->getStringPool(auxBaseNodeId);
                baseTile->setStrings(auxBaseStringPool);
                baseTile->setNodeId(auxBaseNodeId);

                // Adopt new attributes, features and relations for the base feature
                // from the auxiliary feature.
                std::unordered_map<uint32_t, simfil::ModelNode::Ptr> clonedModelNodes;
                for (auto const& auxFeature : *auxTile)
                {
                    // Note: A single secondary feature ID may resolve to multiple
                    // primary feature IDs. So we keep a vector of aux feature ID info.
                    std::vector<std::pair<std::string_view, KeyValueViewPairs>> auxFeatureIds = {
                        {auxFeature->id()->typeId(), auxFeature->id()->keyValuePairs()}};

                    // Convert the feature reference to multiple direct ones on-demand.
                    // If the ID does not validate as a primary feature id, we assume
                    // that it uses a secondary ID scheme for which a locate-call
                    // is required.
                    auto idIsIndirect = !baseTile->layerInfo()->validFeatureId(
                        auxFeatureIds[0].first,
                        auxFeatureIds[0].second,
                        true);
                    std::vector<LocateResponse> locateResponses;
                    if (idIsIndirect)
                    {
                        locateResponses = baseDataSource.locate(LocateRequest(
                            auxTile->mapId(),
                            std::string(auxFeatureIds[0].first),
                            castToKeyValue(auxFeatureIds[0].second)));
                        if (locateResponses.empty()) {
                            log().warn("Could not locate indirect aux feature id {}", auxFeature->id()->toString());
                            continue;
                        }
                        auxFeatureIds.clear();
                        for (auto const& resolution : locateResponses) {
                            // Do not adopt resolutions which point to a different tile layer.
                            if (resolution.tileKey_ != baseTile->id())
                                continue;
                            auxFeatureIds.emplace_back(
                                resolution.typeId_,
                                castToKeyValueView(resolution.featureId_));
                        }
                    }

                    // Go over all feature IDs to which the auxiliary feature data should be appended.
                    for (auto const& [auxFeatureType, auxFeatureKvp] : auxFeatureIds) {
                        baseTile->clone(
                            clonedModelNodes,
                            auxTile,
                            *auxFeature,
                            auxFeatureType,
                            auxFeatureKvp);
                    }
                }
            }
        }
    }
};

Service::Service(Cache::Ptr cache, bool useDataSourceConfig, std::optional<std::chrono::milliseconds> defaultTtl)
    : impl_(std::make_unique<Impl>(std::move(cache), useDataSourceConfig, std::move(defaultTtl)))
{
}

Service::~Service() = default;

void Service::add(DataSource::Ptr const& dataSource)
{
    impl_->addDataSource(dataSource);
}

void Service::remove(const DataSource::Ptr& dataSource)
{
    impl_->removeDataSource(dataSource);
}

bool Service::request(std::vector<LayerTilesRequest::Ptr> const& requests, std::optional<AuthHeaders> const& clientHeaders)
{
    bool dataSourcesAvailable = true;
    for (const auto& r : requests) {
        switch (hasLayerAndCanAccess(r->mapId_, r->layerId_, clientHeaders))
        {
        case RequestStatus::NoDataSource:
            dataSourcesAvailable = false;
            log().debug("No data source can provide requested map and layer: {}::{}",
                r->mapId_,
                r->layerId_);
            r->setStatus(RequestStatus::NoDataSource);
            break;
        case RequestStatus::Unauthorized:
            dataSourcesAvailable = false;
            log().debug("Not authorized to access requested map and layer: {}::{}",
                r->mapId_,
                r->layerId_);
            r->setStatus(RequestStatus::Unauthorized);
            break;
        default: {}
            // Nothing to do.
        }
    }

    // Second pass either aborts requests or add all to job queue.
    for (const auto& r : requests) {
        if (!dataSourcesAvailable) {
            if (r->getStatus() == RequestStatus::Open) {
                log().debug("Aborting unfulfillable request!");
                r->setStatus(RequestStatus::Aborted);
            }
        }
        else {
            impl_->addRequest(r);
        }
    }
    return dataSourcesAvailable;
}

std::vector<LocateResponse> Service::locate(LocateRequest const& req)
{
    std::vector<LocateResponse> results;
    for (auto const& [ds, info] : impl_->dataSourceInfo_)
        if (info.mapId_ == req.mapId_ && !info.isAddOn_) {
            for (auto const& location : ds->locate(req))
                results.emplace_back(location);
        }
    return results;
}

void Service::abort(const LayerTilesRequest::Ptr& r)
{
    impl_->abortRequest(r);
}

std::vector<DataSourceInfo> Service::info(std::optional<AuthHeaders> const& clientHeaders)
{
    return impl_->getDataSourceInfos(clientHeaders);
}

Cache::Ptr Service::cache()
{
    return impl_->cache_;
}

RequestStatus Service::hasLayerAndCanAccess(
    std::string const& mapId,
    std::string const& layerId,
    std::optional<AuthHeaders> const& clientHeaders) const
{
    std::unique_lock lock(impl_->jobsMutex_);
    // Check that one of the data sources can fulfill the request.
    for (auto& [ds, info] : impl_->dataSourceInfo_) {
        if (mapId != info.mapId_)
            continue;
        if (info.layers_.find(layerId) != info.layers_.end()) {
            if (clientHeaders && !ds->isDataSourceAuthorized(*clientHeaders)) {
                return RequestStatus::Unauthorized;
            }
            return RequestStatus::Success;
        }
    }
    return RequestStatus::NoDataSource;
}

nlohmann::json Service::getStatistics() const
{
    auto datasources = nlohmann::json::array();
    for (auto const& [dataSource, info] : impl_->dataSourceInfo_) {
        datasources.push_back({
            {"name", info.mapId_},
            {"workers", impl_->dataSourceWorkers_[dataSource].size()}
        });
    }

    return {
        {"datasources", datasources},
        {"active-requests", impl_->requests_.size()}
    };
}

}  // namespace mapget
