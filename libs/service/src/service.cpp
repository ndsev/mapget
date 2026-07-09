#include "service.h"

#include "fmt/format.h"
#include "locate.h"
#include "config.h"
#include "mapget/log.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/info.h"
#include "mapget/model/layer.h"
#include "mapget/model/stream.h"

#include <memory>
#include <optional>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <list>
#include <chrono>
#include <cmath>
#include <cctype>
#include <exception>
#include <shared_mutex>
#include <algorithm>
#include <numeric>
#include <regex>
#include <unordered_map>
#include <utility>

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

bool isDataSourceDescriptorEnabled(YAML::Node const& descriptor)
{
    if (!descriptor["enabled"].IsDefined()) {
        return true;
    }
    try {
        return descriptor["enabled"].as<bool>(true);
    } catch (...) {
        return true;
    }
}

bool isDescriptorAuthorized(DataSourceDescriptor const& descriptor, std::optional<AuthHeaders> const& clientHeaders)
{
    if (!clientHeaders || descriptor.authHeaderAlternatives.empty()) {
        return true;
    }

    for (auto const& [k, v] : *clientHeaders) {
        auto key = k;
        std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto authHeaderPatternIt = descriptor.authHeaderAlternatives.find(key);
        if (authHeaderPatternIt != descriptor.authHeaderAlternatives.end()
            && std::regex_match(v, authHeaderPatternIt->second))
        {
            return true;
        }
    }

    return false;
}

std::optional<float> normalizeProgressPercentage(std::optional<float> progress)
{
    if (!progress || !std::isfinite(*progress)) {
        return std::nullopt;
    }
    return std::clamp(*progress, 0.0f, 100.0f);
}

DataSourceCatalogSourceUpdate makeSourceCatalogSourceUpdate(DataSourceCatalogEntry const& entry)
{
    return DataSourceCatalogSourceUpdate{
        .descriptor = entry.descriptor,
        .status = entry.status,
        .statusMessage = entry.statusMessage,
        .progress = entry.progress,
        .dataSource = entry.dataSource};
}

/** Build the common search-status payload, omitting interactive-only fields when absent. */
nlohmann::json makeSearchStatusJson(
    FeatureLayerSearchTilesRequest const& request,
    std::string state)
{
    auto status = nlohmann::json::object({
        {"type", "mapget.search.status"},
        {"mapId", request.mapId_},
        {"layerId", request.layerId_},
        {"state", std::move(state)},
    });
    if (!request.search_.searchId_.empty()) {
        status["searchId"] = request.search_.searchId_;
        status["refresh"] = request.search_.refresh_.value_or(0);
    }
    if (!request.search_.requestKey_.empty()) {
        status["requestKey"] = request.search_.requestKey_;
    }
    return status;
}

/** Create a detached LayerInfo copy without retaining datasource-owned schema emitters. */
std::shared_ptr<LayerInfo> cloneLayerInfo(LayerInfo const& info)
{
    auto result = std::make_shared<LayerInfo>();
    result->layerId_ = info.layerId_;
    result->type_ = info.type_;
    result->featureTypes_ = info.featureTypes_;
    result->zoomLevels_ = info.zoomLevels_;
    result->coverage_ = info.coverage_;
    result->stages_ = info.stages_;
    result->stageLabels_ = info.stageLabels_;
    result->highFidelityStage_ = info.highFidelityStage_;
    result->canRead_ = info.canRead_;
    result->canWrite_ = info.canWrite_;
    result->version_ = info.version_;
    result->featureModelSchema_ = info.featureModelSchema_
        ? info.featureModelSchema_->detachedCopy()
        : nullptr;
    return result;
}

/** Create a service-owned metadata snapshot, detached from datasource internals. */
DataSourceInfo cloneDataSourceInfo(DataSourceInfo const& info)
{
    info.validateIdentifiers();

    auto result = info;
    if (result.protocolVersion_ == Version{}) {
        result.protocolVersion_ = TileLayerStream::CurrentProtocolVersion;
    }
    result.layers_.clear();
    result.layers_.reserve(info.layers_.size());
    for (auto const& [layerId, layerInfo] : info.layers_) {
        if (!layerInfo) {
            raise(fmt::format(
                "Datasource '{}' has null LayerInfo for layer '{}'.",
                info.nodeId_,
                layerId));
        }
        result.layers_.try_emplace(layerId, cloneLayerInfo(*layerInfo));
    }
    return result;
}

}  // namespace

LayerTilesRequest::LayerTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<TileId> tiles)
    : LayerTilesRequest(
          std::move(mapId),
          std::move(layerId),
          std::move(tiles),
          {})
{
}

LayerTilesRequest::LayerTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<TileId> tiles,
    std::vector<TileId> const& priorityTileIds)
    : LayerTilesRequest(
          std::move(mapId),
          std::move(layerId),
          std::vector<std::vector<TileId>>{std::move(tiles)},
          std::move(priorityTileIds))
{
    usesStageBuckets_ = false;
}

LayerTilesRequest::LayerTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<std::vector<TileId>> tileIdsByNextStage)
    : LayerTilesRequest(
          std::move(mapId),
          std::move(layerId),
          std::move(tileIdsByNextStage),
          {})
{
}

