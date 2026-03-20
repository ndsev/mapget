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
#include <atomic>
#include <condition_variable>
#include <thread>
#include <list>
#include <chrono>
#include <shared_mutex>
#include <algorithm>
#include <numeric>
#include <unordered_map>

#include "simfil/types.h"

namespace mapget
{

namespace {

/** Ensure that Tile IDs are unique across all stages. */
std::vector<std::vector<TileId>> normalizeTileBuckets(std::vector<std::vector<TileId>> buckets)
{
    std::set<TileId> seenTileIds;
    for (auto& bucket : buckets) {
        std::vector<TileId> uniqueTiles;
        uniqueTiles.reserve(bucket.size());
        for (auto const& tileId : bucket) {
            if (seenTileIds.insert(tileId).second) {
                uniqueTiles.push_back(tileId);
            }
        }
        bucket.swap(uniqueTiles);
    }
    return buckets;
}

}  // namespace

LayerTilesRequest::LayerTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<TileId> tiles)
    : LayerTilesRequest(
          std::move(mapId),
          std::move(layerId),
          std::vector<std::vector<TileId>>{std::move(tiles)})
{
}

LayerTilesRequest::LayerTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<std::vector<TileId>> tileIdsByNextStage)
    : mapId_(std::move(mapId)),
      layerId_(std::move(layerId)),
      tileIdsByNextStage_(normalizeTileBuckets(std::move(tileIdsByNextStage)))
{
    bool hasAnyTileIds = false;
    for (auto const& bucket : tileIdsByNextStage_) {
        if (!bucket.empty()) {
            hasAnyTileIds = true;
            break;
        }
    }

    if (!hasAnyTileIds) {
        // An empty request is always set to success, but the client/service
        // is responsible for triggering notifyStatus() in that case.
        status_ = RequestStatus::Success;
    }
}

void LayerTilesRequest::prepareResolvedLayer(LayerType layerType, uint32_t stages)
{
    nextTileIndex_ = 0;
    resultCount_ = 0;
    resolvedTileKeys_.clear();
    tileKeysNotStarted_.clear();

    const auto normalizedStages = std::max<uint32_t>(1U, stages);

    for (uint32_t stage = 0; stage < normalizedStages; ++stage) {
        // For all tiles in bucket 0, we need to enqueue N stages.
        // For tiles in bucket 1, we need to enqueue N-1 stages.
        // etc.
        for (size_t bucketIndex = 0; bucketIndex < tileIdsByNextStage_.size(); ++bucketIndex) {
            const auto nextMissingStage = static_cast<uint32_t>(bucketIndex);
            if (nextMissingStage > stage || nextMissingStage >= normalizedStages) {
                continue;
            }
            for (auto const& tileId : tileIdsByNextStage_[bucketIndex]) {
                MapTileKey key(layerType, mapId_, layerId_, tileId, stage);
                if (tileKeysNotStarted_.insert(key).second) {
                    resolvedTileKeys_.push_back(std::move(key));
                }
            }
        }
    }

    status_ = resolvedTileKeys_.empty() ? RequestStatus::Success : RequestStatus::Open;
}

void LayerTilesRequest::notifyResult(TileLayer::Ptr r) {
    if (isDone()) {
        return;
    }

    const auto type = r->layerInfo()->type_;
    switch (type) {
    case LayerType::Features:
        if (onFeatureLayer_)
            onFeatureLayer_(std::move(std::static_pointer_cast<TileFeatureLayer>(r)));
        break;
    case LayerType::SourceData:
        if (onSourceDataLayer_)
            onSourceDataLayer_(std::move(std::static_pointer_cast<TileSourceDataLayer>(r)));
        break;
    default:
        log().error(fmt::format("Unhandled layer type {}, no matching callback!", static_cast<int>(type)));
        break;
    }

    ++resultCount_;
    if (resultCount_ == resolvedTileKeys_.size()) {
        setStatus(RequestStatus::Success);
    }
}

void LayerTilesRequest::notifyLoadState(MapTileKey const& key, TileLayer::LoadState state) const {
    if (onLoadStateChanged_) {
        onLoadStateChanged_(key, state);
    }
}

