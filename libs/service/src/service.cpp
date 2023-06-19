#include "service.h"

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

bool Request::isDone() const
{
    return nextTileIndex_ == tiles_.size();
}

struct Service::Worker
{
    using Ptr = std::shared_ptr<Worker>;
    using Job = std::pair<MapTileKey, Request::Ptr>;
    using NewJobCallback = std::function<std::optional<Worker::Job>()>;

    Cache::Ptr cache_;             // Cache used by the worker
    DataSource::Ptr dataSource_;   // Data source the worker is responsible for
    DataSourceInfo info_;          // Information about the data source
    NewJobCallback nextJobFun_;    // Function to get the next job for this worker
    bool shouldTerminate_ = false; // Flag indicating whether the worker thread should terminate
    std::condition_variable& cv_;  // Condition variable to signal job availability or thread termination
    std::mutex& cvMutex_;          // Mutex used with the condition variable
    std::thread thread_;           // The worker thread

    Worker(
        Cache::Ptr cache,
        DataSource::Ptr dataSource,
        DataSourceInfo info,
        NewJobCallback nextJobFun,
        std::condition_variable& cv,
        std::mutex& cvMutex)
        : cache_(std::move(cache)),
          dataSource_(std::move(dataSource)),
          info_(std::move(info)),
          nextJobFun_(std::move(nextJobFun)),
          cv_(cv),
          cvMutex_(cvMutex)
    {
        thread_ = std::thread(
            [this]()
            {
                while (true) {
                    std::unique_lock<std::mutex> lock(cvMutex_);
                    cv_.wait(lock, [this]() { return shouldTerminate_ || nextJobFun_(); });

                    if (shouldTerminate_)
                        return;

                    auto nextKeyAndCallback = nextJobFun_();
                    if (!nextKeyAndCallback)
                        continue;

                    auto& [mapTileKey, request] = *nextKeyAndCallback;
                    auto result = dataSource_->get(mapTileKey, *cache_, info_);
                    if (request->onResult_)
                        request->onResult_(result);
                }
            });
    }
};

struct Service::Impl
{
    explicit Impl(Cache::Ptr cache) : cache_(std::move(cache)) {}

    std::map<DataSource::Ptr, DataSourceInfo> dataSourceInfo_;  // Map of data sources and their corresponding info
    std::map<DataSource::Ptr, std::vector<Worker::Ptr>> dataSourceWorkers_; // Map of data sources and their corresponding workers
    std::set<MapTileKey> jobsInProgress_;    // Set of jobs currently in progress
    Cache::Ptr cache_;                       // The cache for the service
    std::list<Request::Ptr> requests_;       // List of requests currently being processed
    std::condition_variable jobsAvailable_;  // Condition variable to signal job availability
    std::mutex jobsMutex_;  // Mutex used with the jobsAvailable_ condition variable

    void addDataSource(DataSource::Ptr const& dataSource)
    {
        DataSourceInfo info = dataSource->info();
        dataSourceInfo_[dataSource] = info;
        auto& workers = dataSourceWorkers_[dataSource];

        // Create workers for this DataSource
        for (auto i = 0; i < info.maxParallelJobs_; ++i)
            workers.emplace_back(std::make_shared<Worker>(
                cache_,
                dataSource,
                info,
                [this, info]() { return nextJob(info); },
                jobsAvailable_,
                jobsMutex_));
    }

    std::optional<Worker::Job> nextJob(DataSourceInfo const& i)
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        std::optional<Worker::Job> result;

        // Return next job if available
        for (auto requestIt = requests_.begin(); requestIt != requests_.end(); ++requestIt) {
            auto& request = *requestIt;
            auto layerIt = i.layers_.find(request->layerId_);
            if (request->mapId_ == i.mapId_ && layerIt != i.layers_.end()) {
                auto tileId = request->tiles_[request->nextTileIndex_++];
                result = {{}, request};
                result->first.layer_ = layerIt->second->type_;
                result->first.mapId_ = request->mapId_;
                result->first.layerId_ = request->layerId_;
                result->first.tileId_ = tileId;

                // Move this request to the end of the list, so it loses priority
                requests_.splice(requests_.end(), requests_, requestIt);
            }
        }

        // Clean up done requests.
        requests_.remove_if([](auto&& r) { return r->isDone(); });

        // No job available
        return result;
    }

    void removeDataSource(DataSource::Ptr const& dataSource)
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        dataSourceInfo_.erase(dataSource);
        auto workers = dataSourceWorkers_.find(dataSource);

        if (workers != dataSourceWorkers_.end())
        {
            // Signal each worker thread to terminate
            for (auto& worker : workers->second) {
                worker->shouldTerminate_ = true;
            }
            jobsAvailable_.notify_all();

            // Wait for each worker thread to terminate
            for (auto& worker : workers->second) {
                if (worker->thread_.joinable()) {
                    worker->thread_.join();
                }
            }

            // Remove workers
            dataSourceWorkers_.erase(workers);
        }
    }

    void addRequest(Request::Ptr r)
    {
        {
            std::lock_guard<std::mutex> lock(jobsMutex_);
            requests_.push_back(std::move(r));
        }
        jobsAvailable_.notify_all();
    }

    void abortRequest(Request::Ptr const& r)
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        // Simply Remove the request from the list of requests
        requests_.remove_if([r](auto&& request) { return r == request; });
    }

    std::vector<DataSourceInfo> getDataSourceInfos()
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        std::vector<DataSourceInfo> infos;
        infos.reserve(dataSourceInfo_.size());
        for (const auto& [dataSource, info] : dataSourceInfo_) {
            infos.push_back(info);
        }
        return std::move(infos);
    }
};

Service::Service(Cache::Ptr cache) : impl_(std::make_unique<Impl>(std::move(cache))) {}

Service::~Service()
{
    for (auto& dataSourceAndWorkers : impl_->dataSourceWorkers_) {
        for (auto& worker : dataSourceAndWorkers.second) {
            worker->shouldTerminate_ = true;
        }
    }
    impl_->jobsAvailable_.notify_all();  // Wake up all workers to check shouldTerminate_

    for (auto& dataSourceAndWorkers : impl_->dataSourceWorkers_) {
        for (auto& worker : dataSourceAndWorkers.second) {
            if (worker->thread_.joinable()) {
                worker->thread_.join();
            }
        }
    }
}

void Service::add(DataSource::Ptr const& dataSource)
{
    impl_->addDataSource(dataSource);
}

void Service::remove(const DataSource::Ptr& dataSource)
{
    impl_->removeDataSource(dataSource);
}

void Service::request(Request::Ptr r)
{
    impl_->addRequest(std::move(r));
}

void Service::abort(const Request::Ptr& r)
{
    impl_->abortRequest(r);
}

std::vector<DataSourceInfo> Service::info()
{
    return impl_->getDataSourceInfos();
}

}  // namespace mapget