LayerTilesRequest::LayerTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<std::vector<TileId>> tileIdsByNextStage,
    std::vector<TileId> const& priorityTileIds)
    : mapId_(std::move(mapId)),
      layerId_(std::move(layerId)),
      tileIdsByNextStage_(normalizeTileBuckets(std::move(tileIdsByNextStage))),
      priorityTileIds_({priorityTileIds.begin(), priorityTileIds.end()})
{
    usesStageBuckets_ = true;
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
    const auto isPriorityTile = [this](TileId const& tileId) {
        return priorityTileIds_.find(tileId) != priorityTileIds_.end();
    };
    const auto appendKey = [this](MapTileKey key) {
        if (tileKeysNotStarted_.insert(key).second) {
            resolvedTileKeys_.push_back(std::move(key));
        }
    };

    if (!usesStageBuckets_) {
        const auto appendUnstagedTiles = [&](std::optional<bool> priorityFilter) {
            if (tileIdsByNextStage_.empty()) {
                return;
            }
            for (auto const& tileId : tileIdsByNextStage_.front()) {
                if (priorityFilter && isPriorityTile(tileId) != *priorityFilter) {
                    continue;
                }
                MapTileKey key(
                    layerType,
                    mapId_,
                    layerId_,
                    tileId,
                    UnspecifiedStage);
                appendKey(std::move(key));
            }
        };

        if (priorityTileIds_.empty()) {
            appendUnstagedTiles(std::nullopt);
        } else {
            appendUnstagedTiles(true);
            appendUnstagedTiles(false);
        }
        status_ = resolvedTileKeys_.empty() ? RequestStatus::Success : RequestStatus::Open;
        return;
    }

    const auto appendTileMajorStagedTiles = [&](std::optional<bool> priorityFilter) {
        for (size_t bucketIndex = 0; bucketIndex < tileIdsByNextStage_.size(); ++bucketIndex) {
            const auto nextMissingStage = static_cast<uint32_t>(bucketIndex);
            if (nextMissingStage >= normalizedStages) {
                continue;
            }
            for (auto const& tileId : tileIdsByNextStage_[bucketIndex]) {
                if (priorityFilter && isPriorityTile(tileId) != *priorityFilter) {
                    continue;
                }
                for (uint32_t stage = nextMissingStage; stage < normalizedStages; ++stage) {
                    appendKey(MapTileKey(layerType, mapId_, layerId_, tileId, stage));
                }
            }
        }
    };

    const auto appendStagedTiles = [&](std::optional<bool> priorityFilter) {
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
                    if (priorityFilter && isPriorityTile(tileId) != *priorityFilter) {
                        continue;
                    }
                    appendKey(MapTileKey(layerType, mapId_, layerId_, tileId, stage));
                }
            }
        }
    };

    if (priorityTileIds_.empty()) {
        if (preferCompleteStagedTiles_) {
            // Search cannot emit a result until all stages of a tile are
            // present. Tile-major ordering prevents retaining one partial
            // stage for every requested tile in large searches.
            appendTileMajorStagedTiles(std::nullopt);
        } else {
            appendStagedTiles(std::nullopt);
        }
    } else {
        if (preferCompleteStagedTiles_) {
            // Keep foreground tiles first while still completing each tile
            // before advancing to the next one.
            appendTileMajorStagedTiles(true);
            appendTileMajorStagedTiles(false);
        } else {
            appendStagedTiles(true);
            appendStagedTiles(false);
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

    if (!usesStageBuckets_) {
        auto tileIds = nlohmann::json::array();
        if (!tileIdsByNextStage_.empty()) {
            for (auto const& tileId : tileIdsByNextStage_.front()) {
                tileIds.emplace_back(tileId.value());
            }
        }
        requestJson["tileIds"] = std::move(tileIds);
        if (!priorityTileIds_.empty()) {
            auto priorityTileIds = nlohmann::json::array();
            for (auto const& tileId : priorityTileIds_) {
                priorityTileIds.emplace_back(tileId.value());
            }
            requestJson["priorityTileIds"] = std::move(priorityTileIds);
        }
        return requestJson;
    }

    auto tileIdsByNextStage = nlohmann::json::array();
    for (auto const& bucket : tileIdsByNextStage_) {
        auto tileIds = nlohmann::json::array();
        for (auto const& tileId : bucket) {
            tileIds.emplace_back(tileId.value());
        }
        tileIdsByNextStage.push_back(std::move(tileIds));
    }
    requestJson["tileIdsByNextStage"] = std::move(tileIdsByNextStage);
    if (!priorityTileIds_.empty()) {
        auto priorityTileIds = nlohmann::json::array();
        for (auto const& tileId : priorityTileIds_) {
            priorityTileIds.emplace_back(tileId.value());
        }
        requestJson["priorityTileIds"] = std::move(priorityTileIds);
    }
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

FeatureLayerSearchTilesRequest::FeatureLayerSearchTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<TileId> tiles,
    FeatureLayerSearchRequest search)
    : FeatureLayerSearchTilesRequest(
          std::move(mapId),
          std::move(layerId),
          std::move(tiles),
          std::move(search),
          {})
{
}

FeatureLayerSearchTilesRequest::FeatureLayerSearchTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<TileId> tiles,
    FeatureLayerSearchRequest search,
    std::vector<TileId> const& priorityTileIds)
    : mapId_(std::move(mapId)),
      layerId_(std::move(layerId)),
      tileIds_(std::move(tiles)),
      priorityTileIds_({priorityTileIds.begin(), priorityTileIds.end()}),
      search_(std::move(search))
{
    std::set<TileId> seenTileIds;
    std::vector<TileId> uniqueTileIds;
    uniqueTileIds.reserve(tileIds_.size());
    for (auto const& tileId : tileIds_) {
        if (seenTileIds.insert(tileId).second) {
            uniqueTileIds.push_back(tileId);
        }
    }
    tileIds_.swap(uniqueTileIds);
    if (tileIds_.empty()) {
        status_ = RequestStatus::Success;
    }
}

RequestStatus FeatureLayerSearchTilesRequest::getStatus()
{
    return status_;
}

bool FeatureLayerSearchTilesRequest::isDone()
{
    return status_ != RequestStatus::Open;
}

bool FeatureLayerSearchTilesRequest::isCancelled() const
{
    return cancelled_;
}

void FeatureLayerSearchTilesRequest::wait()
{
    std::unique_lock doneLock(statusMutex_);
    if (!isDone()) {
        statusConditionVariable_.wait(doneLock, [this] { return isDone(); });
    }
}

void FeatureLayerSearchTilesRequest::notifyResult(TileSearchResultLayer::Ptr result)
{
    if (cancelled_ || isDone()) {
        return;
    }
    if (onSearchResult_) {
        onSearchResult_(std::move(result));
    }
}

void FeatureLayerSearchTilesRequest::notifyProgress(nlohmann::json const& status)
{
    if (cancelled_) {
        return;
    }
    if (onStatus_) {
        onStatus_(status);
    }
}

void FeatureLayerSearchTilesRequest::setStatus(RequestStatus s)
{
    auto const previous = status_.exchange(s);
    if (previous != RequestStatus::Open) {
        return;
    }
    notifyStatus();
}

void FeatureLayerSearchTilesRequest::notifyStatus()
{
    if (isDone() && onDone_) {
        onDone_(status_);
    }
    statusConditionVariable_.notify_all();
}

void FeatureLayerSearchTilesRequest::cancel()
{
    cancelled_ = true;
    setStatus(RequestStatus::Aborted);
}

struct Service::Controller
{
    virtual ~Controller() = default;

    struct Job {
        MapTileKey tileKey;
        std::vector<LayerTilesRequest::Ptr> waitingRequests;
        std::optional<std::chrono::system_clock::time_point> cacheExpiredAt;
        TileLayer::LoadState loadStatus = TileLayer::LoadState::LoadingQueued;
        std::function<void()> searchWork;
    };

    struct SearchEvalWork {
        DataSourceInfo dataSourceInfo;
        std::weak_ptr<FeatureLayerSearchTilesRequest> owner;
        std::function<void()> work;
        std::function<void()> discard;
    };

