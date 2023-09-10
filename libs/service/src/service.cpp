#include "service.h"
#include "mapget/log.h"

#include <optional>
#include <set>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <list>

namespace mapget
{

Request::Request(
    std::string mapId,
    std::string layerId,
    std::vector<TileId> tiles,
    std::function<void(TileFeatureLayer::Ptr)> onResult)
    : mapId_(std::move(mapId)),
      layerId_(std::move(layerId)),
      tiles_(std::move(tiles)),
      onResult_(std::move(onResult))
{
}

void Request::notifyResult(TileFeatureLayer::Ptr r) {
    if (onResult_)
        onResult_(std::move(r));
    ++resultCount_;
    if (resultCount_ == tiles_.size()) {
        setStatus(RequestStatus::Done);
    }
}

void Request::setStatus(RequestStatus s)
{
    {
        std::unique_lock doneLock(statusMutex_);
        this->status_ = s;
    }
    notifyStatus();
}

void Request::notifyStatus()
{
    // Run the final callback function.
    if (onDone_) {
        onDone_();
    }
    statusConditionVariable_.notify_all();
}

void Request::wait()
{
    std::unique_lock doneLock(statusMutex_);
    statusConditionVariable_.wait(doneLock, [this]{return this->getStatus() != Open;});
}

nlohmann::json Request::toJson()
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

RequestStatus Request::getStatus()
{
    return this->status_;
}

struct Service::Controller
{
    using Job = std::pair<MapTileKey, Request::Ptr>;

    std::set<MapTileKey> jobsInProgress_;    // Set of jobs currently in progress
    Cache::Ptr cache_;                       // The cache for the service
    std::list<Request::Ptr> requests_;       // List of requests currently being processed
    std::condition_variable jobsAvailable_;  // Condition variable to signal job availability
    std::mutex jobsMutex_;  // Mutex used with the jobsAvailable_ condition variable

    explicit Controller(Cache::Ptr cache) : cache_(std::move(cache))
    {
        if (!cache_)
            throw logRuntimeError("Cache must not be null!");
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
                auto& request = *reqIt;
                auto layerIt = i.layers_.find(request->layerId_);

                // Are there tiles left to be processed in the request?
                if (request->mapId_ == i.mapId_ && layerIt != i.layers_.end()) {
                    if (request->nextTileIndex_ >= request->tiles_.size()) {
                        continue;
                    }

                    // Create result wrapper object.
                    auto tileId = request->tiles_[request->nextTileIndex_++];
                    result = {MapTileKey(), request};
                    result->first.layer_ = layerIt->second->type_;
                    result->first.mapId_ = request->mapId_;
                    result->first.layerId_ = request->layerId_;
                    result->first.tileId_ = tileId;

                    // Cache lookup.
                    auto cachedResult = cache_->getTileFeatureLayer(result->first, i);
                    if (cachedResult) {
                        // TODO: Consider TTL.
                        log().debug("Serving cached tile: {}", result->first.toString());
                        request->notifyResult(cachedResult);
                        result.reset();
                        cachedTilesServed = true;
                        continue;
                    }

                    if (jobsInProgress_.find(result->first) != jobsInProgress_.end()) {
                        // Don't work on something that is already being worked on.
                        // Wait for the work to finish, then send the (hopefully cached) result.
                        log().debug("Delaying tile with job in progress: {}",
                                     result->first.toString());
                        --(request->nextTileIndex_);
                        result.reset();
                        continue;
                    }

                    // Enter into the jobs-in-progress set.
                    jobsInProgress_.insert(result->first);

                    // Move this request to the end of the list, so others gain priority.
                    requests_.splice(requests_.end(), requests_, reqIt);

		    log().debug("Working on tile: {}", result->first.toString());
                    break;
                }
            }
        }
        while (cachedTilesServed && !result);

        // Clean up done requests.
        requests_.remove_if([](auto&& r) {return r->nextTileIndex_ == r->tiles_.size(); });

        return result;
    }
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
                    if (shouldTerminate_) {
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

        auto& [mapTileKey, request] = *nextJob;

        try
        {
            auto result = dataSource_->get(mapTileKey, controller_.cache_, info_);
            if (!result)
                throw logRuntimeError("DataSource::get() returned null.");

            {
                std::unique_lock<std::mutex> lock(controller_.jobsMutex_);
                controller_.cache_->putTileFeatureLayer(result);
                controller_.jobsInProgress_.erase(mapTileKey);
                request->notifyResult(result);
                // As we entered a tile into the cache, notify other workers
                // that this tile can be served.
                controller_.jobsAvailable_.notify_all();
            }
        }
        catch (std::exception& e) {
		log().error("Could not load tile {}: {}",
				mapTileKey.toString(),
				e.what());
        }

        return true;
    }
};

struct Service::Impl : public Service::Controller
{
    std::map<DataSource::Ptr, DataSourceInfo> dataSourceInfo_;
    std::map<DataSource::Ptr, std::vector<Worker::Ptr>> dataSourceWorkers_;

    explicit Impl(Cache::Ptr cache) : Controller(std::move(cache)) {}

    ~Impl()
    {
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
        DataSourceInfo info = dataSource->info();
        dataSourceInfo_[dataSource] = info;
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
        std::unique_lock lock(jobsMutex_);
        dataSourceInfo_.erase(dataSource);
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
    void addRequest(Request::Ptr r)
    {
        if (!r)
            throw logRuntimeError("Attempt to call Service::parseRequestFromJson(nullptr).");
        if (r->getStatus() == RequestStatus::Done) {
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

    void abortRequest(Request::Ptr const& r)
    {
        std::unique_lock lock(jobsMutex_);
        // Remove the request from the list of requests.
        requests_.remove_if([r](auto&& request) { return r == request; });
    }

    std::vector<DataSourceInfo> getDataSourceInfos()
    {
        std::vector<DataSourceInfo> infos;
        infos.reserve(dataSourceInfo_.size());
        for (const auto& [dataSource, info] : dataSourceInfo_) {
            infos.push_back(info);
        }
        return std::move(infos);
    }
};

Service::Service(Cache::Ptr cache) : impl_(std::make_unique<Impl>(std::move(cache))) {}

Service::~Service() = default;

void Service::add(DataSource::Ptr const& dataSource)
{
    impl_->addDataSource(dataSource);
}

void Service::remove(const DataSource::Ptr& dataSource)
{
    impl_->removeDataSource(dataSource);
}

Request::Ptr Service::request(Request::Ptr r)
{
    impl_->addRequest(r);
    return r;
}

void Service::abort(const Request::Ptr& r)
{
    impl_->abortRequest(r);
}

std::vector<DataSourceInfo> Service::info()
{
    return impl_->getDataSourceInfos();
}

Cache::Ptr Service::cache()
{
    return impl_->cache_;
}

bool Service::canProcess(std::string const& mapId, std::string const& layerId)
{
    {
        std::unique_lock lock(impl_->jobsMutex_);
        // Check that one of the data sources can fulfill the request.
        for (auto& dataSourceAndInfo : impl_->dataSourceInfo_) {
            auto& info = dataSourceAndInfo.second;
            if (mapId != info.mapId_)
                continue;
            if (info.layers_.find(layerId) != info.layers_.end()) {
                return true;
            }
        }
        return false;
    }
}

}  // namespace mapget