void LayerTilesRequest::setStatus(RequestStatus s)
{
    this->status_ = s;
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
    auto requestJson = nlohmann::json::object({
        {"mapId", mapId_},
        {"layerId", layerId_}
    });

    if (tileIdsByNextStage_.size() <= 1) {
        auto tileIds = nlohmann::json::array();
        if (!tileIdsByNextStage_.empty()) {
            for (auto const& tileId : tileIdsByNextStage_.front()) {
                tileIds.emplace_back(tileId.value_);
            }
        }
        requestJson["tileIds"] = std::move(tileIds);
        return requestJson;
    }

    auto tileIdsByNextStage = nlohmann::json::array();
    for (auto const& bucket : tileIdsByNextStage_) {
        auto tileIds = nlohmann::json::array();
        for (auto const& tileId : bucket) {
            tileIds.emplace_back(tileId.value_);
        }
        tileIdsByNextStage.push_back(std::move(tileIds));
    }
    requestJson["tileIdsByNextStage"] = std::move(tileIdsByNextStage);
    return requestJson;
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
    virtual ~Controller() = default;

    struct Job {
        MapTileKey tileKey;
        std::vector<LayerTilesRequest::Ptr> waitingRequests;
        std::optional<std::chrono::system_clock::time_point> cacheExpiredAt;
        TileLayer::LoadState loadStatus = TileLayer::LoadState::LoadingQueued;
    };

    std::map<MapTileKey, std::shared_ptr<Job>> jobsInProgress_;    // Jobs currently in progress + interested requests
    Cache::Ptr cache_;                       // The cache for the service
    std::optional<std::chrono::milliseconds> defaultTtl_; // Default TTL applied when datasource does not override
    std::list<LayerTilesRequest::Ptr> requests_;       // List of requests currently being processed
    std::condition_variable jobsAvailable_;  // Condition variable to signal job availability
    std::mutex jobsMutex_;  // Mutex used with the jobsAvailable_ condition variable

    explicit Controller(Cache::Ptr cache, std::optional<std::chrono::milliseconds> defaultTtl)
        : cache_(std::move(cache)),
          defaultTtl_(defaultTtl)
    {
        if (!cache_)
            raise("Cache must not be null!");
    }

    struct Candidate {
        std::list<LayerTilesRequest::Ptr>::const_iterator requestIt_;
        LayerTilesRequest::Ptr request_;
        MapTileKey tileKey_;
        size_t nextTileIndex_ = 0;
    };

    static bool requestMatchesDataSource(
        LayerTilesRequest::Ptr const& request,
        DataSourceInfo const& info)
    {
        if (!request || request->isDone())
            return false;
        if (request->mapId_ != info.mapId_)
            return false;
        return info.layers_.find(request->layerId_) != info.layers_.end();
    }

    [[nodiscard]] std::optional<size_t> nextPendingTileKey(LayerTilesRequest const& request) const
    {
        auto keyIndex = request.nextTileIndex_;
        while (keyIndex < request.resolvedTileKeys_.size()) {
            auto const& candidate = request.resolvedTileKeys_[keyIndex];
            if (request.tileKeysNotStarted_.find(candidate) != request.tileKeysNotStarted_.end()) {
                return keyIndex;
            }
            ++keyIndex;
        }
        return {};
    }

    [[nodiscard]] std::optional<Candidate> bestCandidate(DataSourceInfo const& info) const
    {
        std::optional<Candidate> best;

        for (auto reqIt = requests_.begin(); reqIt != requests_.end(); ++reqIt) {
            auto const& request = *reqIt;
            if (!requestMatchesDataSource(request, info))
                continue;

            auto pendingIndex = nextPendingTileKey(*request);
            if (!pendingIndex)
                continue;
            auto pendingKey = request->resolvedTileKeys_[*pendingIndex];

            if (!best || pendingKey.stage_ < best->tileKey_.stage_) {
                best = Candidate{
                    .requestIt_ = reqIt,
                    .request_ = request,
                    .tileKey_ = pendingKey,
                    .nextTileIndex_ = *pendingIndex,
                };
            }
        }

        return best;
    }

    void attachMatchingRequests(
        LayerTilesRequest::Ptr const& selectedRequest,
        MapTileKey const& tileKey,
        std::vector<LayerTilesRequest::Ptr>& waitingRequests) const
    {
        for (auto const& otherRequest : requests_) {
            if (!otherRequest || otherRequest == selectedRequest)
                continue;
            if (otherRequest->mapId_ != selectedRequest->mapId_ || otherRequest->layerId_ != selectedRequest->layerId_)
                continue;
            if (otherRequest->tileKeysNotStarted_.erase(tileKey) == 0)
                continue;
            waitingRequests.push_back(otherRequest);
        }
    }

    [[nodiscard]] std::shared_ptr<Job> dispatchCandidate(
        Candidate const& candidate,
        DataSourceInfo const& info)
    {
        // Commit this candidate as "consumed" for the request before dispatching it.
        // The tile is then either satisfied immediately (cache/in-progress) or started.
        auto const& request = candidate.request_;
        request->nextTileIndex_ = candidate.nextTileIndex_;
        request->tileKeysNotStarted_.erase(candidate.tileKey_);

        auto cachedResult = cache_->getTileLayer(candidate.tileKey_, info);
        if (cachedResult.tile) {
            log().debug("Serving cached tile: {}", candidate.tileKey_.toString());
            request->notifyResult(cachedResult.tile);
            return nullptr;
        }

        if (auto inProgress = jobsInProgress_.find(candidate.tileKey_);
            inProgress != jobsInProgress_.end())
        {
            log().debug("Joining tile with job in progress: {}", candidate.tileKey_.toString());
            request->notifyLoadState(candidate.tileKey_, inProgress->second->loadStatus);
            inProgress->second->waitingRequests.push_back(request);
            return nullptr;
        }

        auto startedJob = std::make_shared<Job>(Job{candidate.tileKey_, {request}, cachedResult.expiredAt});
        attachMatchingRequests(request, candidate.tileKey_, startedJob->waitingRequests);
        jobsInProgress_.emplace(startedJob->tileKey, startedJob);

        // Move this request to the end of the list, so others gain priority.
        requests_.splice(requests_.end(), requests_, candidate.requestIt_);
        log().debug("Working on tile: {}", startedJob->tileKey.toString());

        return startedJob;
    }

    void removeCompletedRequests()
    {
        requests_.remove_if([](auto const& request) {
            return !request || request->tileKeysNotStarted_.empty();
        });
    }

    std::shared_ptr<Job> nextJob(DataSourceInfo const& i, std::unique_lock<std::mutex>& lock)
    {
        // Workers call the nextJob function when they are free.
        // Note: For thread safety, jobsMutex_ must be held
        //  when calling this function. The lock may be released/re-acquired
        //  between sweeps to allow external updates.

        while (true) {
            // 1) Pick highest-priority pending tile for this datasource worker.
            auto candidate = bestCandidate(i);
            if (!candidate)
                break;

            // 2) Dispatch that tile: cache hit, join running job, or start backend work.
            if (auto dispatchResult = dispatchCandidate(*candidate, i)) {
                removeCompletedRequests();
                return dispatchResult;
            }

            // 3) No backend job started yet, so yield lock and sweep again.
            // Cached/in-progress work was handled without starting a new backend job.
            // Let external threads update requests_ before the next sweep.
            lock.unlock();
            lock.lock();
        }

        removeCompletedRequests();
        return {};
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
        std::shared_ptr<Controller::Job> nextJob;

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
                    nextJob = controller_.nextJob(info_, lock);
                    return !!nextJob;
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

            auto notifyWaitingRequests = [&](TileLayer::LoadState state) {
                std::vector<LayerTilesRequest::Ptr> waiting;
                {
                    std::unique_lock lock(controller_.jobsMutex_);
                    job.loadStatus = state;
                    waiting = job.waitingRequests;
                }
                for (auto const& req : waiting) {
                    if (req) {
                        req->notifyLoadState(job.tileKey, state);
                    }
                }
            };

            notifyWaitingRequests(TileLayer::LoadState::BackendFetching);
            auto layer = dataSource_->get(
                job.tileKey,
                controller_.cache_,
                info_,
                [&notifyWaitingRequests](TileLayer::LoadState state) {
                    notifyWaitingRequests(state);
                });
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

            std::vector<LayerTilesRequest::Ptr> notifyRequests;
            {
                std::unique_lock<std::mutex> lock(controller_.jobsMutex_);
                controller_.jobsInProgress_.erase(job.tileKey);
                notifyRequests = job.waitingRequests;
            }
            for (auto const& req : notifyRequests) {
                if (req) {
                    req->notifyResult(layer);
                }
            }
            // As we entered a tile into the cache, notify other workers
            // that this tile can be served.
            controller_.jobsAvailable_.notify_all();
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

    mutable std::shared_mutex dataSourcesMutex_;
    std::unique_ptr<DataSourceConfigService::Subscription> configSubscription_;
    std::vector<DataSource::Ptr> dataSourcesFromConfig_;

    explicit Impl(
        Cache::Ptr cache,
        bool useDataSourceConfig,
        std::optional<std::chrono::milliseconds> defaultTtl)
        : Controller(std::move(cache), defaultTtl)
    {
        if (!useDataSourceConfig)
            return;
        configSubscription_ = DataSourceConfigService::get().subscribe(
            [this](auto&& dataSourceConfigNodes)
            {
                std::vector<DataSource::Ptr> previousDataSources;
                {
                    std::unique_lock lock(dataSourcesMutex_);
                    previousDataSources.swap(dataSourcesFromConfig_);
                }

                // Remove previous datasources.
                log().info("Config changed. Removing previous datasources.");
                for (auto const& datasource : previousDataSources) {
                    removeDataSource(datasource);
                }

                // Add datasources present in the new configuration.
                auto index = 0;
                std::vector<DataSource::Ptr> configuredDataSources;
                for (const auto& configNode : dataSourceConfigNodes) {
                    if (auto dataSource = DataSourceConfigService::get().makeDataSource(configNode)) {
                        addDataSource(dataSource);
                        configuredDataSources.push_back(dataSource);
                    }
                    else {
                        log().error(
                            "Failed to make datasource at index {}.", index);
                    }
                    ++index;
                }

                std::unique_lock lock(dataSourcesMutex_);
                dataSourcesFromConfig_ = std::move(configuredDataSources);
            });
    }

    ~Impl() override
    {
        // Ensure that no new datasources are added while we are cleaning up.
        configSubscription_.reset();

        std::vector<Worker::Ptr> workersToJoin;
        {
            std::unique_lock lock(dataSourcesMutex_);
            for (auto& [_, workers] : dataSourceWorkers_) {
                for (auto& worker : workers) {
                    worker->shouldTerminate_ = true;
                    workersToJoin.push_back(worker);
                }
            }
            dataSourceWorkers_.clear();
            dataSourceInfo_.clear();
            addOnDataSources_.clear();
            dataSourcesFromConfig_.clear();
        }
        // Wake up all workers to check shouldTerminate_.
        jobsAvailable_.notify_all();

        for (auto& worker : workersToJoin) {
            if (worker->thread_.joinable()) {
                worker->thread_.join();
            }
        }
    }

    void addDataSource(DataSource::Ptr const& dataSource)
    {
        std::unique_lock lock(dataSourcesMutex_);

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
        std::vector<Worker::Ptr> workersToJoin;
        {
            std::unique_lock lock(dataSourcesMutex_);
            dataSourceInfo_.erase(dataSource);
            addOnDataSources_.remove(dataSource);

            auto workers = dataSourceWorkers_.find(dataSource);
            if (workers == dataSourceWorkers_.end()) {
                return;
            }
            for (auto& worker : workers->second) {
                worker->shouldTerminate_ = true;
                workersToJoin.push_back(worker);
            }
            dataSourceWorkers_.erase(workers);
        }

        jobsAvailable_.notify_all();

        for (auto& worker : workersToJoin) {
            if (worker->thread_.joinable()) {
                worker->thread_.join();
            }
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
        // Mark the request as aborted.
        r->setStatus(RequestStatus::Aborted);

        // Remove the request from the list of requests (needs lock).
        {
            std::unique_lock lock(jobsMutex_);
            requests_.remove_if([r](auto&& request) { return r == request; });
        }
    }

    std::vector<DataSourceInfo> getDataSourceInfos(std::optional<AuthHeaders> const& clientHeaders)
    {
        std::vector<DataSourceInfo> infos;
        std::shared_lock lock(dataSourcesMutex_);
        infos.reserve(dataSourceInfo_.size());
        for (const auto& [dataSource, info] : dataSourceInfo_) {
            if (!clientHeaders || dataSource->isDataSourceAuthorized(*clientHeaders)) {
                infos.push_back(info);
            }
        }
        return infos;
    }

    void loadAddOnTiles(TileFeatureLayer::Ptr const& baseTile, DataSource& baseDataSource) override {
        std::vector<DataSource::Ptr> addOnDataSources;
        {
            std::shared_lock lock(dataSourcesMutex_);
            addOnDataSources.assign(addOnDataSources_.begin(), addOnDataSources_.end());
        }

        for (auto const& auxDataSource : addOnDataSources) {
            if (auxDataSource->info().mapId_ == baseTile->mapId()) {
                auto auxTile = [&]() -> TileFeatureLayer::Ptr
                {
                    auto result = auxDataSource->get(baseTile->id(), cache_, auxDataSource->info());
                    if (!result) {
                        log().warn("auxDataSource returned null for {}", baseTile->id().toString());
                        return {};
                    }
                    if (result->error()) {
                        log().warn("Error while fetching addon tile {}: {}", baseTile->id().toString(), *result->error());
                        return {};
                    }
                    if (result->layerInfo()->type_ != LayerType::Features) {
                        log().warn("Addon tile is not a feature layer");
                        return {};
                    }

                    return std::static_pointer_cast<TileFeatureLayer>(result);
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
                (void) baseTile->setStrings(auxBaseStringPool);
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
    : impl_(std::make_unique<Impl>(std::move(cache), useDataSourceConfig, defaultTtl))
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
        auto context = resolveLayerRequest(r->mapId_, r->layerId_, clientHeaders);
        switch (context.status_) {
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
        default: {
            // Nothing to do.
            r->prepareResolvedLayer(context.layerType_, context.stages_);
            if (r->isDone()) {
                r->notifyStatus();
            }
        }
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
            if (!r->isDone()) {
                impl_->addRequest(r);
            }
        }
    }
    return dataSourcesAvailable;
}

std::vector<LocateResponse> Service::locate(LocateRequest const& req)
{
    std::vector<LocateResponse> results;
    std::vector<std::pair<DataSource::Ptr, DataSourceInfo>> dataSources;
    {
        std::shared_lock lock(impl_->dataSourcesMutex_);
        dataSources.assign(impl_->dataSourceInfo_.begin(), impl_->dataSourceInfo_.end());
    }

    for (auto const& [ds, info] : dataSources)
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
    return resolveLayerRequest(mapId, layerId, clientHeaders).status_;
}

LayerRequestContext Service::resolveLayerRequest(
    std::string const& mapId,
    std::string const& layerId,
    std::optional<AuthHeaders> const& clientHeaders) const
{
    LayerRequestContext result;

    std::shared_lock lock(impl_->dataSourcesMutex_);
    bool layerExists = false;
    bool unauthorized = false;
    bool foundAuthorizedLayer = false;
    for (auto const& [ds, info] : impl_->dataSourceInfo_) {
        if (mapId != info.mapId_)
            continue;

        auto layerIt = info.layers_.find(layerId);
        if (layerIt == info.layers_.end())
            continue;

        layerExists = true;
        if (clientHeaders && !ds->isDataSourceAuthorized(*clientHeaders)) {
            unauthorized = true;
            continue;
        }

        if (!foundAuthorizedLayer) {
            result.status_ = RequestStatus::Success;
            result.layerType_ = layerIt->second->type_;
            result.stages_ = std::max<uint32_t>(1U, layerIt->second->stages_);
            foundAuthorizedLayer = true;
            continue;
        }

        if (result.layerType_ != layerIt->second->type_) {
            log().warn(
                "Conflicting layer types for {}::{} across data sources ({} vs {}).",
                mapId,
                layerId,
                static_cast<int>(result.layerType_),
                static_cast<int>(layerIt->second->type_));
        }
        result.stages_ = std::max<uint32_t>(
            result.stages_,
            std::max<uint32_t>(1U, layerIt->second->stages_));
    }

    if (foundAuthorizedLayer) {
        return result;
    }

    if (layerExists && unauthorized) {
        result.status_ = RequestStatus::Unauthorized;
    }
    else {
        result.status_ = RequestStatus::NoDataSource;
    }
    return result;
}

namespace
{

[[nodiscard]] nlohmann::json buildTileSizeDistribution(std::vector<int64_t> tileSizes)
{
    if (tileSizes.empty())
        return nlohmann::json::object();

    std::sort(tileSizes.begin(), tileSizes.end());

    const int64_t totalBytes = std::accumulate(tileSizes.begin(), tileSizes.end(), int64_t{0});
    const int64_t tileCount = static_cast<int64_t>(tileSizes.size());
    const int64_t meanBytes = totalBytes / tileCount;

    struct HistogramBin {
        int64_t upperBound;
        const char* label;
        int64_t count = 0;
    };
    std::vector<HistogramBin> bins = {
        {16 * 1024, "<=16 KiB"},
        {32 * 1024, "16-32 KiB"},
        {64 * 1024, "32-64 KiB"},
        {128 * 1024, "64-128 KiB"},
        {256 * 1024, "128-256 KiB"},
        {512 * 1024, "256-512 KiB"},
        {1024 * 1024, "512 KiB-1 MiB"},
        {2 * 1024 * 1024, "1-2 MiB"},
        {4 * 1024 * 1024, "2-4 MiB"},
    };
    int64_t overflowCount = 0;

    for (const auto bytes : tileSizes) {
        bool assigned = false;
        for (auto& bin : bins) {
            if (bytes <= bin.upperBound) {
                ++bin.count;
                assigned = true;
                break;
            }
        }
        if (!assigned) {
            ++overflowCount;
        }
    }

    auto histogram = nlohmann::json::array();
    for (const auto& bin : bins) {
        histogram.push_back(nlohmann::json::object({
            {"label", bin.label},
            {"count", bin.count},
        }));
    }
    histogram.push_back(nlohmann::json::object({
        {"label", ">4 MiB"},
        {"count", overflowCount},
    }));

    return nlohmann::json::object({
        {"tile-count", tileCount},
        {"total-tile-bytes", totalBytes},
        {"min-bytes", tileSizes.front()},
        {"max-bytes", tileSizes.back()},
        {"mean-bytes", meanBytes},
        {"histogram", std::move(histogram)},
    });
}

}  // namespace

nlohmann::json Service::getStatistics() const
{
    // Preserve old behavior for existing callers.
    return getStatistics(true, false);
}

nlohmann::json Service::getStatistics(bool includeCachedFeatureTreeBytes, bool includeTileSizeDistribution) const
{
    std::vector<std::pair<DataSourceInfo, size_t>> dataSources;
    {
        std::shared_lock lock(impl_->dataSourcesMutex_);
        dataSources.reserve(impl_->dataSourceInfo_.size());
        for (auto const& [dataSource, info] : impl_->dataSourceInfo_) {
            auto workersIt = impl_->dataSourceWorkers_.find(dataSource);
            auto workerCount = workersIt == impl_->dataSourceWorkers_.end()
                ? size_t{0}
                : workersIt->second.size();
            dataSources.emplace_back(info, workerCount);
        }
    }

    auto datasources = nlohmann::json::array();
    for (auto const& [info, workerCount] : dataSources) {
        datasources.push_back({
            {"name", info.mapId_},
            {"workers", workerCount}
        });
    }

    size_t activeRequests = 0;
    {
        std::unique_lock lock(impl_->jobsMutex_);
        activeRequests = impl_->requests_.size();
    }
    auto result = nlohmann::json{
        {"datasources", datasources},
        {"active-requests", activeRequests}
    };

    if (!includeCachedFeatureTreeBytes && !includeTileSizeDistribution) {
        return result;
    }

    auto featureLayerTotals = nlohmann::json::object();
    auto modelPoolTotals = nlohmann::json::object();
    auto geometryUsageTotals = nlohmann::json::object();
    auto validityUsageTotals = nlohmann::json::object();
    auto arrayArenaSingletonTotals = nlohmann::json::object();
    int64_t parsedTiles = 0;
    int64_t totalTileBytes = 0;
    int64_t parseErrors = 0;
    std::vector<int64_t> tileSizes;

    auto addTotals = [](nlohmann::json& totals, const nlohmann::json& stats, const auto& self) -> void {
        for (const auto& [key, value] : stats.items()) {
            if (value.is_number_integer()) {
                totals[key] = totals.template value<int64_t>(key, 0) + value.template get<int64_t>();
            } else if (value.is_number_float()) {
                totals[key] = totals.template value<double>(key, .0) + value.template get<double>();
            } else if (value.is_object()) {
                if (!totals.contains(key) || !totals[key].is_object()) {
                    totals[key] = nlohmann::json::object();
                }
                self(totals[key], value, self);
            }
        }
    };

    std::unique_ptr<TileLayerStream::Reader> tileReader;
    if (includeCachedFeatureTreeBytes) {
        auto layerInfoByMap =
            std::unordered_map<std::string, std::unordered_map<std::string, std::shared_ptr<LayerInfo>>>{};
        std::vector<DataSourceInfo> infos;
        {
            std::shared_lock lock(impl_->dataSourcesMutex_);
            infos.reserve(impl_->dataSourceInfo_.size());
            for (auto const& [_, info] : impl_->dataSourceInfo_) {
                infos.push_back(info);
            }
        }
        for (auto const& info : infos) {
            auto& layers = layerInfoByMap[info.mapId_];
            for (auto const& [layerId, layerInfo] : info.layers_) {
                layers[layerId] = layerInfo;
            }
        }

        auto resolveLayerInfo = [layerInfoByMap](std::string_view mapId, std::string_view layerId)
            -> std::shared_ptr<LayerInfo> {
            auto mapIt = layerInfoByMap.find(std::string(mapId));
            if (mapIt == layerInfoByMap.end())
                return std::make_shared<LayerInfo>();
            auto layerIt = mapIt->second.find(std::string(layerId));
            if (layerIt == mapIt->second.end()) {
                auto fallback = std::make_shared<LayerInfo>();
                fallback->layerId_ = std::string(layerId);
                return fallback;
            }
            return layerIt->second;
        };

        tileReader = std::make_unique<TileLayerStream::Reader>(
            resolveLayerInfo,
            [&](auto&& parsedLayer) {
                auto tile = std::dynamic_pointer_cast<mapget::TileFeatureLayer>(parsedLayer);
                if (!tile) {
                    ++parseErrors;
                    return;
                }
                auto sizeStats = tile->serializationSizeStats();
                addTotals(featureLayerTotals, sizeStats["feature-layer"], addTotals);
                addTotals(modelPoolTotals, sizeStats["model-pool"], addTotals);
                addTotals(geometryUsageTotals, sizeStats["geometry-usage"], addTotals);
                addTotals(validityUsageTotals, sizeStats["validity-usage"], addTotals);
                addTotals(arrayArenaSingletonTotals, sizeStats["array-arena-singletons"], addTotals);
            },
            impl_->cache_);
    }

    impl_->cache_->forEachTileLayerBlob([&](const MapTileKey& key, const std::string& blob) {
        if (key.layer_ != LayerType::Features)
            return;

        const int64_t tileBytes = static_cast<int64_t>(blob.size());
        ++parsedTiles;
        totalTileBytes += tileBytes;

        if (includeTileSizeDistribution) {
            tileSizes.push_back(tileBytes);
        }

        if (!includeCachedFeatureTreeBytes) {
            return;
        }

        try {
            tileReader->read(blob);
        } catch (const std::exception&) {
            ++parseErrors;
        }
    });

    if (includeCachedFeatureTreeBytes && parsedTiles > 0) {
        result["cached-feature-tree-bytes"] = nlohmann::json{
            {"tile-count", parsedTiles},
            {"total-tile-bytes", totalTileBytes},
            {"parse-errors", parseErrors},
            {"feature-layer", featureLayerTotals},
            {"model-pool", modelPoolTotals},
            {"geometry-usage", geometryUsageTotals},
            {"validity-usage", validityUsageTotals},
            {"array-arena-singletons", arrayArenaSingletonTotals}
        };
    }

    if (includeTileSizeDistribution && !tileSizes.empty()) {
        result["cached-feature-tile-size-distribution"] = buildTileSizeDistribution(std::move(tileSizes));
    }

    return result;
}

}  // namespace mapget