    std::map<MapTileKey, std::shared_ptr<Job>> jobsInProgress_;    // Jobs currently in progress + interested requests
    Cache::Ptr cache_;                       // The cache for the service
    std::optional<std::chrono::milliseconds> defaultTtl_; // Default TTL applied when datasource does not override
    std::list<LayerTilesRequest::Ptr> requests_;       // List of requests currently being processed
    std::list<SearchEvalWork> searchEvalJobs_; // Derived search jobs scheduled after source stages are loaded.
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

    [[nodiscard]] std::optional<Candidate> nextCandidateInRequestOrder(DataSourceInfo const& info) const
    {
        for (auto reqIt = requests_.begin(); reqIt != requests_.end(); ++reqIt) {
            auto const& request = *reqIt;
            if (!requestMatchesDataSource(request, info))
                continue;

            auto pendingIndex = nextPendingTileKey(*request);
            if (!pendingIndex)
                continue;
            auto pendingKey = request->resolvedTileKeys_[*pendingIndex];

            return Candidate{
                .requestIt_ = reqIt,
                .request_ = request,
                .tileKey_ = pendingKey,
                .nextTileIndex_ = *pendingIndex,
            };
        }

        return {};
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
        DataSourceInfo const& info,
        std::unique_lock<std::mutex>& lock)
    {
        // Commit this candidate as "consumed" for the request before dispatching it.
        // The tile is then either satisfied immediately (cache/in-progress) or started.
        auto const& request = candidate.request_;
        request->nextTileIndex_ = candidate.nextTileIndex_;
        request->tileKeysNotStarted_.erase(candidate.tileKey_);

        // Rotate on every consumed candidate. Cache hits and in-progress joins
        // should participate in fairness just like newly started backend work.
        requests_.splice(requests_.end(), requests_, candidate.requestIt_);

        auto cachedResult = cache_->getTileLayer(candidate.tileKey_, info);
        if (cachedResult.tile) {
            log().debug("Serving cached tile: {}", candidate.tileKey_.toString());
            // Cached-result callbacks may do non-trivial derived work; never run
            // them while the scheduler mutex is held.
            lock.unlock();
            request->notifyResult(cachedResult.tile);
            lock.lock();
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
        log().debug("Working on tile: {}", startedJob->tileKey.toString());

        return startedJob;
    }

    [[nodiscard]] std::shared_ptr<Job> nextSearchEvalJob(DataSourceInfo const& info)
    {
        for (auto it = searchEvalJobs_.begin(); it != searchEvalJobs_.end(); ++it) {
            if (it->dataSourceInfo.nodeId_ != info.nodeId_) {
                continue;
            }
            if (auto owner = it->owner.lock(); !owner || owner->isDone()) {
                // The owning request already reached a terminal state; dropping
                // the closure releases any staged tiles it captured.
                it = searchEvalJobs_.erase(it);
                continue;
            }
            auto work = std::move(it->work);
            searchEvalJobs_.erase(it);
            auto job = std::make_shared<Job>();
            job->searchWork = std::move(work);
            return job;
        }
        return {};
    }

    void enqueueSearchEvalJob(
        DataSourceInfo dataSourceInfo,
        FeatureLayerSearchTilesRequest::Ptr const& owner,
        std::function<void()> work,
        std::function<void()> discard)
    {
        if (!work) {
            return;
        }
        {
            std::unique_lock lock(jobsMutex_);
            searchEvalJobs_.push_back(SearchEvalWork{
                std::move(dataSourceInfo),
                owner,
                std::move(work),
                std::move(discard)});
        }
        jobsAvailable_.notify_all();
    }

    void abortSearchEvalJobs(FeatureLayerSearchTilesRequest::Ptr const& request)
    {
        std::vector<std::function<void()>> discarded;
        {
            std::unique_lock lock(jobsMutex_);
            for (auto it = searchEvalJobs_.begin(); it != searchEvalJobs_.end();) {
                if (it->owner.lock() != request) {
                    ++it;
                    continue;
                }
                if (it->discard) {
                    discarded.push_back(std::move(it->discard));
                }
                it = searchEvalJobs_.erase(it);
            }
        }
        for (auto& discard : discarded) {
            discard();
        }
        jobsAvailable_.notify_all();
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
            if (auto searchJob = nextSearchEvalJob(i)) {
                return searchJob;
            }

            // 1) Pick the next pending tile from the first matching request.
            auto candidate = nextCandidateInRequestOrder(i);
            if (!candidate)
                break;

            // 2) Dispatch that tile: cache hit, join running job, or start backend work.
            if (auto dispatchResult = dispatchCandidate(*candidate, i, lock)) {
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
            if (job.searchWork) {
                job.searchWork();
                controller_.jobsAvailable_.notify_all();
                return true;
            }

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
            if (job.searchWork) {
                log().error("Could not evaluate search job: {}", e.what());
            } else {
                log().error("Could not load tile {}: {}",
                    job.tileKey.toString(),
                    e.what());
            }
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
    size_t dataSourceConstructionFailed_ = 0;
    std::vector<DataSourceCatalogEntry> sourceCatalog_;
    uint64_t sourceCatalogGeneration_ = 0;
    uint64_t sourceCatalogRevision_ = 0;
    std::string sourceConfigStatus_ = "ok";
    std::string sourceConfigStatusMessage_;
    mutable std::condition_variable_any sourceCatalogReadyCv_;

    struct ConstructionThread {
        // Use explicit cancellation instead of std::jthread; macOS CI's libc++ does not expose it yet.
        std::thread thread;
        std::shared_ptr<std::atomic_bool> done;
        std::shared_ptr<std::atomic_bool> stopRequested;
    };

    std::vector<ConstructionThread> dataSourceConstructionThreads_;
    std::mutex constructionSlotMutex_;
    std::condition_variable_any constructionSlotCv_;
    size_t activeDataSourceConstructions_ = 0;
    size_t maxConcurrentDataSourceConstructions_ = std::max<size_t>(
        1,
        std::min<size_t>(4, std::max<unsigned>(1, std::thread::hardware_concurrency())));
    std::atomic_bool shuttingDown_{false};

    mutable std::mutex sourceCatalogCallbacksMutex_;
    std::map<uint64_t, Service::DataSourceCatalogCallback> sourceCatalogCallbacks_;
    uint64_t nextSourceCatalogCallbackId_ = 1;

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
                applyDataSourceConfig(dataSourceConfigNodes);
            },
            [this](std::string const& error)
            {
                DataSourceCatalogChange change;
                {
                    std::unique_lock lock(dataSourcesMutex_);
                    sourceConfigStatus_ = "error";
                    sourceConfigStatusMessage_ = error;
                    change = markSourceCatalogChangedLocked("config-error");
                }
                notifySourceCatalogChanged(change);
            });
    }

    ~Impl() override
    {
        // Ensure that no new datasources are added while we are cleaning up.
        shuttingDown_ = true;
        sourceCatalogReadyCv_.notify_all();
        configSubscription_.reset();

        std::vector<ConstructionThread> constructionThreadsToJoin;
        {
            std::unique_lock lock(dataSourcesMutex_);
            requestStopForConstructionThreadsLocked();
            constructionThreadsToJoin.swap(dataSourceConstructionThreads_);
        }
        constructionSlotCv_.notify_all();
        for (auto& constructionThread : constructionThreadsToJoin) {
            if (constructionThread.thread.joinable()) {
                constructionThread.thread.join();
            }
        }

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
            dataSourceConstructionFailed_ = 0;
        }
        // Wake up all workers to check shouldTerminate_.
        jobsAvailable_.notify_all();

        for (auto& worker : workersToJoin) {
            if (worker->thread_.joinable()) {
                worker->thread_.join();
            }
        }
    }

    [[nodiscard]] DataSourceCatalogChange markSourceCatalogChangedLocked(
        std::string reason,
        DataSourceCatalogEntry const* sourceUpdate = nullptr)
    {
        auto change = DataSourceCatalogChange{
            .revision = ++sourceCatalogRevision_,
            .reason = std::move(reason)};
        if (sourceUpdate) {
            change.sourceUpdate = makeSourceCatalogSourceUpdate(*sourceUpdate);
        }
        return change;
    }

    /** True once default `/sources` may expose the current catalog without legacy-visible partial reloads. */
    [[nodiscard]] bool sourceCatalogReloadDoneLocked() const
    {
        return shuttingDown_.load(std::memory_order_relaxed)
            || std::ranges::none_of(sourceCatalog_, [](auto const& entry) {
                return entry.status == DataSourceCatalogStatus::Initializing;
            });
    }

    void notifySourceCatalogChanged(DataSourceCatalogChange const& change)
    {
        sourceCatalogReadyCv_.notify_all();

        std::vector<Service::DataSourceCatalogCallback> callbacks;
        {
            std::lock_guard lock(sourceCatalogCallbacksMutex_);
            callbacks.reserve(sourceCatalogCallbacks_.size());
            for (auto const& [_, callback] : sourceCatalogCallbacks_) {
                callbacks.push_back(callback);
            }
        }

        for (auto const& callback : callbacks) {
            try {
                callback(change);
            }
            catch (std::exception const& e) {
                log().warn("Datasource catalog callback failed: {}", e.what());
            }
        }
    }

    void pruneCompletedConstructionThreadsLocked()
    {
        std::erase_if(dataSourceConstructionThreads_, [](ConstructionThread& constructionThread) {
            if (!constructionThread.done || !constructionThread.done->load(std::memory_order_acquire)) {
                return false;
            }
            if (constructionThread.thread.joinable()) {
                constructionThread.thread.join();
            }
            return true;
        });
    }

    void requestStopForConstructionThreadsLocked()
    {
        for (auto& constructionThread : dataSourceConstructionThreads_) {
            if (constructionThread.stopRequested) {
                constructionThread.stopRequested->store(true, std::memory_order_release);
            }
        }
        constructionSlotCv_.notify_all();
    }

    bool acquireConstructionSlot(std::shared_ptr<std::atomic_bool> const& stopRequested)
    {
        std::unique_lock lock(constructionSlotMutex_);
        constructionSlotCv_.wait(lock, [this, &stopRequested]() {
            return shuttingDown_.load(std::memory_order_relaxed)
                || (stopRequested && stopRequested->load(std::memory_order_acquire))
                || activeDataSourceConstructions_ < maxConcurrentDataSourceConstructions_;
        });
        if ((stopRequested && stopRequested->load(std::memory_order_acquire))
            || shuttingDown_.load(std::memory_order_relaxed)) {
            return false;
        }
        ++activeDataSourceConstructions_;
        return true;
    }

    void releaseConstructionSlot()
    {
        {
            std::lock_guard lock(constructionSlotMutex_);
            if (activeDataSourceConstructions_ > 0) {
                --activeDataSourceConstructions_;
            }
        }
        constructionSlotCv_.notify_all();
    }

    [[nodiscard]] bool isCurrentCatalogGeneration(uint64_t generation) const
    {
        std::shared_lock lock(dataSourcesMutex_);
        return generation == sourceCatalogGeneration_ && !shuttingDown_.load(std::memory_order_relaxed);
    }

    void updateCatalogStatusMessage(uint64_t generation, std::string const& sourceId, std::string message)
    {
        DataSourceCatalogChange change;
        bool changed = false;
        {
            std::unique_lock lock(dataSourcesMutex_);
            if (generation != sourceCatalogGeneration_) {
                return;
            }
            auto it = std::ranges::find_if(sourceCatalog_, [&](auto const& entry) {
                return entry.descriptor.sourceId == sourceId;
            });
            if (it == sourceCatalog_.end() || it->statusMessage == message) {
                return;
            }
            it->statusMessage = std::move(message);
            change = markSourceCatalogChangedLocked("status-message", &*it);
            changed = true;
        }
        if (changed) {
            notifySourceCatalogChanged(change);
        }
    }

    void updateCatalogProgress(uint64_t generation, std::string const& sourceId, std::optional<float> progress)
    {
        DataSourceCatalogChange change;
        bool changed = false;
        progress = normalizeProgressPercentage(progress);
        {
            std::unique_lock lock(dataSourcesMutex_);
            if (generation != sourceCatalogGeneration_) {
                return;
            }
            auto it = std::ranges::find_if(sourceCatalog_, [&](auto const& entry) {
                return entry.descriptor.sourceId == sourceId;
            });
            if (it == sourceCatalog_.end() || it->progress == progress) {
                return;
            }
            it->progress = progress;
            change = markSourceCatalogChangedLocked("progress", &*it);
            changed = true;
        }
        if (changed) {
            notifySourceCatalogChanged(change);
        }
    }

    void markCatalogConstructionFailed(
        uint64_t generation,
        std::string const& sourceId,
        std::string message)
    {
        DataSourceCatalogChange change;
        bool changed = false;
        {
            std::unique_lock lock(dataSourcesMutex_);
            if (generation != sourceCatalogGeneration_) {
                return;
            }
            auto it = std::ranges::find_if(sourceCatalog_, [&](auto const& entry) {
                return entry.descriptor.sourceId == sourceId;
            });
            if (it == sourceCatalog_.end()) {
                return;
            }
            it->status = DataSourceCatalogStatus::Failed;
            it->statusMessage = std::move(message);
            it->progress.reset();
            ++dataSourceConstructionFailed_;
            change = markSourceCatalogChangedLocked("status", &*it);
            changed = true;
        }
        if (changed) {
            notifySourceCatalogChanged(change);
        }
    }

    void markCatalogConstructionReady(
        uint64_t generation,
        std::string const& sourceId,
        DataSource::Ptr dataSource,
        DataSourceInfo info)
    {
        DataSourceCatalogChange change;
        bool changed = false;
        {
            std::unique_lock lock(dataSourcesMutex_);
            if (generation != sourceCatalogGeneration_) {
                return;
            }
            auto it = std::ranges::find_if(sourceCatalog_, [&](auto const& entry) {
                return entry.descriptor.sourceId == sourceId;
            });
            if (it == sourceCatalog_.end()) {
                return;
            }
            it->status = DataSourceCatalogStatus::Ready;
            it->statusMessage.clear();
            it->progress.reset();
            it->dataSource = std::move(dataSource);
            it->info = cloneDataSourceInfo(info);
            dataSourcesFromConfig_.push_back(it->dataSource);
            change = markSourceCatalogChangedLocked("status", &*it);
            changed = true;
        }
        if (changed) {
            notifySourceCatalogChanged(change);
        }
    }

    void launchDataSourceConstruction(
        uint64_t generation,
        YAML::Node configNode,
        DataSourceDescriptor descriptor)
    {
        auto done = std::make_shared<std::atomic_bool>(false);
        auto stopRequested = std::make_shared<std::atomic_bool>(false);
        auto sourceId = descriptor.sourceId;
        auto configIndex = descriptor.configIndex;
        dataSourceConstructionThreads_.push_back(ConstructionThread{
            .thread = std::thread(
                [this, generation, configNode = std::move(configNode), sourceId, configIndex, done, stopRequested]() mutable
                {
                    bool slotAcquired = false;
                    auto finish = [&]() {
                        if (slotAcquired) {
                            releaseConstructionSlot();
                        }
                        done->store(true, std::memory_order_release);
                    };

                    try {
                        slotAcquired = acquireConstructionSlot(stopRequested);
                        if (!slotAcquired || !isCurrentCatalogGeneration(generation)) {
                            finish();
                            return;
                        }

                        std::string lastStatusMessage;
                        DataSourceInitContext initContext{
                            .setStatusMessage = [this, generation, sourceId, &lastStatusMessage](std::string message) {
                                lastStatusMessage = message;
                                updateCatalogStatusMessage(generation, sourceId, std::move(message));
                            },
                            .setProgress = [this, generation, sourceId](std::optional<float> progress) {
                                updateCatalogProgress(generation, sourceId, progress);
                            },
                            .isCancelled = [this, generation, stopRequested]() {
                                return stopRequested->load(std::memory_order_acquire)
                                    || !isCurrentCatalogGeneration(generation);
                            }};

                        auto dataSource = DataSourceConfigService::get().makeDataSource(configNode, initContext);
                        if (!dataSource) {
                            if (isCurrentCatalogGeneration(generation)) {
                                if (lastStatusMessage.empty()) {
                                    lastStatusMessage = fmt::format(
                                        "Failed to make datasource at index {}.",
                                        configIndex);
                                }
                                markCatalogConstructionFailed(generation, sourceId, std::move(lastStatusMessage));
                            }
                            finish();
                            return;
                        }

                        if (!isCurrentCatalogGeneration(generation)) {
                            finish();
                            return;
                        }

                        auto info = addDataSource(dataSource, false);
                        if (!isCurrentCatalogGeneration(generation)) {
                            removeDataSource(dataSource, false);
                            finish();
                            return;
                        }
                        markCatalogConstructionReady(generation, sourceId, std::move(dataSource), std::move(info));
                    }
                    catch (std::exception const& e) {
                        if (isCurrentCatalogGeneration(generation)) {
                            markCatalogConstructionFailed(
                                generation,
                                sourceId,
                                fmt::format("Exception while making datasource at index {}: {}", configIndex, e.what()));
                        }
                    }
                    catch (...) {
                        if (isCurrentCatalogGeneration(generation)) {
                            markCatalogConstructionFailed(
                                generation,
                                sourceId,
                                fmt::format("Unknown exception while making datasource at index {}.", configIndex));
                        }
                    }
                    finish();
                }),
            .done = std::move(done),
            .stopRequested = std::move(stopRequested)});
    }

    void applyDataSourceConfig(std::vector<YAML::Node> const& dataSourceConfigNodes)
    {
        std::vector<DataSource::Ptr> previousDataSources;
        std::vector<std::pair<YAML::Node, DataSourceDescriptor>> constructionInputs;
        uint64_t generation = 0;
        DataSourceCatalogChange change;

        {
            std::unique_lock lock(dataSourcesMutex_);
            pruneCompletedConstructionThreadsLocked();
            requestStopForConstructionThreadsLocked();
            previousDataSources.swap(dataSourcesFromConfig_);
            dataSourceConstructionFailed_ = 0;
            sourceCatalog_.clear();
            sourceConfigStatus_ = "ok";
            sourceConfigStatusMessage_.clear();
            generation = ++sourceCatalogGeneration_;

            auto index = uint32_t{0};
            for (auto const& configNode : dataSourceConfigNodes) {
                if (!isDataSourceDescriptorEnabled(configNode)) {
                    ++index;
                    continue;
                }

                auto descriptor = DataSourceConfigService::get().describeDataSource(configNode, index);
                sourceCatalog_.push_back(DataSourceCatalogEntry{
                    .descriptor = descriptor,
                    .status = DataSourceCatalogStatus::Initializing,
                    .statusMessage = "Initializing datasource."});
                constructionInputs.emplace_back(configNode, std::move(descriptor));
                ++index;
            }

            change = markSourceCatalogChangedLocked("reload");
        }

        // Remove previous datasources after publishing the initializing catalog
        // so `/sources` never has to wait for constructor/network teardown.
        log().info("Config changed. Removing previous datasources.");
        for (auto const& datasource : previousDataSources) {
            removeDataSource(datasource, false);
        }
        notifySourceCatalogChanged(change);

        {
            std::unique_lock lock(dataSourcesMutex_);
            for (auto& [configNode, descriptor] : constructionInputs) {
                launchDataSourceConstruction(generation, std::move(configNode), std::move(descriptor));
            }
        }
    }

    DataSourceInfo addDataSource(DataSource::Ptr const& dataSource, bool publishCatalogChange = true)
    {
        if (!dataSource) {
            raise("Tried to add a null data source.");
        }

        auto info = cloneDataSourceInfo(dataSource->info());
        std::unique_lock lock(dataSourcesMutex_);

        if (info.nodeId_.empty()) {
            // Unique node IDs are required for the string pool offsets.
            raise("Tried to create service worker for an unnamed node!");
        }
        for (auto& existingSource : dataSourceInfo_) {
            if (existingSource.second.nodeId_ == info.nodeId_) {
                // Unique node IDs are required for the string pool offsets.
                raise(
                    fmt::format("Data source with node ID '{}' already registered!",
                                info.nodeId_));
            }
        }

        dataSourceInfo_[dataSource] = info;

        // If the datasource is an add-on source, then it
        // does not have separate workers.
        if (info.isAddOn_) {
            addOnDataSources_.emplace_back(dataSource);
            if (publishCatalogChange) {
                auto change = markSourceCatalogChangedLocked("added");
                lock.unlock();
                notifySourceCatalogChanged(change);
            }
            return info;
        }

        auto& workers = dataSourceWorkers_[dataSource];

        // Create workers for this DataSource
        for (auto i = 0; i < info.maxParallelJobs_; ++i)
            workers.emplace_back(std::make_shared<Worker>(
                dataSource,
                info,
                *this));

        if (publishCatalogChange) {
            auto change = markSourceCatalogChangedLocked("added");
            lock.unlock();
            notifySourceCatalogChanged(change);
        }

        return info;
    }

    void removeDataSource(DataSource::Ptr const& dataSource, bool publishCatalogChange = true)
    {
        std::vector<Worker::Ptr> workersToJoin;
        std::optional<DataSourceCatalogChange> change;
        {
            std::unique_lock lock(dataSourcesMutex_);
            dataSourceInfo_.erase(dataSource);
            addOnDataSources_.remove(dataSource);
            if (publishCatalogChange) {
                change = markSourceCatalogChangedLocked("removed");
            }

            auto workers = dataSourceWorkers_.find(dataSource);
            if (workers == dataSourceWorkers_.end()) {
                lock.unlock();
                if (change) {
                    notifySourceCatalogChanged(*change);
                }
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
        if (change) {
            notifySourceCatalogChanged(*change);
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
        std::vector<std::pair<DataSource::Ptr, DataSourceInfo>> dataSources;
        {
            std::shared_lock lock(dataSourcesMutex_);
            dataSources.assign(dataSourceInfo_.begin(), dataSourceInfo_.end());
        }

        std::vector<DataSourceInfo> infos;
        infos.reserve(dataSources.size());
        for (const auto& [dataSource, info] : dataSources) {
            if (!clientHeaders || dataSource->isDataSourceAuthorized(*clientHeaders)) {
                // Return detached LayerInfo snapshots so callers such as
                // `/sources` can serialize without observing datasource-side
                // metadata mutations or holding the registry lock.
                infos.push_back(cloneDataSourceInfo(info));
            }
        }
        return infos;
    }

    DataSourceCatalogSnapshot getSourceCatalog(
        std::optional<AuthHeaders> const& clientHeaders,
        bool waitUntilReloadDone) const
    {
        DataSourceCatalogSnapshot snapshot;
        std::shared_lock lock(dataSourcesMutex_);
        if (waitUntilReloadDone) {
            sourceCatalogReadyCv_.wait(lock, [this] {
                return sourceCatalogReloadDoneLocked();
            });
        }
        snapshot.revision = sourceCatalogRevision_;
        snapshot.configStatus = sourceConfigStatus_;
        snapshot.configStatusMessage = sourceConfigStatusMessage_;

        if (!sourceCatalog_.empty()) {
            snapshot.sources.reserve(sourceCatalog_.size());
            for (auto const& entry : sourceCatalog_) {
                const bool authorized = entry.dataSource
                    ? (!clientHeaders || entry.dataSource->isDataSourceAuthorized(*clientHeaders))
                    : isDescriptorAuthorized(entry.descriptor, clientHeaders);
                if (!authorized) {
                    continue;
                }

                auto copy = entry;
                if (copy.info) {
                    copy.info = cloneDataSourceInfo(*copy.info);
                }
                snapshot.sources.push_back(std::move(copy));
            }
            return snapshot;
        }

        snapshot.sources.reserve(dataSourceInfo_.size());
        auto configIndex = uint32_t{0};
        for (auto const& [dataSource, info] : dataSourceInfo_) {
            if (clientHeaders && !dataSource->isDataSourceAuthorized(*clientHeaders)) {
                continue;
            }

            auto infoCopy = cloneDataSourceInfo(info);
            snapshot.sources.push_back(DataSourceCatalogEntry{
                .descriptor = DataSourceDescriptor{
                    .sourceId = infoCopy.nodeId_,
                    .configIndex = configIndex++,
                    .type = "",
                    .configuredMapId = infoCopy.mapId_,
                    .addOn = infoCopy.isAddOn_},
                .status = DataSourceCatalogStatus::Ready,
                .statusMessage = "",
                .dataSource = dataSource,
                .info = std::move(infoCopy)});
        }
        return snapshot;
    }

    uint64_t addSourceCatalogCallback(Service::DataSourceCatalogCallback callback)
    {
        if (!callback) {
            return 0;
        }
        std::lock_guard lock(sourceCatalogCallbacksMutex_);
        auto id = nextSourceCatalogCallbackId_++;
        sourceCatalogCallbacks_[id] = std::move(callback);
        return id;
    }

    void removeSourceCatalogCallback(uint64_t id)
    {
        if (!id) {
            return;
        }
        std::lock_guard lock(sourceCatalogCallbacksMutex_);
        sourceCatalogCallbacks_.erase(id);
    }

    uint64_t getSourceCatalogRevision() const
    {
        std::shared_lock lock(dataSourcesMutex_);
        return sourceCatalogRevision_;
    }

    bool isSourceCatalogChangeVisible(
        DataSourceCatalogChange const& change,
        std::optional<AuthHeaders> const& clientHeaders) const
    {
        if (!change.sourceUpdate) {
            return true;
        }
        auto const& source = *change.sourceUpdate;
        if (source.dataSource) {
            return !clientHeaders || source.dataSource->isDataSourceAuthorized(*clientHeaders);
        }
        return isDescriptorAuthorized(source.descriptor, clientHeaders);
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
                TileFeatureLayer::CloneCache clonedModelNodes;
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

Service::DataSourceCatalogSubscription::DataSourceCatalogSubscription(Service* service, uint64_t id)
    : service_(service),
      id_(id)
{
}

Service::DataSourceCatalogSubscription::~DataSourceCatalogSubscription()
{
    if (service_ && id_) {
        service_->impl_->removeSourceCatalogCallback(id_);
    }
}

Service::DataSourceCatalogSubscription::DataSourceCatalogSubscription(DataSourceCatalogSubscription&& other) noexcept
    : service_(std::exchange(other.service_, nullptr)),
      id_(std::exchange(other.id_, 0))
{
}

Service::DataSourceCatalogSubscription& Service::DataSourceCatalogSubscription::operator=(
    DataSourceCatalogSubscription&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    if (service_ && id_) {
        service_->impl_->removeSourceCatalogCallback(id_);
    }
    service_ = std::exchange(other.service_, nullptr);
    id_ = std::exchange(other.id_, 0);
    return *this;
}

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

bool Service::request(FeatureLayerSearchTilesRequest::Ptr const& request, std::optional<AuthHeaders> const& clientHeaders)
{
    if (!request) {
        raise("Attempt to request a null FeatureLayerSearchTilesRequest.");
    }
    if (request->isDone()) {
        request->notifyStatus();
        return true;
    }

    auto context = resolveLayerRequest(request->mapId_, request->layerId_, clientHeaders);
    if (context.status_ != RequestStatus::Success) {
        request->setStatus(context.status_);
        return false;
    }
    if (context.layerType_ != LayerType::Features) {
        request->setStatus(RequestStatus::NoDataSource);
        return false;
    }

    std::optional<DataSourceInfo> sourceInfo;
    {
        std::shared_lock lock(impl_->dataSourcesMutex_);
        for (auto const& [dataSource, info] : impl_->dataSourceInfo_) {
            if (info.isAddOn_ || info.mapId_ != request->mapId_) {
                continue;
            }
            if (info.layers_.find(request->layerId_) == info.layers_.end()) {
                continue;
            }
            if (clientHeaders && !dataSource->isDataSourceAuthorized(*clientHeaders)) {
                continue;
            }
            sourceInfo = info;
            break;
        }
    }
    if (!sourceInfo) {
        request->setStatus(RequestStatus::NoDataSource);
        return false;
    }

    auto stageCount = std::max<uint32_t>(1U, context.stages_);
    auto childRequest = [&]() -> LayerTilesRequest::Ptr {
        std::vector<TileId> priorityTileIds(
            request->priorityTileIds_.begin(),
            request->priorityTileIds_.end());
        if (stageCount > 1U) {
            auto stagedRequest = std::make_shared<LayerTilesRequest>(
                request->mapId_,
                request->layerId_,
                std::vector<std::vector<TileId>>{request->tileIds_},
                priorityTileIds);
            stagedRequest->preferCompleteStagedTiles_ = true;
            return stagedRequest;
        }
        return std::make_shared<LayerTilesRequest>(
            request->mapId_,
            request->layerId_,
            request->tileIds_,
            priorityTileIds);
    }();
    request->childRequests_.push_back(childRequest);

    struct SearchExecutionState : std::enable_shared_from_this<SearchExecutionState>
    {
        Service::Impl* impl = nullptr;
        FeatureLayerSearchTilesRequest::Ptr request;
        DataSourceInfo sourceInfo;
        uint32_t stageCount = 1;
        size_t expectedTiles = 0;

        std::mutex mutex;
        std::map<TileId, std::map<uint32_t, TileFeatureLayer::Ptr>> stagesByTile;
        size_t loadedStages = 0;
        size_t searchedTiles = 0;
        size_t matches = 0;
        size_t chunksEmitted = 0;
        size_t pendingEvalJobs = 0;
        bool childDone = false;
        bool terminal = false;

        [[nodiscard]] nlohmann::json progress(std::string state) const
        {
            auto status = makeSearchStatusJson(*request, std::move(state));
            status["tilesQueued"] = expectedTiles;
            status["tilesLoaded"] = loadedStages;
            status["tilesSearched"] = searchedTiles;
            status["matches"] = matches;
            status["chunksEmitted"] = chunksEmitted;
            return status;
        }

        void emitProgress(std::string state)
        {
            request->notifyProgress(progress(std::move(state)));
        }

        void releaseChildRequests()
        {
            request->childRequests_.clear();
        }

        void abortChildRequests()
        {
            auto children = request->childRequests_;
            for (auto const& child : children) {
                if (child && !child->isDone()) {
                    impl->abortRequest(child);
                }
            }
            releaseChildRequests();
        }

        void finishIfComplete()
        {
            RequestStatus finalStatus = RequestStatus::Open;
            {
                std::lock_guard lock(mutex);
                if (terminal || !childDone || pendingEvalJobs != 0 || searchedTiles < expectedTiles) {
                    return;
                }
                terminal = true;
                finalStatus = request->isCancelled() ? RequestStatus::Aborted : RequestStatus::Success;
            }
            releaseChildRequests();
            emitProgress(finalStatus == RequestStatus::Success ? "Success" : "Aborted");
            request->setStatus(finalStatus);
        }

        void fail(simfil::Error const& error)
        {
            {
                std::lock_guard lock(mutex);
                if (terminal) {
                    return;
                }
                terminal = true;
            }
            log().error(
                "Search {} failed for {}::{}: {}",
                request->search_.searchId_,
                request->mapId_,
                request->layerId_,
                error.message);
            auto status = makeSearchStatusJson(*request, "Failed");
            status["error"] = error.message;
            request->notifyProgress(status);
            abortChildRequests();
            request->setStatus(RequestStatus::Aborted);
        }

        void collect(TileFeatureLayer::Ptr layer)
        {
            if (!layer || request->isCancelled()) {
                return;
            }

            std::vector<TileFeatureLayer::Ptr> readyStages;
            {
                std::lock_guard lock(mutex);
                if (terminal) {
                    return;
                }
                auto stage = stageCount > 1U ? layer->stage().value_or(0U) : 0U;
                auto& tileStages = stagesByTile[layer->tileId()];
                if (tileStages.emplace(stage, layer).second) {
                    ++loadedStages;
                }
                if (tileStages.size() != stageCount) {
                    return;
                }
                readyStages.reserve(tileStages.size());
                for (auto const& [_, stageLayer] : tileStages) {
                    readyStages.push_back(stageLayer);
                }
                // The eval job now owns the ready stage pointers. Keeping them
                // here would retain every searched source tile until request end.
                stagesByTile.erase(layer->tileId());
                ++pendingEvalJobs;
            }

            emitProgress("TileLoaded");
            auto self = shared_from_this();
            impl->enqueueSearchEvalJob(
                sourceInfo,
                request,
                [self, readyStages = std::move(readyStages)]() mutable {
                    self->evaluate(std::move(readyStages));
                },
                [self]() {
                    self->markEvalDone(0, false);
                });
        }

        void evaluate(std::vector<TileFeatureLayer::Ptr> readyStages)
        {
            try {
                if (request->isCancelled()) {
                    markEvalDone(0, false);
                    return;
                }

                auto searchRequest = request->search_;
                searchRequest.sourceStageMask_.clear();
                searchRequest.sourceStageMask_.reserve(readyStages.size());
                for (auto const& stageLayer : readyStages) {
                    if (stageLayer) {
                        searchRequest.sourceStageMask_.push_back(
                            stageCount > 1U ? stageLayer->stage().value_or(0U) : UnspecifiedStage);
                    }
                }

                TileFeatureLayer::Ptr searchSource;
                if (readyStages.size() == 1) {
                    searchSource = readyStages.front();
                } else {
                    auto assembled = assembleFeatureLayerStages(readyStages);
                    if (!assembled) {
                        fail(assembled.error());
                        markEvalDone(0, false);
                        return;
                    }
                    searchSource = *assembled;
                }

                auto searchResult = searchFeatureLayerAsResultLayer(*searchSource, searchRequest);
                if (!searchResult) {
                    fail(searchResult.error());
                    markEvalDone(0, false);
                    return;
                }

                auto resultCount = searchResult->layer_ ? searchResult->layer_->size() : 0;
                if (searchResult->layer_) {
                    request->notifyResult(std::move(searchResult->layer_));
                }
                markEvalDone(resultCount, true);
            } catch (std::exception const& exception) {
                auto const sourceTileId = readyStages.empty() || !readyStages.front()
                    ? TileId{}
                    : readyStages.front()->tileId();
                fail(simfil::Error{
                    simfil::Error::InternalError,
                    fmt::format(
                        "Search evaluation failed for {}::{} tile {:x}: {}",
                        request->mapId_,
                        request->layerId_,
                        sourceTileId.value(),
                        exception.what())});
                markEvalDone(0, false);
            }
        }

        void markEvalDone(size_t resultCount, bool emittedChunk)
        {
            {
                std::lock_guard lock(mutex);
                if (pendingEvalJobs > 0) {
                    --pendingEvalJobs;
                }
                if (terminal) {
                    return;
                }
                ++searchedTiles;
                matches += resultCount;
                if (emittedChunk) {
                    ++chunksEmitted;
                }
            }
            emitProgress("TileSearched");
            finishIfComplete();
        }

        void childFinished(RequestStatus status)
        {
            if (status != RequestStatus::Success) {
                {
                    std::lock_guard lock(mutex);
                    terminal = true;
                    stagesByTile.clear();
                }
                releaseChildRequests();
                request->setStatus(status);
                return;
            }
            {
                std::lock_guard lock(mutex);
                childDone = true;
            }
            finishIfComplete();
        }
    };

    auto state = std::make_shared<SearchExecutionState>();
    state->impl = impl_.get();
    state->request = request;
    state->sourceInfo = *sourceInfo;
    state->stageCount = stageCount > 1U ? stageCount : 1U;
    state->expectedTiles = request->tileIds_.size();

    childRequest->onFeatureLayer([state](TileFeatureLayer::Ptr layer) {
        state->collect(std::move(layer));
    });
    childRequest->onDone_ = [state](RequestStatus status) {
        state->childFinished(status);
    };

    auto status = makeSearchStatusJson(*request, "Open");
    status["tilesQueued"] = request->tileIds_.size();
    status["tilesLoaded"] = 0;
    status["tilesSearched"] = 0;
    status["matches"] = 0;
    status["chunksEmitted"] = 0;
    request->notifyProgress(status);

    return this->request(std::vector<LayerTilesRequest::Ptr>{childRequest}, clientHeaders);
}

bool Service::request(
    std::vector<FeatureLayerSearchTilesRequest::Ptr> const& requests,
    std::optional<AuthHeaders> const& clientHeaders)
{
    bool allAccepted = true;
    for (auto const& request : requests) {
        allAccepted = this->request(request, clientHeaders) && allAccepted;
    }
    return allAccepted;
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

void Service::abort(const FeatureLayerSearchTilesRequest::Ptr& r)
{
    if (!r || r->isDone()) {
        return;
    }
    r->cancel();
    impl_->abortSearchEvalJobs(r);
    auto childRequests = r->childRequests_;
    for (auto const& child : childRequests) {
        if (child && !child->isDone()) {
            impl_->abortRequest(child);
        }
    }
    r->childRequests_.clear();
}

std::vector<DataSourceInfo> Service::info(std::optional<AuthHeaders> const& clientHeaders)
{
    return impl_->getDataSourceInfos(clientHeaders);
}

DataSourceCatalogSnapshot Service::sourceCatalog(
    std::optional<AuthHeaders> const& clientHeaders,
    bool waitUntilReloadDone) const
{
    return impl_->getSourceCatalog(clientHeaders, waitUntilReloadDone);
}

uint64_t Service::sourceCatalogRevision() const
{
    return impl_->getSourceCatalogRevision();
}

bool Service::isSourceCatalogChangeVisible(
    DataSourceCatalogChange const& change,
    std::optional<AuthHeaders> const& clientHeaders) const
{
    return impl_->isSourceCatalogChangeVisible(change, clientHeaders);
}

Service::DataSourceCatalogSubscription Service::subscribeToSourceCatalogChanges(DataSourceCatalogCallback callback)
{
    return DataSourceCatalogSubscription(this, impl_->addSourceCatalogCallback(std::move(callback)));
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
        result.noDataSourceReason_ = NoDataSourceReason::None;
    }
    else {
        result.status_ = RequestStatus::NoDataSource;
        result.noDataSourceReason_ = NoDataSourceReason::MissingMapOrLayer;

        if (impl_->dataSourceInfo_.empty()) {
            auto const configPath = DataSourceConfigService::get().getConfigFilePath();
            auto const configStats = DataSourceConfigService::get().getDataSourceConfigStats();
            if (!configPath.has_value()) {
                result.noDataSourceReason_ = NoDataSourceReason::NoConfig;
            }
            else if (configStats.configured == 0) {
                result.noDataSourceReason_ = NoDataSourceReason::EmptySources;
            }
            else if (configStats.enabled == 0) {
                result.noDataSourceReason_ = NoDataSourceReason::AllSourcesDisabled;
            }
            else if (impl_->dataSourceConstructionFailed_ > 0) {
                result.noDataSourceReason_ = NoDataSourceReason::DatasourceInitializationFailed;
            }
        }
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
    size_t constructionFailures = 0;
    {
        std::unique_lock lock(impl_->jobsMutex_);
        activeRequests = impl_->requests_.size();
    }
    {
        std::shared_lock lock(impl_->dataSourcesMutex_);
        constructionFailures = impl_->dataSourceConstructionFailed_;
    }
    auto configStats = DataSourceConfigService::get().getDataSourceConfigStats();
    auto result = nlohmann::json{
        {"datasources", datasources},
        {"active-requests", activeRequests},
        {"datasource-config", nlohmann::json{
            {"configured", configStats.configured},
            {"enabled", configStats.enabled},
            {"disabled", configStats.disabled},
            {"construction-failed", constructionFailures}
        }}
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
