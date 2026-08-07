#include "service.h"

#include "fmt/format.h"
#include "locate.h"
#include "config.h"
#include "service-memory.h"
#include "mapget/log.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/featureid.h"
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
#include <unordered_set>

#include "simfil/types.h"

namespace mapget
{

namespace {

using detail::FilterMemoryTracker;
using detail::dataSourceDescriptorMemoryUsage;
using detail::dataSourceInfoContainerMemoryUsage;

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

TileFeatureLayer::Ptr restrictFeatureLayerForResponse(
    TileFeatureLayer::Ptr const& source,
    std::span<std::string const> featureIds)
{
    auto result = std::make_shared<TileFeatureLayer>(
        source->tileId(),
        source->stringPoolId(),
        source->mapId(),
        source->layerInfo(),
        source->strings());
    result->setInfo(source->info());
    if (auto legalInfo = source->legalInfo()) {
        result->setLegalInfo(*legalInfo);
    }
    result->setGlbAttachmentName(
        source->glbAttachmentName());

    TileFeatureLayer::CloneCache cloneCache;
    for (auto const& canonicalId : featureIds) {
        auto feature = source->find(canonicalId);
        if (!feature) {
            continue;
        }
        auto featureId = feature->id();
        result->clone(
            cloneCache,
            source,
            *feature,
            featureId->typeId(),
            featureId->keyValuePairs());
    }
    return result;
}

tl::expected<TileId, simfil::Error> pointGroupOwnerTile(
    FeatureLayerPointGroupMember const& member,
    FeatureLayerFilterRequest const& request,
    int level)
{
    if (member.channelIndex_ >= request.channels_.size()) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InternalError,
            "Point-group member references a missing filter channel.",
        });
    }
    auto const& group =
        request.channels_[member.channelIndex_].group_;
    if (!group) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InternalError,
            "Point-group member references a channel without grouping.",
        });
    }

    auto const longitude =
        group->origin_.x +
        (static_cast<double>(member.key_.x_) + 0.5) *
            group->cellSize_.x;
    auto const latitude =
        group->origin_.y +
        (static_cast<double>(member.key_.y_) + 0.5) *
            group->cellSize_.y;
    if (!std::isfinite(longitude) || !std::isfinite(latitude)) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InvalidArguments,
            "Point-group cell center is outside the finite WGS84 domain.",
        });
    }

    auto wrappedLongitude = std::fmod(longitude + 180.0, 360.0);
    if (wrappedLongitude < 0.0) {
        wrappedLongitude += 360.0;
    }
    wrappedLongitude -= 180.0;
    auto const boundedLatitude = std::clamp(
        latitude,
        -90.0,
        std::nextafter(
            90.0,
            -std::numeric_limits<double>::infinity()));
    return TileId::fromWgs84(
        wrappedLongitude,
        boundedLatitude,
        level);
}

void mergeFilterTraces(
    std::map<std::string, simfil::Trace>& target,
    std::map<std::string, simfil::Trace> source)
{
    for (auto&& [name, trace] : source) {
        target[name].append(std::move(trace));
    }
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

/** Build the common generation-aware filter status payload. */
nlohmann::json makeFilterStatusJson(
    FeatureLayerFilterTilesRequest const& request,
    std::string state)
{
    auto status = nlohmann::json::object({
        {"type", "mapget.filter.status"},
        {"mapId", request.mapId_},
        {"layerId", request.layerId_},
        {"state", std::move(state)},
        {"filterId", request.filter_.filterId_},
        {"generation", request.filter_.generation_},
    });
    if (request.sourceId_) {
        status["sourceId"] = *request.sourceId_;
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
                info.stringPoolId_,
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
    : mapId_(std::move(mapId)),
      layerId_(std::move(layerId)),
      tileIds_(std::move(tiles)),
      priorityTileIds_({priorityTileIds.begin(), priorityTileIds.end()})
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
    for (auto const& priorityTileId :
         priorityTileIds_)
    {
        if (!seenTileIds.contains(
                priorityTileId))
        {
            raise(
                "Priority tile IDs must be contained in the request tile IDs.");
        }
    }
    if (tileIds_.empty()) {
        // An empty request is always set to success, but the client/service
        // is responsible for triggering notifyStatus() in that case.
        status_ = RequestStatus::Success;
    }
}

void LayerTilesRequest::prepareResolvedLayer(LayerType layerType)
{
    nextTileIndex_ = 0;
    resultCount_ = 0;
    resolvedTileKeys_.clear();
    tileKeysNotStarted_.clear();

    const auto isPriorityTile = [this](TileId const& tileId) {
        return priorityTileIds_.find(tileId) != priorityTileIds_.end();
    };
    const auto appendKey = [this](MapTileKey key) {
        if (tileKeysNotStarted_.insert(key).second) {
            resolvedTileKeys_.push_back(std::move(key));
        }
    };

    const auto appendTiles = [&](std::optional<bool> priorityFilter) {
        for (auto const& tileId : tileIds_) {
            if (priorityFilter && isPriorityTile(tileId) != *priorityFilter) {
                continue;
            }
            appendKey(MapTileKey(layerType, mapId_, layerId_, tileId));
        }
    };
    if (priorityTileIds_.empty()) {
        appendTiles(std::nullopt);
    } else {
        appendTiles(true);
        appendTiles(false);
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
        if (onFeatureLayer_) {
            auto featureLayer =
                std::static_pointer_cast<TileFeatureLayer>(r);
            if (auto restriction =
                    featureIdsByTile_.find(
                        featureLayer->tileId());
                restriction !=
                    featureIdsByTile_.end())
            {
                featureLayer =
                    restrictFeatureLayerForResponse(
                        featureLayer,
                        restriction->second);
            }
            onFeatureLayer_(std::move(featureLayer));
        }
        break;
    case LayerType::SourceData:
        if (onSourceDataLayer_)
            onSourceDataLayer_(std::move(std::static_pointer_cast<TileSourceDataLayer>(r)));
        break;
    default:
        log().error(fmt::format("Unhandled layer type {}, no matching callback!", static_cast<int>(type)));
        break;
    }

    const auto resultCount =
        resultCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (resultCount == resolvedTileKeys_.size()) {
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
    if (sourceId_) {
        requestJson["sourceId"] = *sourceId_;
    }

    auto tileIds = nlohmann::json::array();
    for (auto const& tileId : tileIds_) {
        tileIds.emplace_back(tileId.value());
    }
    requestJson["tileIds"] = std::move(tileIds);
    if (!priorityTileIds_.empty()) {
        auto priorityTileIds = nlohmann::json::array();
        for (auto const& tileId : priorityTileIds_) {
            priorityTileIds.emplace_back(tileId.value());
        }
        requestJson["priorityTileIds"] = std::move(priorityTileIds);
    }
    if (!featureIdsByTile_.empty()) {
        auto featureIds = nlohmann::json::array();
        for (auto const& [tileId, ids] :
             featureIdsByTile_)
        {
            featureIds.push_back({
                {"tileId", tileId.value()},
                {"ids", ids},
            });
        }
        requestJson["featureIds"] =
            std::move(featureIds);
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

FeatureLayerFilterTilesRequest::FeatureLayerFilterTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<TileId> tiles,
    FeatureLayerFilterRequest filter)
    : FeatureLayerFilterTilesRequest(
          std::move(mapId),
          std::move(layerId),
          std::move(tiles),
          std::move(filter),
          {})
{
}

FeatureLayerFilterTilesRequest::FeatureLayerFilterTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<TileId> tiles,
    FeatureLayerFilterRequest filter,
    std::vector<TileId> const& priorityTileIds)
    : mapId_(std::move(mapId)),
      layerId_(std::move(layerId)),
      tileIds_(std::move(tiles)),
      priorityTileIds_({priorityTileIds.begin(), priorityTileIds.end()}),
      filter_(std::move(filter))
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
    for (auto const& priorityTileId :
         priorityTileIds_)
    {
        if (!seenTileIds.contains(
                priorityTileId))
        {
            raise(
                "Priority tile IDs must be contained in the request tile IDs.");
        }
    }
    if (tileIds_.empty()) {
        status_ = RequestStatus::Success;
    }
}

RequestStatus FeatureLayerFilterTilesRequest::getStatus()
{
    return status_;
}

bool FeatureLayerFilterTilesRequest::isDone()
{
    return status_ != RequestStatus::Open;
}

bool FeatureLayerFilterTilesRequest::isCancelled() const
{
    return cancelled_;
}

void FeatureLayerFilterTilesRequest::wait()
{
    std::unique_lock doneLock(statusMutex_);
    if (!isDone()) {
        statusConditionVariable_.wait(doneLock, [this] { return isDone(); });
    }
}

void FeatureLayerFilterTilesRequest::notifyResult(TileSubsetLayer::Ptr result)
{
    if (cancelled_ || isDone()) {
        return;
    }
    if (onFilterResult_) {
        onFilterResult_(std::move(result));
    }
}

void FeatureLayerFilterTilesRequest::notifyProgress(nlohmann::json const& status)
{
    if (cancelled_) {
        return;
    }
    if (onStatus_) {
        onStatus_(status);
    }
}

void FeatureLayerFilterTilesRequest::setStatus(RequestStatus s)
{
    auto const previous = status_.exchange(s);
    if (previous != RequestStatus::Open) {
        return;
    }
    notifyStatus();
}

void FeatureLayerFilterTilesRequest::notifyStatus()
{
    if (isDone() && onDone_) {
        onDone_(status_);
    }
    statusConditionVariable_.notify_all();
}

void FeatureLayerFilterTilesRequest::cancel()
{
    cancelled_ = true;
    setStatus(RequestStatus::Aborted);
}

struct Service::Controller
{
    virtual ~Controller()
    {
        stopFilterEvalWorkers();
    }

    struct Job {
        MapTileKey tileKey;
        std::vector<LayerTilesRequest::Ptr> waitingRequests;
        std::optional<std::chrono::system_clock::time_point> cacheExpiredAt;
        TileLayer::LoadState loadStatus = TileLayer::LoadState::LoadingQueued;
        uint64_t mapEpoch = 0;
    };

    struct FilterEvalWork {
        std::string mapId;
        std::weak_ptr<FeatureLayerFilterTilesRequest> owner;
        std::function<void()> work;
        std::function<void()> discard;
    };

    std::map<MapTileKey, std::shared_ptr<Job>> jobsInProgress_;    // Jobs currently in progress + interested requests
    Cache::Ptr cache_;                       // The cache for the service
    std::optional<std::chrono::milliseconds> defaultTtl_; // Default TTL applied when datasource does not override
    std::list<LayerTilesRequest::Ptr> requests_;       // List of requests currently being processed
    std::list<FilterEvalWork> filterEvalJobs_; // Derived filter jobs scheduled after source tiles are loaded.
    std::vector<std::weak_ptr<FilterMemoryTracker>> filterMemoryTrackers_; // Active filter ownership gauges.
    std::map<std::string, uint64_t> mapEpochs_; // Invalidates late work after source-composition changes.
    std::condition_variable jobsAvailable_;  // Condition variable to signal job availability
    std::condition_variable filterJobsAvailable_; // Signals CPU-only derived evaluation work.
    std::mutex jobsMutex_;  // Mutex used with the jobsAvailable_ condition variable
    std::vector<std::thread> filterEvalWorkers_;
    size_t runningFilterEvalJobs_ = 0;
    bool filterEvalStopping_ = false;

    explicit Controller(Cache::Ptr cache, std::optional<std::chrono::milliseconds> defaultTtl)
        : cache_(std::move(cache)),
          defaultTtl_(defaultTtl)
    {
        if (!cache_)
            raise("Cache must not be null!");
    }

    [[nodiscard]] static size_t filterEvalWorkerCount()
    {
        auto const available =
            std::max(1u, std::thread::hardware_concurrency());
        return std::min<size_t>(32, available);
    }

    /**
     * Leave CPU capacity for datasource conversion while source work remains,
     * then use the complete filter pool for cache-hit/filter-only bursts.
     *
     * Must be called with jobsMutex_ held.
     */
    [[nodiscard]] size_t filterEvalConcurrentLimitLocked() const
    {
        auto const fullLimit = filterEvalWorkerCount();
        auto const sourceWorkPending =
            !jobsInProgress_.empty() ||
            std::ranges::any_of(
                requests_,
                [](auto const& request) {
                    return request &&
                        !request->isDone() &&
                        !request->tileKeysNotStarted_.empty();
                });
        if (!sourceWorkPending) {
            return fullLimit;
        }
        return std::max<size_t>(
            1,
            fullLimit * 3 / 4);
    }

    /** Start the request-wide CPU pool lazily on the first filter result. */
    void startFilterEvalWorkersLocked()
    {
        if (!filterEvalWorkers_.empty() ||
            filterEvalStopping_)
        {
            return;
        }
        auto const workerCount = filterEvalWorkerCount();
        filterEvalWorkers_.reserve(workerCount);
        for (size_t index = 0;
             index < workerCount;
             ++index)
        {
            filterEvalWorkers_.emplace_back(
                [this] {
                    filterEvalWorkerLoop();
                });
        }
    }

    /** Consume source-independent subset evaluation work in FIFO order. */
    void filterEvalWorkerLoop()
    {
        while (true) {
            FilterEvalWork job;
            FeatureLayerFilterTilesRequest::Ptr owner;
            bool execute = false;
            {
                std::unique_lock lock(jobsMutex_);
                filterJobsAvailable_.wait(
                    lock,
                    [this] {
                        return filterEvalStopping_ ||
                            (!filterEvalJobs_.empty() &&
                             runningFilterEvalJobs_ <
                                 filterEvalConcurrentLimitLocked());
                    });
                if (filterEvalStopping_) {
                    return;
                }

                job = std::move(filterEvalJobs_.front());
                filterEvalJobs_.pop_front();
                owner = job.owner.lock();
                execute =
                    owner &&
                    !owner->isDone() &&
                    static_cast<bool>(job.work);
                if (execute) {
                    ++runningFilterEvalJobs_;
                }
            }

            if (!execute) {
                if (job.discard) {
                    job.discard();
                }
                continue;
            }

            try {
                job.work();
            }
            catch (std::exception const& error) {
                log().error(
                    "Unhandled filter evaluation failure: {}",
                    error.what());
                owner->setStatus(RequestStatus::Aborted);
                if (job.discard) {
                    job.discard();
                }
            }
            catch (...) {
                log().error(
                    "Unhandled non-standard filter evaluation failure.");
                owner->setStatus(RequestStatus::Aborted);
                if (job.discard) {
                    job.discard();
                }
            }

            {
                std::unique_lock lock(jobsMutex_);
                --runningFilterEvalJobs_;
            }
            // Source work may have drained while this evaluation ran. Wake
            // parked workers so the pool can expand to its filter-only limit.
            filterJobsAvailable_.notify_all();
        }
    }

    /**
     * Drain queued work and join derived-evaluation threads before datasource
     * and request state owned by Impl is torn down.
     */
    void stopFilterEvalWorkers()
    {
        std::vector<std::function<void()>> discarded;
        {
            std::unique_lock lock(jobsMutex_);
            if (filterEvalStopping_) {
                return;
            }
            filterEvalStopping_ = true;
            for (auto& job : filterEvalJobs_) {
                if (job.discard) {
                    discarded.push_back(
                        std::move(job.discard));
                }
            }
            filterEvalJobs_.clear();
        }
        filterJobsAvailable_.notify_all();
        for (auto& worker : filterEvalWorkers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        filterEvalWorkers_.clear();
        for (auto& discard : discarded) {
            discard();
        }
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

        auto startedJob = std::make_shared<Job>();
        startedJob->tileKey = candidate.tileKey_;
        startedJob->waitingRequests = {request};
        startedJob->cacheExpiredAt =
            cachedResult.expiredAt;
        startedJob->mapEpoch =
            mapEpochs_[candidate.tileKey_.mapId_];
        attachMatchingRequests(request, candidate.tileKey_, startedJob->waitingRequests);
        jobsInProgress_.emplace(startedJob->tileKey, startedJob);
        log().debug("Working on tile: {}", startedJob->tileKey.toString());

        return startedJob;
    }

    void enqueueFilterEvalJob(
        std::string mapId,
        FeatureLayerFilterTilesRequest::Ptr const& owner,
        std::function<void()> work,
        std::function<void()> discard)
    {
        if (!work) {
            return;
        }
        bool discardImmediately = false;
        {
            std::unique_lock lock(jobsMutex_);
            if (filterEvalStopping_) {
                discardImmediately = true;
            }
            else {
                startFilterEvalWorkersLocked();
                filterEvalJobs_.push_back(FilterEvalWork{
                    std::move(mapId),
                    owner,
                    std::move(work),
                    std::move(discard)});
            }
        }
        if (discardImmediately) {
            if (discard) {
                discard();
            }
            return;
        }
        filterJobsAvailable_.notify_one();
    }

    void abortFilterEvalJobs(FeatureLayerFilterTilesRequest::Ptr const& request)
    {
        std::vector<std::function<void()>> discarded;
        {
            std::unique_lock lock(jobsMutex_);
            for (auto it = filterEvalJobs_.begin(); it != filterEvalJobs_.end();) {
                if (it->owner.lock() != request) {
                    ++it;
                    continue;
                }
                if (it->discard) {
                    discarded.push_back(std::move(it->discard));
                }
                it = filterEvalJobs_.erase(it);
            }
        }
        for (auto& discard : discarded) {
            discard();
        }
        filterJobsAvailable_.notify_all();
    }

    /**
     * Abort queued/waiting work and invalidate cached content for a map whose
     * primary or add-on composition changed.
     *
     * A monotonically increasing epoch also prevents a backend call which was
     * already running from publishing after this method returns.
     */
    void invalidateMap(std::string const& mapId)
    {
        std::vector<LayerTilesRequest::Ptr> abortedRequests;
        std::vector<std::function<void()>> discarded;
        {
            std::unique_lock lock(jobsMutex_);
            ++mapEpochs_[mapId];

            for (auto it = requests_.begin();
                 it != requests_.end();)
            {
                if (*it && (*it)->mapId_ == mapId) {
                    abortedRequests.push_back(*it);
                    it = requests_.erase(it);
                }
                else {
                    ++it;
                }
            }

            for (auto it = jobsInProgress_.begin();
                 it != jobsInProgress_.end();)
            {
                if (it->first.mapId_ != mapId) {
                    ++it;
                    continue;
                }
                abortedRequests.insert(
                    abortedRequests.end(),
                    it->second->waitingRequests.begin(),
                    it->second->waitingRequests.end());
                it->second->waitingRequests.clear();
                it = jobsInProgress_.erase(it);
            }

            for (auto it = filterEvalJobs_.begin();
                 it != filterEvalJobs_.end();)
            {
                if (it->mapId != mapId) {
                    ++it;
                    continue;
                }
                if (it->discard) {
                    discarded.push_back(
                        std::move(it->discard));
                }
                it = filterEvalJobs_.erase(it);
            }

            // Workers publish under jobsMutex_ as well. Clearing here means
            // an old worker either published before this point and is erased,
            // or observes the new epoch and cannot publish afterward.
            cache_->invalidateMap(mapId);
        }

        std::ranges::sort(
            abortedRequests,
            {},
            [](auto const& request) {
                return request.get();
            });
        abortedRequests.erase(
            std::unique(
                abortedRequests.begin(),
                abortedRequests.end()),
            abortedRequests.end());
        for (auto const& request : abortedRequests) {
            if (request && !request->isDone()) {
                request->setStatus(
                    RequestStatus::Aborted);
            }
        }
        for (auto& discard : discarded) {
            discard();
        }
        jobsAvailable_.notify_all();
        filterJobsAvailable_.notify_all();
    }

    void removeCompletedRequests()
    {
        requests_.remove_if([](auto const& request) {
            return !request ||
                request->isDone() ||
                request->tileKeysNotStarted_.empty();
        });
    }

    /**
     * Complete every request waiting on a source-tile job which failed.
     *
     * The job must leave the in-flight table before callbacks run, otherwise a
     * retry can join a dead job. Failed requests are also detached from other
     * jobs so later successful loads cannot retain or notify them.
     */
    void failTileJob(Job const& job)
    {
        std::vector<LayerTilesRequest::Ptr> failedRequests;
        {
            std::unique_lock lock(jobsMutex_);
            if (auto inProgress =
                    jobsInProgress_.find(job.tileKey);
                inProgress != jobsInProgress_.end() &&
                inProgress->second.get() == &job)
            {
                jobsInProgress_.erase(inProgress);
            }
            failedRequests = job.waitingRequests;

            for (auto const& failed : failedRequests) {
                requests_.remove_if(
                    [&](auto const& request) {
                        return request == failed;
                    });
            }
            for (auto& [_, inProgress] : jobsInProgress_) {
                std::erase_if(
                    inProgress->waitingRequests,
                    [&](auto const& request) {
                        return std::ranges::find(
                                   failedRequests,
                                   request) !=
                               failedRequests.end();
                    });
            }
        }

        for (auto const& request : failedRequests) {
            if (request && !request->isDone()) {
                request->setStatus(
                    RequestStatus::Aborted);
            }
        }
        jobsAvailable_.notify_all();
    }

    std::shared_ptr<Job> nextJob(DataSourceInfo const& i, std::unique_lock<std::mutex>& lock)
    {
        // Workers call the nextJob function when they are free.
        // Note: For thread safety, jobsMutex_ must be held
        //  when calling this function. The lock may be released/re-acquired
        //  between sweeps to allow external updates.

        while (true) {
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

            std::vector<LayerTilesRequest::Ptr> notifyRequests;
            {
                std::unique_lock<std::mutex> lock(controller_.jobsMutex_);
                auto const currentEpoch =
                    controller_.mapEpochs_[
                        job.tileKey.mapId_];
                if (job.mapEpoch == currentEpoch) {
                    controller_.cache_->putTileLayer(
                        layer);
                    notifyRequests =
                        job.waitingRequests;
                }
                if (auto inProgress =
                        controller_.jobsInProgress_.find(
                            job.tileKey);
                    inProgress !=
                        controller_.jobsInProgress_.end() &&
                    inProgress->second == nextJob)
                {
                    controller_.jobsInProgress_.erase(
                        inProgress);
                }
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
            controller_.failTileJob(job);
        }

        return true;
    }
};

struct Service::Impl : public Service::Controller
{
    std::map<DataSource::Ptr, DataSourceInfo> dataSourceInfo_;
    std::map<DataSource::Ptr, simfil::MemoryUsage> dataSourceInfoMemory_;
    std::map<DataSource::Ptr, simfil::MemoryUsage> workerInfoMemory_;
    std::map<DataSource::Ptr, std::string> dataSourceSourceIds_;
    std::map<DataSource::Ptr, std::vector<Worker::Ptr>> dataSourceWorkers_;
    std::list<DataSource::Ptr> addOnDataSources_;

    mutable std::shared_mutex dataSourcesMutex_;
    std::unique_ptr<DataSourceConfigService::Subscription> configSubscription_;
    std::vector<DataSource::Ptr> dataSourcesFromConfig_;
    size_t dataSourceConstructionFailed_ = 0;
    std::vector<DataSourceCatalogEntry> sourceCatalog_;
    std::map<uint32_t, simfil::MemoryUsage> sourceCatalogInfoMemory_;
    uint64_t sourceCatalogGeneration_ = 0;
    uint64_t sourceCatalogRevision_ = 0;
    uint64_t nextRuntimeSourceId_ = 0;
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
        // Derived filter closures capture Impl-owned request state and may
        // enqueue further source work. Let them finish while every datasource
        // worker and catalog object they can reach is still alive.
        stopFilterEvalWorkers();

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
                    workersToJoin.push_back(worker);
                }
            }
            dataSourceWorkers_.clear();
            dataSourceInfo_.clear();
            dataSourceInfoMemory_.clear();
            workerInfoMemory_.clear();
            dataSourceSourceIds_.clear();
            addOnDataSources_.clear();
            dataSourcesFromConfig_.clear();
            dataSourceConstructionFailed_ = 0;
        }
        // Protect the termination predicate with the same mutex used by the
        // condition-variable wait. An atomic flag alone still permits a
        // notify-before-wait race during teardown.
        {
            std::unique_lock lock(jobsMutex_);
            for (auto& worker : workersToJoin) {
                worker->shouldTerminate_ = true;
            }
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

    void updateCatalogStatusMessage(uint64_t generation, uint32_t configIndex, std::string message)
    {
        DataSourceCatalogChange change;
        bool changed = false;
        {
            std::unique_lock lock(dataSourcesMutex_);
            if (generation != sourceCatalogGeneration_) {
                return;
            }
            auto it = std::ranges::find_if(sourceCatalog_, [&](auto const& entry) {
                return entry.descriptor.configIndex == configIndex;
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

    void updateCatalogProgress(uint64_t generation, uint32_t configIndex, std::optional<float> progress)
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
                return entry.descriptor.configIndex == configIndex;
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
        uint32_t configIndex,
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
                return entry.descriptor.configIndex == configIndex;
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
        uint32_t configIndex,
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
                return entry.descriptor.configIndex == configIndex;
            });
            if (it == sourceCatalog_.end()) {
                return;
            }
            it->status = DataSourceCatalogStatus::Ready;
            it->statusMessage.clear();
            it->progress.reset();
            it->dataSource = std::move(dataSource);
            it->info = cloneDataSourceInfo(info);
            auto catalogInfoMemory = it->info->memoryUsage().total();
            // DataSourceInfo itself lives inline in the catalog entry's
            // optional; the entry vector already accounts that object storage.
            catalogInfoMemory.logicalBytes -=
                std::min(catalogInfoMemory.logicalBytes, sizeof(DataSourceInfo));
            catalogInfoMemory.allocatedBytes -=
                std::min(catalogInfoMemory.allocatedBytes, sizeof(DataSourceInfo));
            sourceCatalogInfoMemory_[configIndex] = catalogInfoMemory;
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
        auto configIndex = descriptor.configIndex;
        auto sourceId = descriptor.sourceId;
        dataSourceConstructionThreads_.push_back(ConstructionThread{
            .thread = std::thread(
                [this,
                 generation,
                 configNode = std::move(configNode),
                 configIndex,
                 sourceId = std::move(sourceId),
                 done,
                 stopRequested]() mutable
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
                            .setStatusMessage = [this, generation, configIndex, &lastStatusMessage](std::string message) {
                                lastStatusMessage = message;
                                updateCatalogStatusMessage(generation, configIndex, std::move(message));
                            },
                            .setProgress = [this, generation, configIndex](std::optional<float> progress) {
                                updateCatalogProgress(generation, configIndex, progress);
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
                                markCatalogConstructionFailed(generation, configIndex, std::move(lastStatusMessage));
                            }
                            finish();
                            return;
                        }

                        if (!isCurrentCatalogGeneration(generation)) {
                            finish();
                            return;
                        }

                        auto info = addDataSource(
                            dataSource,
                            false,
                            sourceId);
                        if (!isCurrentCatalogGeneration(generation)) {
                            removeDataSource(dataSource, false);
                            finish();
                            return;
                        }
                        markCatalogConstructionReady(generation, configIndex, std::move(dataSource), std::move(info));
                    }
                    catch (std::exception const& e) {
                        if (isCurrentCatalogGeneration(generation)) {
                            markCatalogConstructionFailed(
                                generation,
                                configIndex,
                                fmt::format("Exception while making datasource at index {}: {}", configIndex, e.what()));
                        }
                    }
                    catch (...) {
                        if (isCurrentCatalogGeneration(generation)) {
                            markCatalogConstructionFailed(
                                generation,
                                configIndex,
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
            sourceCatalogInfoMemory_.clear();
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

    DataSourceInfo addDataSource(
        DataSource::Ptr const& dataSource,
        bool publishCatalogChange = true,
        std::optional<std::string> sourceId = {})
    {
        if (!dataSource) {
            raise("Tried to add a null data source.");
        }

        auto info = cloneDataSourceInfo(dataSource->info());
        auto const infoMemory = info.memoryUsage().total();
        auto const workerInfoMemory = dataSourceInfoContainerMemoryUsage(info);
        std::unique_lock lock(dataSourcesMutex_);

        if (info.stringPoolId_.empty()) {
            // Unique node IDs are required for the string pool offsets.
            raise("Tried to create service worker for an unnamed node!");
        }
        for (auto& existingSource : dataSourceInfo_) {
            if (existingSource.second.stringPoolId_ == info.stringPoolId_) {
                // Unique node IDs are required for the string pool offsets.
                raise(
                    fmt::format("Data source with node ID '{}' already registered!",
                                info.stringPoolId_));
            }
            if (!info.isAddOn_ &&
                !existingSource.second.isAddOn_ &&
                existingSource.second.mapId_ == info.mapId_)
            {
                raise(fmt::format(
                    "Primary data source for map '{}' is already registered.",
                    info.mapId_));
            }
        }

        auto effectiveSourceId = sourceId
            ? std::move(*sourceId)
            : fmt::format(
                  "runtime-source-{}",
                  nextRuntimeSourceId_++);
        if (effectiveSourceId.empty()) {
            raise("Data source catalog sourceId must not be empty.");
        }
        if (std::ranges::any_of(
                dataSourceSourceIds_,
                [&](auto const& entry) {
                    return entry.second == effectiveSourceId;
                }))
        {
            raise(fmt::format(
                "Data source catalog sourceId '{}' is already registered.",
                effectiveSourceId));
        }

        dataSourceInfo_[dataSource] = info;
        dataSourceInfoMemory_[dataSource] = infoMemory;
        dataSourceSourceIds_[dataSource] =
            std::move(effectiveSourceId);

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
        workerInfoMemory_[dataSource] = {
            workerInfoMemory.logicalBytes * workers.size(),
            workerInfoMemory.allocatedBytes * workers.size(),
        };

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
        std::optional<std::string> affectedMapId;
        {
            std::unique_lock lock(dataSourcesMutex_);
            if (auto info = dataSourceInfo_.find(dataSource);
                info != dataSourceInfo_.end())
            {
                affectedMapId =
                    info->second.mapId_;
            }
            dataSourceInfo_.erase(dataSource);
            dataSourceInfoMemory_.erase(dataSource);
            workerInfoMemory_.erase(dataSource);
            dataSourceSourceIds_.erase(dataSource);
            addOnDataSources_.remove(dataSource);
            if (publishCatalogChange) {
                change = markSourceCatalogChangedLocked("removed");
            }

            auto workers = dataSourceWorkers_.find(dataSource);
            if (workers == dataSourceWorkers_.end()) {
                lock.unlock();
                if (affectedMapId) {
                    invalidateMap(*affectedMapId);
                }
                if (change) {
                    notifySourceCatalogChanged(*change);
                }
                return;
            }
            for (auto& worker : workers->second) {
                workersToJoin.push_back(worker);
            }
            dataSourceWorkers_.erase(workers);
        }

        {
            std::unique_lock lock(jobsMutex_);
            for (auto& worker : workersToJoin) {
                worker->shouldTerminate_ = true;
            }
        }
        if (affectedMapId) {
            invalidateMap(*affectedMapId);
        }
        jobsAvailable_.notify_all();

        for (auto& worker : workersToJoin) {
            if (worker->thread_.joinable()) {
                worker->thread_.join();
            }
        }
        // The epoch prevents late publication, and this second pass also
        // covers cache writes performed before the epoch was advanced.
        if (affectedMapId) {
            cache_->invalidateMap(*affectedMapId);
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
            auto const currentConfigIndex = configIndex++;
            if (clientHeaders && !dataSource->isDataSourceAuthorized(*clientHeaders)) {
                continue;
            }

            auto infoCopy = cloneDataSourceInfo(info);
            auto sourceId = dataSourceSourceIds_.find(dataSource);
            snapshot.sources.push_back(DataSourceCatalogEntry{
                .descriptor = DataSourceDescriptor{
                    .configIndex = currentConfigIndex,
                    .sourceId = sourceId != dataSourceSourceIds_.end()
                        ? sourceId->second
                        : fmt::format(
                              "runtime-source-{}",
                              currentConfigIndex),
                    .type = "",
                    .displayName = fmt::format(
                        "datasource-{}-{}",
                        currentConfigIndex,
                        infoCopy.mapId_),
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
                auto auxBaseStringPoolId = baseTile->stringPoolId() + "|" + auxTile->stringPoolId();
                auto auxBaseStringPool = cache_->getStringPool(auxBaseStringPoolId);
                (void) baseTile->setStrings(auxBaseStringPool);
                baseTile->setStringPoolId(auxBaseStringPoolId);

                // Adopt new attributes, features and relations for the base feature
                // from the auxiliary feature.
                TileFeatureLayer::CloneCache clonedModelNodes;
                for (auto const& auxFeature : *auxTile)
                {
                    // Note: A single secondary feature ID may resolve to multiple
                    // primary feature IDs. So we keep a vector of aux feature ID info.
                    std::vector<
                        std::pair<
                            std::string,
                            KeyValuePairs>>
                        auxFeatureIds = {{
                            std::string(
                                auxFeature->id()
                                    ->typeId()),
                            castToKeyValue(
                                auxFeature->id()
                                    ->keyValuePairs())}};

                    // Convert the feature reference to multiple direct ones on-demand.
                    // If the ID does not validate as a primary feature id, we assume
                    // that it uses a secondary ID scheme for which a locate-call
                    // is required.
                    auto idIsIndirect = !baseTile->layerInfo()->validFeatureId(
                        auxFeatureIds[0].first,
                        castToKeyValueView(
                            auxFeatureIds[0]
                                .second),
                        true);
                    if (idIsIndirect)
                    {
                        auto candidates =
                            baseDataSource.locate(
                                LocateRequest(
                            auxTile->mapId(),
                            std::string(auxFeatureIds[0].first),
                            auxFeatureIds[0].second));
                        if (candidates.empty()) {
                            log().warn("Could not locate indirect aux feature id {}", auxFeature->id()->toString());
                            continue;
                        }
                        auxFeatureIds.clear();
                        for (auto const& candidate :
                             candidates)
                        {
                            if (candidate.tileKey_ !=
                                baseTile->id())
                            {
                                continue;
                            }
                            auto selected =
                                resolveLocateCandidate(
                                    candidate,
                                    *baseTile);
                            if (!selected) {
                                log().warn(
                                    "Could not evaluate indirect aux feature selector for {}: {}",
                                    auxFeature->id()
                                        ->toString(),
                                    selected.error()
                                        .message);
                                continue;
                            }
                            for (auto const& feature :
                                 *selected)
                            {
                                auxFeatureIds.emplace_back(
                                    std::string(
                                        feature
                                            ->typeId()),
                                    castToKeyValue(
                                        feature->id()
                                            ->keyValuePairs()));
                            }
                        }
                    }

                    // Go over all feature IDs to which the auxiliary feature data should be appended.
                    for (auto const& [auxFeatureType, auxFeatureKvp] : auxFeatureIds) {
                        baseTile->clone(
                            clonedModelNodes,
                            auxTile,
                            *auxFeature,
                            auxFeatureType,
                            castToKeyValueView(
                                auxFeatureKvp));
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
        auto context = resolveLayerRequest(
            r->mapId_,
            r->layerId_,
            clientHeaders,
            r->sourceId_);
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
            r->prepareResolvedLayer(context.layerType_);
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

bool Service::request(FeatureLayerFilterTilesRequest::Ptr const& request, std::optional<AuthHeaders> const& clientHeaders)
{
    if (!request) {
        raise("Attempt to request a null FeatureLayerFilterTilesRequest.");
    }
    if (request->isDone()) {
        request->notifyStatus();
        return true;
    }

    auto context = resolveLayerRequest(
        request->mapId_,
        request->layerId_,
        clientHeaders,
        request->sourceId_);
    if (context.status_ != RequestStatus::Success) {
        request->setStatus(context.status_);
        return false;
    }
    if (context.layerType_ != LayerType::Features) {
        request->setStatus(RequestStatus::NoDataSource);
        return false;
    }

    std::optional<DataSourceInfo> sourceInfo;
    DataSource::Ptr sourceDataSource;
    {
        std::shared_lock lock(impl_->dataSourcesMutex_);
        for (auto const& [dataSource, info] : impl_->dataSourceInfo_) {
            if (info.isAddOn_ || info.mapId_ != request->mapId_) {
                continue;
            }
            if (request->sourceId_) {
                auto sourceId =
                    impl_->dataSourceSourceIds_.find(dataSource);
                if (sourceId ==
                        impl_->dataSourceSourceIds_.end() ||
                    sourceId->second != *request->sourceId_)
                {
                    continue;
                }
            }
            if (info.layers_.find(request->layerId_) == info.layers_.end()) {
                continue;
            }
            if (clientHeaders && !dataSource->isDataSourceAuthorized(*clientHeaders)) {
                continue;
            }
            sourceInfo = info;
            sourceDataSource = dataSource;
            break;
        }
    }
    if (!sourceInfo) {
        request->setStatus(RequestStatus::NoDataSource);
        return false;
    }

    auto const hasPointGroups = std::ranges::any_of(
        request->filter_.channels_,
        [](auto const& channel) {
            return channel.group_.has_value();
        });
    auto const hasStoredRelations = std::ranges::any_of(
        request->filter_.channels_,
        [](auto const& channel) {
            return channel.scope_ ==
                FeatureLayerFilterScope::Relation;
        });
    if (request->exactRoots_.size() > 4096) {
        auto status =
            makeFilterStatusJson(*request, "Failed");
        status["error"] =
            "Stored-relation traversal exceeded the initial 4096 exact-root limit.";
        request->notifyProgress(status);
        request->setStatus(RequestStatus::Aborted);
        return false;
    }
    for (size_t rootIndex = 0;
         rootIndex < request->exactRoots_.size();
         ++rootIndex)
    {
        request->exactRoots_[rootIndex]
            .requestOrdinal_ = rootIndex;
    }
    if (!request->exactRoots_.empty() &&
        !hasStoredRelations)
    {
        auto status =
            makeFilterStatusJson(*request, "Failed");
        status["error"] =
            "Exact roots are valid only for a relation-scope filter bundle.";
        request->notifyProgress(status);
        request->setStatus(RequestStatus::Aborted);
        return false;
    }
    auto const requestedOutputMembership =
        std::set<TileId>(
            request->tileIds_.begin(),
            request->tileIds_.end());
    if (std::ranges::any_of(
            request->exactRoots_,
            [&](auto const& root) {
                return !requestedOutputMembership
                            .contains(root.tileId_);
            }))
    {
        auto status =
            makeFilterStatusJson(*request, "Failed");
        status["error"] =
            "Every exact relation root must belong to an original requested output tile.";
        request->notifyProgress(status);
        request->setStatus(RequestStatus::Aborted);
        return false;
    }

    std::vector<TileId> tileIdsToProcess = request->tileIds_;
    std::set<TileId> sourceTileMembership(
        tileIdsToProcess.begin(),
        tileIdsToProcess.end());
    if (hasPointGroups) {
        auto const level = request->tileIds_.front().level();
        for (auto const& tileId : request->tileIds_) {
            if (!tileId.isValid() || tileId.level() != level) {
                auto status = makeFilterStatusJson(*request, "Failed");
                status["error"] =
                    "Point-grid outputs must be valid tiles at one common level.";
                request->notifyProgress(status);
                request->setStatus(RequestStatus::Aborted);
                return false;
            }
        }
        auto const [tileWidth, tileHeight] =
            request->tileIds_.front().wgs84Size();
        for (auto const& channel : request->filter_.channels_) {
            if (!channel.group_) {
                continue;
            }
            if (!std::isfinite(channel.group_->cellSize_.x) ||
                !std::isfinite(channel.group_->cellSize_.y) ||
                channel.group_->cellSize_.x <= 0.0 ||
                channel.group_->cellSize_.y <= 0.0 ||
                channel.group_->cellSize_.x > tileWidth ||
                channel.group_->cellSize_.y > tileHeight)
            {
                auto status = makeFilterStatusJson(*request, "Failed");
                status["error"] = fmt::format(
                    "Point-grid channel '{}' exceeds the initial one-tile halo span.",
                    channel.channelId_);
                request->notifyProgress(status);
                request->setStatus(RequestStatus::Aborted);
                return false;
            }
        }

        // Requested outputs remain first. Halo-only source tiles are appended
        // in first-needed order, never promoted ahead of another output.
        for (auto const& outputTileId : request->tileIds_) {
            for (int32_t offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int32_t offsetX = -1; offsetX <= 1; ++offsetX) {
                    if (offsetX == 0 && offsetY == 0) {
                        continue;
                    }
                    auto const sourceTileId =
                        outputTileId.neighbour(offsetX, offsetY);
                    if (sourceTileMembership.insert(sourceTileId).second) {
                        tileIdsToProcess.push_back(sourceTileId);
                    }
                }
            }
        }
    }

    auto childRequest = std::make_shared<LayerTilesRequest>(
        request->mapId_,
        request->layerId_,
        tileIdsToProcess,
        std::vector<TileId>(
            request->priorityTileIds_.begin(),
            request->priorityTileIds_.end()));
    childRequest->sourceId_ = request->sourceId_;
    {
        std::lock_guard lock(
            request->childRequestsMutex_);
        request->childRequests_.push_back(
            childRequest);
    }

    struct FilterExecutionState
        : std::enable_shared_from_this<FilterExecutionState>
    {
        struct SourceTileContribution
        {
            TileSubsetDependency dependency_;
            std::vector<FeatureLayerPointGroupMember>
                pointGroupMembers_;
            std::vector<FeatureLayerRelationDescriptor>
                relationDescriptors_;
            std::vector<FilterIssue> issues_;
            std::map<std::string, simfil::Trace> traces_;
            simfil::Diagnostics diagnostics_;
        };

        struct OutputTileState
        {
            TileId tileId_;
            TileSubsetLayer::Ptr wipSubset_;
            std::vector<TileId> sourceTileIds_;
            std::vector<std::optional<SourceTileContribution>>
                contributions_;
            size_t missingContributions_ = 0;
            bool takenForCompletion_ = false;
            uint64_t wipSubsetBytes_ = 0;
        };

        struct DependentOutputSlot
        {
            size_t outputIndex_ = 0;
            size_t slotIndex_ = 0;
        };

        struct ReadyOutput
        {
            size_t outputIndex_ = 0;
            TileSubsetLayer::Ptr layer_;
            uint64_t layerBytes_ = 0;
            std::vector<SourceTileContribution> contributions_;
            std::map<
                MapTileKey,
                SourceTileContribution>
                dynamicContributions_;
            std::vector<FilterIssue> issues_;
        };

        struct PendingRelationOutput
        {
            ReadyOutput ready_;
            std::set<MapTileKey> pendingTargetTiles_;
        };

        struct RelationTargetTileState
        {
            bool scheduled_ = false;
            bool terminal_ = false;
            TileFeatureLayer::Ptr layer_;
            std::optional<std::string> failureMessage_;
            std::set<size_t> dependentOutputs_;
        };

        struct PreparedRelationOutputs
        {
            std::vector<ReadyOutput> ready_;
            std::vector<MapTileKey> targetsToSchedule_;
        };

        Service::Impl* impl = nullptr;
        Service* service = nullptr;
        FeatureLayerFilterTilesRequest::Ptr request;
        DataSourceInfo sourceInfo;
        DataSource::Ptr sourceDataSource;
        std::optional<AuthHeaders> clientHeaders;
        bool hasPointGroups = false;
        bool hasStoredRelations = false;
        int outputLevel = 0;

        std::vector<TileId> sourceTileIds;
        std::map<TileId, size_t> sourceIndexByTile;
        std::map<TileId, size_t> outputIndexByTile;
        std::vector<OutputTileState> outputs;
        std::vector<std::vector<DependentOutputSlot>>
            dependentOutputsBySource;
        std::vector<bool> receivedSourceTiles;
        std::vector<bool> committedSourceTiles;
        std::set<std::string> groupChannelIds;
        std::map<
            std::string,
            std::vector<LocateCandidate>>
            relationLocationCache;
        std::mutex relationLocationMutex;
        std::map<
            size_t,
            PendingRelationOutput>
            pendingRelationOutputs;
        std::map<
            MapTileKey,
            RelationTargetTileState>
            relationTargetTiles;
        std::mutex mutex;
        size_t loadedSourceTiles = 0;
        size_t evaluatedSourceTiles = 0;
        size_t readyOutputTiles = 0;
        size_t emittedOutputTiles = 0;
        size_t entriesEmitted = 0;
        size_t pendingEvaluationJobs = 0;
        std::atomic_size_t progressEventCount = 0;
        bool childRequestDone = false;
        bool terminal = false;
        std::shared_ptr<FilterMemoryTracker> memory;

        ~FilterExecutionState()
        {
            // No request-owned model remains once every callback has released
            // this execution state. Peaks intentionally survive in the tracker.
            if (memory) {
                memory->sourceTileModels.set(0);
                memory->outputSubsetModels.set(0);
                memory->relationTargetModels.set(0);
                memory->evaluationTemporaries.set(0);
                memory->orchestration.set(0);
            }
        }

        /** Recompute a conservative lower bound for request orchestration containers. */
        [[nodiscard]] uint64_t orchestrationBytesLocked() const
        {
            MemoryUsageBreakdown usage;
            usage.add("state", {sizeof(FilterExecutionState), sizeof(FilterExecutionState)});
            usage.add("request", {
                sizeof(FeatureLayerFilterTilesRequest),
                sizeof(FeatureLayerFilterTilesRequest),
            });
            usage.add("request-strings", stringMemoryUsage(request->mapId_));
            usage.add("request-strings", stringMemoryUsage(request->layerId_));
            if (request->sourceId_) {
                usage.add("request-strings", stringMemoryUsage(*request->sourceId_));
            }
            usage.add("requested-tile-ids", vectorMemoryUsage(request->tileIds_));
            usage.add("priority-tile-index", {
                request->priorityTileIds_.size() * sizeof(TileId),
                request->priorityTileIds_.size() * (sizeof(TileId) + 3 * sizeof(void*)),
            });
            usage.add("exact-roots", vectorMemoryUsage(request->exactRoots_));
            for (auto const& root : request->exactRoots_) {
                usage.add("exact-root-strings", stringMemoryUsage(root.typeId_));
                usage.add("exact-root-strings", stringMemoryUsage(root.canonicalFeatureId_));
                for (auto const& [key, value] : root.featureId_) {
                    usage.add("exact-root-strings", stringMemoryUsage(key));
                    if (auto string = std::get_if<std::string>(&value)) {
                        usage.add("exact-root-strings", stringMemoryUsage(*string));
                    }
                }
            }
            usage.add("filter-id", stringMemoryUsage(request->filter_.filterId_));
            usage.add("filter-channels", vectorMemoryUsage(request->filter_.channels_));
            for (auto const& channel : request->filter_.channels_) {
                usage.add("filter-channel-strings", stringMemoryUsage(channel.channelId_));
                if (channel.featureFilter_) {
                    usage.add("filter-channel-strings", stringMemoryUsage(*channel.featureFilter_));
                }
                if (channel.entryFilter_) {
                    usage.add("filter-channel-strings", stringMemoryUsage(*channel.entryFilter_));
                }
                if (channel.geometryName_) {
                    usage.add("filter-channel-strings", stringMemoryUsage(*channel.geometryName_));
                }
                if (channel.relation_ && channel.relation_->relationNamePattern_) {
                    usage.add("filter-channel-strings", stringMemoryUsage(*channel.relation_->relationNamePattern_));
                }
                usage.add("filter-feature-types", stringVectorMemoryUsage(channel.featureTypes_));
                usage.add("filter-feature-fields", stringVectorMemoryUsage(channel.featureFields_));
                usage.add("filter-entry-fields", stringVectorMemoryUsage(channel.entryFields_));
            }
            usage.add("filter-bindings", {
                request->filter_.bindings_.size() *
                    sizeof(decltype(request->filter_.bindings_)::value_type),
                request->filter_.bindings_.size() *
                    (sizeof(decltype(request->filter_.bindings_)::value_type) + 3 * sizeof(void*)),
            });
            for (auto const& [name, value] : request->filter_.bindings_) {
                usage.add("filter-binding-strings", stringMemoryUsage(name));
                if (auto string = std::get_if<std::string>(&value)) {
                    usage.add("filter-binding-strings", stringMemoryUsage(*string));
                }
            }
            usage.add("source-tile-ids", vectorMemoryUsage(sourceTileIds));
            usage.add("source-index", {
                sourceIndexByTile.size() * sizeof(decltype(sourceIndexByTile)::value_type),
                sourceIndexByTile.size() *
                    (sizeof(decltype(sourceIndexByTile)::value_type) + 3 * sizeof(void*)),
            });
            usage.add("output-index", {
                outputIndexByTile.size() * sizeof(decltype(outputIndexByTile)::value_type),
                outputIndexByTile.size() *
                    (sizeof(decltype(outputIndexByTile)::value_type) + 3 * sizeof(void*)),
            });
            usage.add("outputs", vectorMemoryUsage(outputs));
            for (auto const& output : outputs) {
                usage.add("output-source-ids", vectorMemoryUsage(output.sourceTileIds_));
                usage.add("output-contributions", vectorMemoryUsage(output.contributions_));
                for (auto const& contribution : output.contributions_) {
                    if (!contribution) {
                        continue;
                    }
                    usage.add("point-group-members", vectorMemoryUsage(contribution->pointGroupMembers_));
                    usage.add("relation-descriptors", vectorMemoryUsage(contribution->relationDescriptors_));
                    usage.add("issues", vectorMemoryUsage(contribution->issues_));
                    for (auto const& issue : contribution->issues_) {
                        usage.add("issue-strings", stringMemoryUsage(issue.channelId_));
                        usage.add("issue-strings", stringMemoryUsage(issue.expression_));
                        usage.add("issue-strings", stringMemoryUsage(issue.message_));
                    }
                    usage.add("diagnostics", diagnosticsMemoryUsage(contribution->diagnostics_));
                    usage.add("trace-index", {
                        contribution->traces_.size() * sizeof(decltype(contribution->traces_)::value_type),
                        contribution->traces_.size() *
                            (sizeof(decltype(contribution->traces_)::value_type) + 3 * sizeof(void*)),
                    });
                    for (auto const& [name, _] : contribution->traces_) {
                        usage.add("trace-names", stringMemoryUsage(name));
                    }
                }
            }
            usage.add("dependent-output-lists", vectorMemoryUsage(dependentOutputsBySource));
            for (auto const& dependents : dependentOutputsBySource) {
                usage.add("dependent-output-slots", vectorMemoryUsage(dependents));
            }
            usage.add("received-source-flags", {
                (receivedSourceTiles.size() + 7) / 8,
                (receivedSourceTiles.capacity() + 7) / 8,
            });
            usage.add("committed-source-flags", {
                (committedSourceTiles.size() + 7) / 8,
                (committedSourceTiles.capacity() + 7) / 8,
            });
            usage.add("group-channel-index", {
                groupChannelIds.size() * sizeof(decltype(groupChannelIds)::value_type),
                groupChannelIds.size() *
                    (sizeof(decltype(groupChannelIds)::value_type) + 3 * sizeof(void*)),
            });
            for (auto const& id : groupChannelIds) {
                usage.add("group-channel-ids", stringMemoryUsage(id));
            }
            usage.add("pending-relation-outputs", {
                pendingRelationOutputs.size() * sizeof(decltype(pendingRelationOutputs)::value_type),
                pendingRelationOutputs.size() *
                    (sizeof(decltype(pendingRelationOutputs)::value_type) + 3 * sizeof(void*)),
            });
            usage.add("relation-target-tiles", {
                relationTargetTiles.size() * sizeof(decltype(relationTargetTiles)::value_type),
                relationTargetTiles.size() *
                    (sizeof(decltype(relationTargetTiles)::value_type) + 3 * sizeof(void*)),
            });
            for (auto const& [_, target] : relationTargetTiles) {
                usage.add("relation-target-dependents", {
                    target.dependentOutputs_.size() * sizeof(size_t),
                    target.dependentOutputs_.size() *
                        (sizeof(size_t) + 3 * sizeof(void*)),
                });
                if (target.failureMessage_) {
                    usage.add("relation-target-errors", stringMemoryUsage(*target.failureMessage_));
                }
            }
            return usage.total().allocatedBytes;
        }

        /** Measure source-local vectors that exist between SIMFIL evaluation and state commit. */
        [[nodiscard]] static uint64_t sourceResultAuxiliaryBytes(
            FeatureLayerFilterSourceResult const& result)
        {
            MemoryUsageBreakdown usage;
            usage.add("point-group-members", vectorMemoryUsage(result.pointGroupMembers_));
            for (auto const& member : result.pointGroupMembers_) {
                if (member.geometryName_) {
                    usage.add("point-group-geometry-names", stringMemoryUsage(*member.geometryName_));
                }
            }
            usage.add("relation-descriptors", vectorMemoryUsage(result.relationDescriptors_));
            for (auto const& descriptor : result.relationDescriptors_) {
                usage.add("relation-target-types", stringMemoryUsage(descriptor.targetTypeId_));
                for (auto const& [key, value] : descriptor.targetFeatureId_) {
                    usage.add("relation-target-id-strings", stringMemoryUsage(key));
                    if (auto string = std::get_if<std::string>(&value)) {
                        usage.add("relation-target-id-strings", stringMemoryUsage(*string));
                    }
                }
                usage.add("relation-candidates", vectorMemoryUsage(descriptor.targetCandidates_));
                usage.add("relation-matches", vectorMemoryUsage(descriptor.targetMatches_));
            }
            usage.add("issues", vectorMemoryUsage(result.issues_));
            for (auto const& issue : result.issues_) {
                usage.add("issue-strings", stringMemoryUsage(issue.channelId_));
                usage.add("issue-strings", stringMemoryUsage(issue.expression_));
                usage.add("issue-strings", stringMemoryUsage(issue.message_));
            }
            usage.add("diagnostics", diagnosticsMemoryUsage(result.diagnostics_));
            usage.add("trace-index", {
                result.traces_.size() * sizeof(decltype(result.traces_)::value_type),
                result.traces_.size() *
                    (sizeof(decltype(result.traces_)::value_type) + 3 * sizeof(void*)),
            });
            for (auto const& [name, _] : result.traces_) {
                usage.add("trace-names", stringMemoryUsage(name));
            }
            return usage.total().allocatedBytes;
        }

        void configure(
            std::vector<TileId> const& outputTileIds,
            std::vector<TileId> processingTileIds)
        {
            sourceTileIds = std::move(processingTileIds);
            receivedSourceTiles.resize(sourceTileIds.size(), false);
            committedSourceTiles.resize(sourceTileIds.size(), false);
            dependentOutputsBySource.resize(sourceTileIds.size());
            for (size_t index = 0;
                 index < sourceTileIds.size();
                 ++index)
            {
                sourceIndexByTile.emplace(
                    sourceTileIds[index],
                    index);
            }
            for (auto const& channel : request->filter_.channels_) {
                if (channel.group_) {
                    groupChannelIds.insert(channel.channelId_);
                }
            }

            outputs.reserve(outputTileIds.size());
            for (size_t outputIndex = 0;
                 outputIndex < outputTileIds.size();
                 ++outputIndex)
            {
                auto const outputTileId =
                    outputTileIds[outputIndex];
                outputIndexByTile.emplace(
                    outputTileId,
                    outputIndex);

                std::set<TileId> dependencyMembership{
                    outputTileId};
                if (hasPointGroups) {
                    for (int32_t offsetY = -1;
                         offsetY <= 1;
                         ++offsetY)
                    {
                        for (int32_t offsetX = -1;
                             offsetX <= 1;
                             ++offsetX)
                        {
                            dependencyMembership.insert(
                                outputTileId.neighbour(
                                    offsetX,
                                    offsetY));
                        }
                    }
                }

                OutputTileState output;
                output.tileId_ = outputTileId;
                for (auto const& sourceTileId :
                     sourceTileIds)
                {
                    if (dependencyMembership.contains(
                            sourceTileId))
                    {
                        output.sourceTileIds_.push_back(
                            sourceTileId);
                    }
                }
                output.contributions_.resize(
                    output.sourceTileIds_.size());
                output.missingContributions_ =
                    output.sourceTileIds_.size();
                outputs.push_back(std::move(output));
            }

            for (size_t outputIndex = 0;
                 outputIndex < outputs.size();
                 ++outputIndex)
            {
                auto const& output = outputs[outputIndex];
                for (size_t slotIndex = 0;
                     slotIndex < output.sourceTileIds_.size();
                     ++slotIndex)
                {
                    dependentOutputsBySource.at(
                        sourceIndexByTile.at(
                            output.sourceTileIds_[slotIndex]))
                        .push_back({
                            outputIndex,
                            slotIndex,
                        });
                }
            }
        }

        [[nodiscard]] nlohmann::json progress(
            std::string state)
        {
            std::lock_guard lock(mutex);
            auto status =
                makeFilterStatusJson(*request, std::move(state));
            status["outputTilesRequested"] = outputs.size();
            status["sourceTilesQueued"] = sourceTileIds.size();
            status["sourceTilesLoaded"] = loadedSourceTiles;
            status["sourceTilesEvaluated"] =
                evaluatedSourceTiles;
            status["outputTilesReady"] = readyOutputTiles;
            status["outputTilesEmitted"] =
                emittedOutputTiles;
            status["entriesEmitted"] = entriesEmitted;
            return status;
        }

        void emitProgress(
            std::string state,
            bool force = false)
        {
            // Per-source callbacks can outnumber useful UI refreshes by
            // thousands in a large viewport. Keep exact counters in request
            // state, but serialize only periodic intermediate snapshots.
            constexpr size_t ProgressEventStride = 32;
            if (!force) {
                auto const event =
                    progressEventCount.fetch_add(
                        1,
                        std::memory_order_relaxed) + 1;
                if (event % ProgressEventStride != 0) {
                    return;
                }
            }
            request->notifyProgress(
                progress(std::move(state)));
        }

        void releaseChildRequests()
        {
            std::lock_guard lock(
                request->childRequestsMutex_);
            request->childRequests_.clear();
        }

        void abortChildRequests()
        {
            std::vector<LayerTilesRequest::Ptr> children;
            {
                std::lock_guard lock(
                    request->childRequestsMutex_);
                children = request->childRequests_;
                request->childRequests_.clear();
            }
            for (auto const& child : children) {
                if (child && !child->isDone()) {
                    impl->abortRequest(child);
                }
            }
        }

        void finishIfComplete()
        {
            RequestStatus finalStatus =
                RequestStatus::Open;
            {
                std::lock_guard lock(mutex);
                if (terminal ||
                    !childRequestDone ||
                    pendingEvaluationJobs != 0 ||
                    evaluatedSourceTiles <
                        sourceTileIds.size() ||
                    emittedOutputTiles < outputs.size())
                {
                    return;
                }
                terminal = true;
                finalStatus = request->isCancelled()
                    ? RequestStatus::Aborted
                    : RequestStatus::Success;
            }
            releaseChildRequests();
            emitProgress(
                finalStatus == RequestStatus::Success
                    ? "Success"
                    : "Aborted",
                true);
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
                "Filter {} generation {} failed for {}::{}: {}",
                request->filter_.filterId_,
                request->filter_.generation_,
                request->mapId_,
                request->layerId_,
                error.message);
            auto status =
                makeFilterStatusJson(*request, "Failed");
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
            auto found =
                sourceIndexByTile.find(layer->tileId());
            if (found == sourceIndexByTile.end()) {
                fail(simfil::Error{
                    simfil::Error::InternalError,
                    "Filter source request returned an unplanned tile.",
                });
                return;
            }

            bool duplicate = false;
            auto const sourceIndex = found->second;
            {
                std::lock_guard lock(mutex);
                if (terminal) {
                    return;
                }
                if (receivedSourceTiles[sourceIndex]) {
                    // The ordinary child request is stable-deduplicated, so a
                    // second delivery is an internal scheduler violation.
                    duplicate = true;
                }
                else {
                    receivedSourceTiles[sourceIndex] = true;
                    ++loadedSourceTiles;
                    ++pendingEvaluationJobs;
                }
            }
            if (duplicate) {
                fail(simfil::Error{
                    simfil::Error::InternalError,
                    "Filter source tile was delivered more than once.",
                });
                return;
            }

            // Source jobs are admitted by LayerTilesRequest in request order,
            // but their datasource work may complete in parallel. Evaluate a
            // completed source immediately: waiting for every preceding
            // completion creates an unnecessary request-wide head-of-line
            // barrier and retains complete source models while idle.
            auto self = shared_from_this();
            auto const sourceBytes = layer->memoryUsage().total().allocatedBytes;
            memory->sourceTileModels.add(sourceBytes);
            impl->enqueueFilterEvalJob(
                sourceInfo.mapId_,
                request,
                [self,
                 sourceIndex,
                 sourceBytes,
                 source = std::move(layer)]() mutable {
                    self->evaluate(
                        sourceIndex,
                        sourceBytes,
                        std::move(source));
                },
                [self, sourceBytes]() {
                    self->discardEvaluationJob(sourceBytes);
                });
            emitProgress("SourceTileLoaded");
        }

        void discardEvaluationJob(uint64_t sourceBytes)
        {
            memory->sourceTileModels.subtract(sourceBytes);
            {
                std::lock_guard lock(mutex);
                if (pendingEvaluationJobs > 0) {
                    --pendingEvaluationJobs;
                }
            }
            finishIfComplete();
        }

        tl::expected<void, simfil::Error>
        locateRelationTargets(
            TileFeatureLayer const& source,
            FeatureLayerFilterSourceResult& result)
        {
            if (result.relationDescriptors_.size() >
                100000)
            {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InvalidArguments,
                    "Stored-relation traversal exceeded the initial 100000 directed-relation limit.",
                });
            }

            std::vector<FeatureLayerRelationDescriptor>
                retained;
            retained.reserve(
                result.relationDescriptors_.size());
            for (auto&& descriptor :
                 result.relationDescriptors_)
            {
                if (descriptor.target_) {
                    retained.push_back(
                        std::move(descriptor));
                    continue;
                }
                auto targetId =
                    descriptor.relation_
                        ? descriptor.relation_->target()
                        : model_ptr<FeatureId>{};
                auto const channelId =
                    descriptor.channelIndex_ <
                            request->filter_.channels_.size()
                    ? request->filter_
                          .channels_[
                              descriptor.channelIndex_]
                          .channelId_
                    : std::string{};
                auto addIssue =
                    [&](std::string message) {
                        result.issues_.push_back(
                            FilterIssue{
                                channelId,
                                "<relation-target>",
                                Scope::Relation,
                                std::move(message),
                                1,
                            });
                    };
                if (!targetId) {
                    addIssue(
                        "Stored relation has no target feature identity.");
                    continue;
                }
                if (auto externalMap =
                        targetId->externalMapId();
                    externalMap &&
                    *externalMap != request->mapId_)
                {
                    addIssue(fmt::format(
                        "Cross-map relation target '{}' is unsupported.",
                        targetId->toString()));
                    continue;
                }

                LocateRequest locateRequest(
                    request->mapId_,
                    descriptor.targetTypeId_,
                    descriptor.targetFeatureId_);
                auto const locationKey =
                    locateRequest.serialize().dump();
                std::vector<LocateCandidate> locations;
                bool locationCached = false;
                {
                    std::lock_guard lock(
                        relationLocationMutex);
                    auto found =
                        relationLocationCache.find(
                            locationKey);
                    if (found !=
                        relationLocationCache.end())
                    {
                        locations = found->second;
                        locationCached = true;
                    }
                }
                if (!locationCached) {
                    // Never invoke a potentially remote datasource callback
                    // while holding the shared location-cache mutex. Racing
                    // misses may perform duplicate idempotent locate work;
                    // the first normalized value installed wins.
                    auto located =
                        sourceDataSource->locate(
                            locateRequest);
                    std::map<
                        std::string,
                        LocateCandidate>
                        unique;
                    for (auto&& location : located) {
                        if (location.tileKey_.layer_ !=
                                LayerType::Features ||
                            location.tileKey_.mapId_ !=
                                request->mapId_)
                        {
                            continue;
                        }
                        unique.try_emplace(
                            location.serialize().dump(),
                            std::move(location));
                    }
                    std::vector<LocateCandidate>
                        normalized;
                    normalized.reserve(unique.size());
                    for (auto&& [_, location] : unique) {
                        normalized.push_back(
                            std::move(location));
                    }
                    std::lock_guard lock(
                        relationLocationMutex);
                    auto [found, _] =
                        relationLocationCache
                            .try_emplace(
                                locationKey,
                                std::move(normalized));
                    locations = found->second;
                }

                if (locations.empty()) {
                    addIssue(fmt::format(
                        "Could not locate relation target '{}'.",
                        targetId->toString()));
                    continue;
                }
                bool selectorFailed = false;
                for (auto const& location :
                     locations)
                {
                    auto& candidate =
                        descriptor
                            .targetCandidates_
                            .emplace_back(
                                FeatureLayerRelationTargetCandidate{
                                    location
                                        .tileKey_,
                                    location
                                        .selector_,
                                    false});
                    if (location.tileKey_ !=
                        source.id())
                    {
                        continue;
                    }
                    auto selected =
                        resolveLocateCandidate(
                            location,
                            source);
                    if (!selected) {
                        addIssue(fmt::format(
                            "Could not evaluate relation-target selector for '{}': {}",
                            targetId->toString(),
                            selected.error().message));
                        selectorFailed = true;
                        break;
                    }
                    candidate.resolved_ = true;
                    descriptor.targetMatches_.insert(
                        descriptor
                            .targetMatches_.end(),
                        selected->begin(),
                        selected->end());
                }
                if (selectorFailed) {
                    continue;
                }
                if (std::ranges::all_of(
                        descriptor
                            .targetCandidates_,
                        &FeatureLayerRelationTargetCandidate::
                            resolved_))
                {
                    std::map<
                        std::pair<
                            MapTileKey,
                            std::string>,
                        model_ptr<Feature>>
                        uniqueMatches;
                    for (auto const& match :
                         descriptor
                             .targetMatches_)
                    {
                        uniqueMatches.emplace(
                            std::make_pair(
                                MapTileKey(
                                    match->model()),
                                match->id()
                                    ->toString()),
                            match);
                    }
                    if (uniqueMatches.size() != 1) {
                        addIssue(
                            uniqueMatches.empty()
                            ? fmt::format(
                                  "Located relation target '{}' was not found in its candidate tiles.",
                                  targetId->toString())
                            : fmt::format(
                                  "Relation target '{}' resolved ambiguously to {} features.",
                                  targetId->toString(),
                                  uniqueMatches.size()));
                        continue;
                    }
                    descriptor.target_ =
                        uniqueMatches.begin()
                            ->second;
                    descriptor.targetTileKey_ =
                        uniqueMatches.begin()
                            ->first.first;
                    descriptor.targetTypeId_ =
                        std::string(
                            descriptor.target_
                                ->typeId());
                    descriptor.targetFeatureId_ =
                        castToKeyValue(
                            descriptor.target_->id()
                                ->keyValuePairs());
                }
                retained.push_back(
                    std::move(descriptor));
            }
            result.relationDescriptors_ =
                std::move(retained);
            return {};
        }

        tl::expected<std::vector<ReadyOutput>, simfil::Error>
        commitSource(
            size_t sourceIndex,
            TileFeatureLayer const& source,
            uint64_t outputModelBytes,
            FeatureLayerFilterSourceResult result)
        {
            std::map<
                size_t,
                std::vector<FeatureLayerPointGroupMember>>
                membersByOutput;
            for (auto&& member :
                 result.pointGroupMembers_)
            {
                auto owner = pointGroupOwnerTile(
                    member,
                    request->filter_,
                    outputLevel);
                if (!owner) {
                    return tl::unexpected(owner.error());
                }
                auto output =
                    outputIndexByTile.find(*owner);
                if (output != outputIndexByTile.end()) {
                    auto const& dependents =
                        dependentOutputsBySource[sourceIndex];
                    if (std::ranges::none_of(
                            dependents,
                            [&](auto const& dependent) {
                                return dependent.outputIndex_ ==
                                    output->second;
                            }))
                    {
                        return tl::unexpected(simfil::Error{
                            simfil::Error::InternalError,
                            "Point-group member escaped the configured source halo.",
                        });
                    }
                    membersByOutput[output->second]
                        .push_back(std::move(member));
                }
            }

            std::vector<ReadyOutput> ready;
            {
                std::lock_guard lock(mutex);
                if (terminal || request->isCancelled()) {
                    return ready;
                }
                if (sourceIndex >=
                        committedSourceTiles.size() ||
                    committedSourceTiles[sourceIndex])
                {
                    return tl::unexpected(simfil::Error{
                        simfil::Error::InternalError,
                        "Filter source tile committed more than once.",
                    });
                }

                auto outputForSource =
                    outputIndexByTile.find(source.tileId());
                if (outputForSource !=
                    outputIndexByTile.end())
                {
                    if (!result.layer_) {
                        return tl::unexpected(simfil::Error{
                            simfil::Error::InternalError,
                            "Requested filter output source produced no WIP subset.",
                        });
                    }
                    outputs[outputForSource->second]
                        .wipSubset_ =
                        std::move(result.layer_);
                    outputs[outputForSource->second]
                        .wipSubsetBytes_ = outputModelBytes;
                }
                else if (result.layer_) {
                    return tl::unexpected(simfil::Error{
                        simfil::Error::InternalError,
                        "Halo-only filter source produced an output subset.",
                    });
                }

                auto const dependency =
                    TileSubsetDependency{
                        MapTileKey(source),
                        result.sourceFeatureCount_,
                    };
                for (auto const& dependent :
                     dependentOutputsBySource[sourceIndex])
                {
                    auto& output =
                        outputs[dependent.outputIndex_];
                    auto& slot =
                        output.contributions_[
                            dependent.slotIndex_];
                    if (slot) {
                        return tl::unexpected(simfil::Error{
                            simfil::Error::InternalError,
                            "Filter contribution slot was written more than once.",
                        });
                    }

                    std::vector<FilterIssue> issues;
                    issues.reserve(result.issues_.size());
                    auto const isLocalOutput =
                        output.tileId_ == source.tileId();
                    for (auto const& issue :
                         result.issues_)
                    {
                        if (isLocalOutput ||
                            groupChannelIds.contains(
                                issue.channelId_))
                        {
                            issues.push_back(issue);
                        }
                    }
                    simfil::Diagnostics contributionDiagnostics;
                    contributionDiagnostics.append(
                        result.diagnostics_);
                    slot.emplace(SourceTileContribution{
                        dependency,
                        std::move(
                            membersByOutput[
                                dependent.outputIndex_]),
                        isLocalOutput
                            ? std::move(
                                  result
                                      .relationDescriptors_)
                            : std::vector<
                                  FeatureLayerRelationDescriptor>{},
                        std::move(issues),
                        result.traces_,
                        std::move(
                            contributionDiagnostics),
                    });
                    --output.missingContributions_;
                    if (output.missingContributions_ == 0) {
                        if (!output.wipSubset_) {
                            return tl::unexpected(simfil::Error{
                                simfil::Error::InternalError,
                                "Complete filter output has no WIP subset.",
                            });
                        }
                        output.takenForCompletion_ = true;
                        ReadyOutput item{
                            dependent.outputIndex_,
                            std::move(output.wipSubset_),
                            output.wipSubsetBytes_,
                            {},
                        };
                        item.contributions_.reserve(
                            output.contributions_.size());
                        for (auto& contribution :
                             output.contributions_)
                        {
                            if (!contribution) {
                                return tl::unexpected(
                                    simfil::Error{
                                        simfil::Error::InternalError,
                                        "Complete filter output has an empty contribution slot.",
                                    });
                            }
                            item.contributions_.push_back(
                                std::move(*contribution));
                        }
                        ready.push_back(std::move(item));
                    }
                }

                committedSourceTiles[sourceIndex] = true;
                ++evaluatedSourceTiles;
            }
            std::ranges::sort(
                ready,
                {},
                &ReadyOutput::outputIndex_);
            return ready;
        }

        tl::expected<void, simfil::Error>
        resolveRelationTargetInOutput(
            ReadyOutput& output,
            MapTileKey const& targetKey,
            TileFeatureLayer const& targetLayer)
        {
            if (targetLayer.size() >
                static_cast<size_t>(
                    std::numeric_limits<uint32_t>::max()))
            {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InvalidArguments,
                    "Relation target feature count exceeds the subset dependency representation.",
                });
            }
            SourceTileContribution targetContribution{
                TileSubsetDependency{
                    targetKey,
                    static_cast<uint32_t>(
                        targetLayer.size()),
                },
                {},
                {},
                {},
                {},
            };

            for (auto& contribution :
                 output.contributions_)
            {
                for (auto& descriptor :
                     contribution.relationDescriptors_)
                {
                    if (descriptor.target_)
                    {
                        continue;
                    }
                    for (auto& candidate :
                         descriptor
                             .targetCandidates_)
                    {
                        if (candidate.resolved_ ||
                            candidate.tileKey_ !=
                                targetKey)
                        {
                            continue;
                        }
                        auto selected =
                            selectFeatureLayerFeatures(
                                targetLayer,
                                candidate
                                    .selector_);
                        if (!selected) {
                            return tl::unexpected(
                                selected.error());
                        }
                        candidate.resolved_ = true;
                        descriptor
                            .targetMatches_
                            .insert(
                                descriptor
                                    .targetMatches_
                                    .end(),
                                selected->begin(),
                                selected->end());
                    }
                }
            }
            output.dynamicContributions_.erase(
                targetKey);
            output.dynamicContributions_.emplace(
                targetKey,
                std::move(targetContribution));
            return {};
        }

        void markRelationTargetUnavailableInOutput(
            ReadyOutput& output,
            MapTileKey const& targetKey,
            std::string const& failureMessage)
        {
            std::map<std::string, uint64_t>
                affectedByChannel;
            for (auto& contribution :
                 output.contributions_)
            {
                for (auto& descriptor :
                     contribution.relationDescriptors_)
                {
                    if (descriptor.target_) {
                        continue;
                    }
                    bool affected = false;
                    for (auto& candidate :
                         descriptor.targetCandidates_)
                    {
                        if (candidate.resolved_ ||
                            candidate.tileKey_ !=
                                targetKey)
                        {
                            continue;
                        }
                        candidate.resolved_ = true;
                        affected = true;
                    }
                    if (!affected) {
                        continue;
                    }
                    auto const channelId =
                        descriptor.channelIndex_ <
                                request->filter_
                                    .channels_.size()
                        ? request->filter_
                              .channels_[
                                  descriptor
                                      .channelIndex_]
                              .channelId_
                        : std::string{};
                    ++affectedByChannel[channelId];
                }
            }
            for (auto const& [channelId, count] :
                 affectedByChannel)
            {
                output.issues_.push_back(
                    FilterIssue{
                        channelId,
                        "<relation-target>",
                        Scope::Relation,
                        failureMessage,
                        count,
                    });
            }
        }

        tl::expected<
            PreparedRelationOutputs,
            simfil::Error>
        prepareRelationOutputs(
            std::vector<ReadyOutput> fixedReady)
        {
            PreparedRelationOutputs prepared;
            std::lock_guard lock(mutex);
            if (terminal || request->isCancelled()) {
                return prepared;
            }

            for (auto&& ready : fixedReady) {
                std::set<MapTileKey> targetKeys;
                for (auto const& contribution :
                     ready.contributions_)
                {
                    for (auto const& descriptor :
                         contribution
                             .relationDescriptors_)
                    {
                        if (descriptor.target_) {
                            continue;
                        }
                        for (auto const& candidate :
                             descriptor
                                 .targetCandidates_)
                        {
                            if (!candidate.resolved_) {
                                targetKeys.insert(
                                    candidate
                                        .tileKey_);
                            }
                        }
                    }
                }

                std::set<MapTileKey> pendingKeys;
                for (auto const& targetKey :
                     targetKeys)
                {
                    auto [targetState, inserted] =
                        relationTargetTiles
                            .try_emplace(targetKey);
                    if (inserted &&
                        relationTargetTiles.size() >
                            2048)
                    {
                        return tl::unexpected(
                            simfil::Error{
                                simfil::Error::InvalidArguments,
                                "Stored-relation traversal exceeded the initial 2048 unique-target-tile limit.",
                            });
                    }
                    if (targetState->second.terminal_) {
                        if (targetState->second.layer_) {
                            auto resolved =
                                resolveRelationTargetInOutput(
                                    ready,
                                    targetKey,
                                    *targetState->second
                                         .layer_);
                            if (!resolved) {
                                return tl::unexpected(
                                    resolved.error());
                            }
                        }
                        else {
                            markRelationTargetUnavailableInOutput(
                                ready,
                                targetKey,
                                targetState->second
                                    .failureMessage_
                                    .value_or(
                                        fmt::format(
                                            "Could not load relation target tile {}.",
                                            targetKey
                                                .toString())));
                        }
                        continue;
                    }
                    pendingKeys.insert(targetKey);
                    targetState->second
                        .dependentOutputs_
                        .insert(ready.outputIndex_);
                    if (!targetState->second
                             .scheduled_)
                    {
                        targetState->second
                            .scheduled_ = true;
                        prepared.targetsToSchedule_
                            .push_back(targetKey);
                    }
                }

                if (pendingKeys.empty()) {
                    prepared.ready_.push_back(
                        std::move(ready));
                    ++readyOutputTiles;
                    continue;
                }
                auto [pending, inserted] =
                    pendingRelationOutputs.emplace(
                        ready.outputIndex_,
                        PendingRelationOutput{
                            std::move(ready),
                            std::move(pendingKeys),
                        });
                if (!inserted) {
                    return tl::unexpected(
                        simfil::Error{
                            simfil::Error::InternalError,
                            "Filter output entered relation-target resolution more than once.",
                        });
                }
            }
            return prepared;
        }

        void scheduleRelationTarget(
            MapTileKey const& targetKey)
        {
            auto child =
                std::make_shared<LayerTilesRequest>(
                    targetKey.mapId_,
                    targetKey.layerId_,
                    std::vector<TileId>{
                        targetKey.tileId_});
            child->sourceId_ =
                request->sourceId_;
            auto self = shared_from_this();
            child->onFeatureLayer(
                [self, targetKey](
                    TileFeatureLayer::Ptr layer)
                {
                    self->collectRelationTarget(
                        targetKey,
                        std::move(layer));
                });
            child->onDone_ =
                [self, targetKey](
                    RequestStatus status)
                {
                    self
                        ->completeUnavailableRelationTarget(
                            targetKey,
                            status ==
                                    RequestStatus::
                                        Success
                                ? fmt::format(
                                      "Relation target request completed without tile {}.",
                                      targetKey
                                          .toString())
                                : fmt::format(
                                      "Could not load relation target tile {}.",
                                      targetKey
                                          .toString()));
                };
            {
                std::lock_guard lock(
                    request->childRequestsMutex_);
                request->childRequests_.push_back(
                    child);
            }
            if (!service->request(
                    std::vector<
                        LayerTilesRequest::Ptr>{
                        child},
                    clientHeaders))
            {
                completeUnavailableRelationTarget(
                    targetKey,
                    fmt::format(
                        "Could not schedule relation target tile {}.",
                        targetKey.toString()));
            }
        }

        void completeUnavailableRelationTarget(
            MapTileKey const& targetKey,
            std::string message)
        {
            std::vector<ReadyOutput> ready;
            {
                std::lock_guard lock(mutex);
                if (terminal ||
                    request->isCancelled())
                {
                    return;
                }
                auto found =
                    relationTargetTiles.find(
                        targetKey);
                if (found ==
                    relationTargetTiles.end())
                {
                    return;
                }
                if (found->second.terminal_) {
                    return;
                }
                found->second.terminal_ = true;
                found->second.failureMessage_ =
                    std::move(message);
                for (auto const outputIndex :
                     found->second
                         .dependentOutputs_)
                {
                    auto pending =
                        pendingRelationOutputs
                            .find(outputIndex);
                    if (pending ==
                        pendingRelationOutputs.end())
                    {
                        continue;
                    }
                    markRelationTargetUnavailableInOutput(
                        pending->second.ready_,
                        targetKey,
                        *found->second
                             .failureMessage_);
                    pending->second
                        .pendingTargetTiles_
                        .erase(targetKey);
                    if (pending->second
                            .pendingTargetTiles_
                            .empty())
                    {
                        ready.push_back(
                            std::move(
                                pending->second
                                    .ready_));
                        pendingRelationOutputs
                            .erase(pending);
                        ++readyOutputTiles;
                    }
                }
            }
            std::ranges::sort(
                ready,
                {},
                &ReadyOutput::outputIndex_);
            emitCompletedOutputs(
                std::move(ready));
            emitProgress(
                "RelationTargetUnavailable");
            finishIfComplete();
        }

        void collectRelationTarget(
            MapTileKey const& targetKey,
            TileFeatureLayer::Ptr layer)
        {
            if (!layer ||
                layer->id() != targetKey)
            {
                fail(simfil::Error{
                    simfil::Error::InternalError,
                    fmt::format(
                        "Relation target request returned the wrong tile for {}.",
                        targetKey.toString()),
                });
                return;
            }

            std::vector<ReadyOutput> ready;
            std::optional<simfil::Error> error;
            {
                std::lock_guard lock(mutex);
                if (terminal ||
                    request->isCancelled())
                {
                    return;
                }
                auto found =
                    relationTargetTiles.find(
                        targetKey);
                if (found ==
                    relationTargetTiles.end())
                {
                    error = simfil::Error{
                        simfil::Error::InternalError,
                        "Unplanned relation target tile was delivered.",
                    };
                }
                else if (found->second.terminal_) {
                    // A request-level failure may race a late child delivery.
                    // The target already has a terminal contribution, so the
                    // late value must not fail the entire filter generation.
                    return;
                }
                else {
                    found->second.terminal_ = true;
                    found->second.layer_ = layer;
                    memory->relationTargetModels.add(
                        layer->memoryUsage().total().allocatedBytes);
                    for (auto const outputIndex :
                         found->second
                             .dependentOutputs_)
                    {
                        auto pending =
                            pendingRelationOutputs
                                .find(outputIndex);
                        if (pending ==
                            pendingRelationOutputs.end())
                        {
                            continue;
                        }
                        auto resolved =
                            resolveRelationTargetInOutput(
                                pending->second.ready_,
                                targetKey,
                                *layer);
                        if (!resolved) {
                            error =
                                resolved.error();
                            break;
                        }
                        pending->second
                            .pendingTargetTiles_
                            .erase(targetKey);
                        if (pending->second
                                .pendingTargetTiles_
                                .empty())
                        {
                            ready.push_back(
                                std::move(
                                    pending->second
                                        .ready_));
                            pendingRelationOutputs
                                .erase(pending);
                            ++readyOutputTiles;
                        }
                    }
                }
            }
            if (error) {
                fail(*error);
                return;
            }
            std::ranges::sort(
                ready,
                {},
                &ReadyOutput::outputIndex_);
            emitCompletedOutputs(
                std::move(ready));
            emitProgress(
                "RelationTargetResolved");
            finishIfComplete();
        }

        tl::expected<size_t, simfil::Error>
        finalizeOutput(ReadyOutput ready)
        {
            std::vector<TileSubsetDependency> dependencies;
            std::vector<FeatureLayerPointGroupMember> members;
            std::vector<FeatureLayerRelationDescriptor>
                relationDescriptors;
            std::map<
                std::tuple<
                    std::string,
                    std::string,
                    Scope,
                    std::string>,
                FilterIssue>
                issues;
            std::map<std::string, simfil::Trace> traces;
            simfil::Diagnostics diagnostics;

            for (auto&& issue : ready.issues_) {
                auto key = std::make_tuple(
                    issue.channelId_,
                    issue.expression_,
                    issue.scope_,
                    issue.message_);
                auto [found, inserted] =
                    issues.emplace(key, issue);
                if (!inserted) {
                    auto const remaining =
                        std::numeric_limits<uint64_t>::max() -
                        found->second.occurrenceCount_;
                    found->second.occurrenceCount_ +=
                        std::min(
                            remaining,
                            issue.occurrenceCount_);
                }
            }

            for (auto& contribution :
                 ready.contributions_)
            {
                dependencies.push_back(
                    std::move(contribution.dependency_));
                members.insert(
                    members.end(),
                    std::make_move_iterator(
                        contribution
                            .pointGroupMembers_.begin()),
                    std::make_move_iterator(
                        contribution
                            .pointGroupMembers_.end()));
                relationDescriptors.insert(
                    relationDescriptors.end(),
                    std::make_move_iterator(
                        contribution
                            .relationDescriptors_.begin()),
                    std::make_move_iterator(
                        contribution
                            .relationDescriptors_.end()));
                for (auto&& issue : contribution.issues_) {
                    auto key = std::make_tuple(
                        issue.channelId_,
                        issue.expression_,
                        issue.scope_,
                        issue.message_);
                    auto [found, inserted] =
                        issues.emplace(key, issue);
                    if (!inserted) {
                        auto const remaining =
                            std::numeric_limits<uint64_t>::max() -
                            found->second.occurrenceCount_;
                        found->second.occurrenceCount_ +=
                            std::min(
                                remaining,
                                issue.occurrenceCount_);
                    }
                }
                mergeFilterTraces(
                    traces,
                    std::move(contribution.traces_));
                if (!contribution.diagnostics_.exprIndex_.empty() ||
                    !contribution.diagnostics_.fieldData_.empty() ||
                    !contribution.diagnostics_.comparisonData_.empty())
                {
                    diagnostics.append(
                        contribution.diagnostics_);
                }
            }
            for (auto& [_, contribution] :
                 ready.dynamicContributions_)
            {
                dependencies.push_back(
                    std::move(contribution.dependency_));
                for (auto&& issue :
                     contribution.issues_)
                {
                    auto key = std::make_tuple(
                        issue.channelId_,
                        issue.expression_,
                        issue.scope_,
                        issue.message_);
                    auto [found, inserted] =
                        issues.emplace(key, issue);
                    if (!inserted) {
                        auto const remaining =
                            std::numeric_limits<uint64_t>::max() -
                            found->second.occurrenceCount_;
                        found->second.occurrenceCount_ +=
                            std::min(
                                remaining,
                                issue.occurrenceCount_);
                    }
                }
                mergeFilterTraces(
                    traces,
                    std::move(contribution.traces_));
            }

            if (hasStoredRelations) {
                std::vector<
                    FeatureLayerRelationDescriptor>
                    resolvedDescriptors;
                resolvedDescriptors.reserve(
                    relationDescriptors.size());
                for (auto&& descriptor :
                     relationDescriptors)
                {
                    if (descriptor.target_) {
                        resolvedDescriptors.push_back(
                            std::move(descriptor));
                        continue;
                    }
                    if (std::ranges::any_of(
                            descriptor
                                .targetCandidates_,
                            [](auto const& candidate) {
                                return !candidate
                                    .resolved_;
                            }))
                    {
                        return tl::unexpected(
                            simfil::Error{
                                simfil::Error::InternalError,
                                "Relation target reached finalization with unresolved locate candidates.",
                            });
                    }

                    std::map<
                        std::pair<
                            MapTileKey,
                            std::string>,
                        model_ptr<Feature>>
                        uniqueMatches;
                    for (auto const& match :
                         descriptor.targetMatches_)
                    {
                        uniqueMatches.emplace(
                            std::make_pair(
                                MapTileKey(
                                    match->model()),
                                match->id()
                                    ->toString()),
                            match);
                    }
                    if (uniqueMatches.size() == 1) {
                        auto const& [identity, match] =
                            *uniqueMatches.begin();
                        descriptor.target_ = match;
                        descriptor.targetTileKey_ =
                            identity.first;
                        descriptor.targetTypeId_ =
                            std::string(
                                match->typeId());
                        descriptor.targetFeatureId_ =
                            castToKeyValue(
                                match->id()
                                    ->keyValuePairs());
                        resolvedDescriptors.push_back(
                            std::move(descriptor));
                        continue;
                    }

                    auto const channelId =
                        descriptor.channelIndex_ <
                                request->filter_
                                    .channels_.size()
                        ? request->filter_
                              .channels_[
                                  descriptor
                                      .channelIndex_]
                              .channelId_
                        : std::string{};
                    auto const targetIdentity =
                        descriptor.relation_ &&
                            descriptor.relation_
                                ->target()
                        ? descriptor.relation_
                              ->target()
                              ->toString()
                        : formatFeatureIdString(
                              descriptor
                                  .targetTypeId_,
                              descriptor
                                  .targetFeatureId_);
                    auto message =
                        uniqueMatches.empty()
                        ? fmt::format(
                              "Located relation target '{}' was not found in its candidate tiles.",
                              targetIdentity)
                        : fmt::format(
                              "Relation target '{}' resolved ambiguously to {} features.",
                              targetIdentity,
                              uniqueMatches.size());
                    auto issue = FilterIssue{
                        channelId,
                        "<relation-target>",
                        Scope::Relation,
                        std::move(message),
                        1,
                    };
                    auto key = std::make_tuple(
                        issue.channelId_,
                        issue.expression_,
                        issue.scope_,
                        issue.message_);
                    auto [found, inserted] =
                        issues.emplace(
                            std::move(key),
                            issue);
                    if (!inserted) {
                        ++found->second
                              .occurrenceCount_;
                    }
                }
                relationDescriptors =
                    std::move(
                        resolvedDescriptors);
            }

            ready.layer_->setDependencies(
                std::move(dependencies));
            if (hasPointGroups) {
                auto const startedAt =
                    std::chrono::steady_clock::now();
                auto completion =
                    completeFeatureLayerPointGroups(
                        *ready.layer_,
                        request->filter_,
                        members,
                        [request = this->request] {
                            return request
                                ->isCancelled();
                        });
                if (!completion) {
                    return tl::unexpected(
                        completion.error());
                }
                if (request->isCancelled()) {
                    return size_t{0};
                }
                for (auto&& issue :
                     completion->issues_)
                {
                    auto key = std::make_tuple(
                        issue.channelId_,
                        issue.expression_,
                        issue.scope_,
                        issue.message_);
                    auto [found, inserted] =
                        issues.emplace(key, issue);
                    if (!inserted) {
                        auto const remaining =
                            std::numeric_limits<uint64_t>::max() -
                            found->second.occurrenceCount_;
                        found->second.occurrenceCount_ +=
                            std::min(
                                remaining,
                                issue.occurrenceCount_);
                    }
                }
                mergeFilterTraces(
                    traces,
                    std::move(completion->traces_));
                ready.layer_->setInfo(
                    "Filter/Process-Groups#ms",
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() -
                        startedAt)
                        .count());
            }
            if (hasStoredRelations) {
                std::vector<MapTileKey>
                    requestedOutputKeys;
                requestedOutputKeys.reserve(
                    outputs.size());
                for (auto const& output : outputs) {
                    requestedOutputKeys.emplace_back(
                        LayerType::Features,
                        request->mapId_,
                        request->layerId_,
                        output.tileId_);
                }
                auto const startedAt =
                    std::chrono::steady_clock::now();
                auto completion =
                    completeFeatureLayerRelations(
                        *ready.layer_,
                        request->filter_,
                        relationDescriptors,
                        requestedOutputKeys,
                        request->exactRoots_,
                        [request = this->request] {
                            return request
                                ->isCancelled();
                        });
                if (!completion) {
                    return tl::unexpected(
                        completion.error());
                }
                if (request->isCancelled()) {
                    return size_t{0};
                }
                for (auto&& issue :
                     completion->issues_)
                {
                    auto key = std::make_tuple(
                        issue.channelId_,
                        issue.expression_,
                        issue.scope_,
                        issue.message_);
                    auto [found, inserted] =
                        issues.emplace(key, issue);
                    if (!inserted) {
                        auto const remaining =
                            std::numeric_limits<uint64_t>::max() -
                            found->second.occurrenceCount_;
                        found->second.occurrenceCount_ +=
                            std::min(
                                remaining,
                                issue.occurrenceCount_);
                    }
                }
                mergeFilterTraces(
                    traces,
                    std::move(completion->traces_));
                auto const priorMilliseconds =
                    ready.layer_->info().value(
                        "Filter/Process-Relations#ms",
                        0.0);
                ready.layer_->setInfo(
                    "Filter/Process-Relations#ms",
                    priorMilliseconds +
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            startedAt)
                            .count());
            }

            for (auto&& [key, issue] : issues) {
                ready.layer_->addIssue(
                    std::move(issue));
            }
            ready.layer_->setTraces(
                std::move(traces));
            ready.layer_->setDiagnostics(
                diagnostics);

            size_t entryCount = 0;
            ready.layer_->forEachChannel(
                [&](model_ptr<TileSubsetChannel> const&
                        channel)
                {
                    entryCount += channel->entryCount();
                    return true;
                });
            if (request->isCancelled()) {
                return size_t{0};
            }
            auto const finalLayerBytes =
                ready.layer_->memoryUsage().total().allocatedBytes;
            if (finalLayerBytes > ready.layerBytes_) {
                memory->outputSubsetModels.add(finalLayerBytes - ready.layerBytes_);
            }
            else {
                memory->outputSubsetModels.subtract(ready.layerBytes_ - finalLayerBytes);
            }
            request->notifyResult(
                std::move(ready.layer_));
            memory->outputSubsetModels.subtract(finalLayerBytes);
            return entryCount;
        }

        bool emitCompletedOutputs(
            std::vector<ReadyOutput> ready)
        {
            for (auto&& output : ready) {
                auto entryCount =
                    finalizeOutput(std::move(output));
                if (!entryCount) {
                    fail(entryCount.error());
                    return false;
                }
                bool stopped = false;
                {
                    std::lock_guard lock(mutex);
                    if (terminal) {
                        stopped = true;
                    }
                    else {
                        ++emittedOutputTiles;
                        entriesEmitted +=
                            *entryCount;
                    }
                }
                if (stopped) {
                    return false;
                }
                emitProgress(
                    "OutputTileEmitted");
            }
            return true;
        }

        void evaluate(
            size_t sourceIndex,
            uint64_t sourceBytes,
            TileFeatureLayer::Ptr source)
        {
            bool evaluationJobPending = true;
            uint64_t trackedOutputModelBytes = 0;
            uint64_t trackedTemporaryBytes = 0;
            bool outputModelTransferred = false;
            auto releaseUntransferredOutputModel = [&]() {
                if (!outputModelTransferred && trackedOutputModelBytes) {
                    memory->outputSubsetModels.subtract(trackedOutputModelBytes);
                    trackedOutputModelBytes = 0;
                }
            };
            auto releaseTemporary = [&]() {
                if (trackedTemporaryBytes) {
                    memory->evaluationTemporaries.subtract(trackedTemporaryBytes);
                    trackedTemporaryBytes = 0;
                }
            };
            auto replaceTemporary = [&](uint64_t replacement) {
                if (replacement >= trackedTemporaryBytes) {
                    memory->evaluationTemporaries.add(
                        replacement - trackedTemporaryBytes);
                }
                else {
                    memory->evaluationTemporaries.subtract(
                        trackedTemporaryBytes - replacement);
                }
                trackedTemporaryBytes = replacement;
            };
            auto finishEvaluationJob = [&]() {
                if (!evaluationJobPending) {
                    return;
                }
                {
                    std::lock_guard lock(mutex);
                    if (pendingEvaluationJobs > 0) {
                        --pendingEvaluationJobs;
                    }
                }
                memory->sourceTileModels.subtract(sourceBytes);
                evaluationJobPending = false;
            };
            try {
                if (!source || request->isCancelled()) {
                    finishEvaluationJob();
                    finishIfComplete();
                    return;
                }

                auto const filterStartedAt =
                    std::chrono::steady_clock::now();
                auto sourceResult =
                    filterFeatureLayerSource(
                        *source,
                        request->filter_,
                        outputIndexByTile.contains(
                            source->tileId()),
                        request->exactRoots_,
                        [request = this->request] {
                            return request
                                ->isCancelled();
                        });
                if (!sourceResult) {
                    finishEvaluationJob();
                    fail(sourceResult.error());
                    return;
                }
                replaceTemporary(sourceResultAuxiliaryBytes(*sourceResult));
                trackedOutputModelBytes = sourceResult->layer_
                    ? sourceResult->layer_->memoryUsage().total().allocatedBytes
                    : uint64_t{0};
                if (trackedOutputModelBytes) {
                    memory->outputSubsetModels.add(trackedOutputModelBytes);
                }
                if (sourceResult->layer_) {
                    sourceResult->layer_->setInfo(
                        "Filter/Process-Entries#ms",
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            filterStartedAt)
                            .count());
                }
                if (request->isCancelled()) {
                    releaseTemporary();
                    releaseUntransferredOutputModel();
                    finishEvaluationJob();
                    finishIfComplete();
                    return;
                }
                auto const relationStartedAt =
                    std::chrono::steady_clock::now();
                auto locatedRelations =
                    locateRelationTargets(
                        *source,
                        *sourceResult);
                if (!locatedRelations) {
                    releaseTemporary();
                    releaseUntransferredOutputModel();
                    finishEvaluationJob();
                    fail(locatedRelations.error());
                    return;
                }
                // Relation discovery appends candidates to the source result;
                // refresh the sampled capacity before ownership is committed.
                replaceTemporary(sourceResultAuxiliaryBytes(*sourceResult));
                if (sourceResult->layer_ &&
                    !sourceResult
                         ->relationDescriptors_
                         .empty())
                {
                    sourceResult->layer_->setInfo(
                        "Filter/Process-Relations#ms",
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            relationStartedAt)
                            .count());
                }

                auto ready = commitSource(
                    sourceIndex,
                    *source,
                    trackedOutputModelBytes,
                    std::move(*sourceResult));
                if (!ready) {
                    releaseTemporary();
                    releaseUntransferredOutputModel();
                    finishEvaluationJob();
                    fail(ready.error());
                    return;
                }
                outputModelTransferred = trackedOutputModelBytes != 0;
                releaseTemporary();
                emitProgress("SourceTileEvaluated");

                auto prepared =
                    prepareRelationOutputs(
                        std::move(*ready));
                if (!prepared) {
                    finishEvaluationJob();
                    fail(prepared.error());
                    return;
                }
                for (auto const& targetKey :
                     prepared
                         ->targetsToSchedule_)
                {
                    scheduleRelationTarget(
                        targetKey);
                }
                if (!emitCompletedOutputs(
                        std::move(
                            prepared->ready_)))
                {
                    finishEvaluationJob();
                    return;
                }
                finishEvaluationJob();
                finishIfComplete();
            }
            catch (std::exception const& exception) {
                releaseTemporary();
                releaseUntransferredOutputModel();
                finishEvaluationJob();
                auto const sourceTileId =
                    source ? source->tileId() : TileId{};
                fail(simfil::Error{
                    simfil::Error::InternalError,
                    fmt::format(
                        "Filter evaluation failed for {}::{} tile {:x}: {}",
                        request->mapId_,
                        request->layerId_,
                        sourceTileId.value(),
                        exception.what())});
            }
        }

        void childFinished(RequestStatus status)
        {
            if (status != RequestStatus::Success) {
                {
                    std::lock_guard lock(mutex);
                    terminal = true;
                }
                abortChildRequests();
                request->setStatus(status);
                return;
            }
            {
                std::lock_guard lock(mutex);
                childRequestDone = true;
            }
            finishIfComplete();
        }
    };

    auto state = std::make_shared<FilterExecutionState>();
    state->impl = impl_.get();
    state->service = this;
    state->request = request;
    state->sourceInfo = *sourceInfo;
    state->sourceDataSource =
        std::move(sourceDataSource);
    state->clientHeaders = clientHeaders;
    state->hasPointGroups = hasPointGroups;
    state->hasStoredRelations = hasStoredRelations;
    state->outputLevel = request->tileIds_.front().level();
    state->memory = std::make_shared<FilterMemoryTracker>();
    state->memory->mapId = request->mapId_;
    state->memory->layerId = request->layerId_;
    state->memory->filterId = request->filter_.filterId_;
    state->memory->generation = request->filter_.generation_;
    state->memory->requestedTiles = request->tileIds_.size();
    state->configure(
        request->tileIds_,
        tileIdsToProcess);
    state->memory->sampleOrchestration = [weakState = std::weak_ptr<FilterExecutionState>{state}] {
        auto active = weakState.lock();
        if (!active) {
            return uint64_t{0};
        }
        std::lock_guard lock(active->mutex);
        return active->orchestrationBytesLocked();
    };
    {
        std::lock_guard lock(impl_->jobsMutex_);
        impl_->filterMemoryTrackers_.push_back(state->memory);
    }

    childRequest->onFeatureLayer(
        [state](TileFeatureLayer::Ptr layer) {
            state->collect(std::move(layer));
        });
    childRequest->onDone_ =
        [state](RequestStatus status) {
            state->childFinished(status);
        };

    request->notifyProgress(
        state->progress("Open"));

    return this->request(
        std::vector<LayerTilesRequest::Ptr>{childRequest},
        clientHeaders);
}

AttachmentResult Service::attachment(
    AttachmentRequest const& request,
    std::optional<AuthHeaders> const& clientHeaders)
{
    std::mutex resultMutex;
    std::condition_variable resultAvailable;
    std::optional<AttachmentResult> completed;
    requestAttachment(
        request,
        [&](AttachmentResult result) {
            {
                std::lock_guard lock(resultMutex);
                completed = std::move(result);
            }
            resultAvailable.notify_one();
        },
        clientHeaders);
    std::unique_lock lock(resultMutex);
    resultAvailable.wait(
        lock,
        [&] { return completed.has_value(); });
    auto result = std::move(*completed);
    return result;
}

void Service::requestAttachment(
    AttachmentRequest request,
    std::function<void(AttachmentResult)> callback,
    std::optional<AuthHeaders> const& clientHeaders)
{
    if (!callback) {
        return;
    }
    if (request.tileKey_.layer_ != LayerType::Features ||
        request.name_.empty())
    {
        callback({});
        return;
    }

    auto context = resolveLayerRequest(
        request.tileKey_.mapId_,
        request.tileKey_.layerId_,
        clientHeaders,
        request.sourceId_);
    if (context.status_ != RequestStatus::Success ||
        context.layerType_ != LayerType::Features)
    {
        callback(AttachmentResult{
            .status_ = context.status_});
        return;
    }

    DataSource::Ptr selectedDataSource;
    {
        std::shared_lock lock(impl_->dataSourcesMutex_);
        for (auto const& [dataSource, info] :
             impl_->dataSourceInfo_)
        {
            if (info.isAddOn_ ||
                info.mapId_ != request.tileKey_.mapId_ ||
                !info.layers_.contains(
                    request.tileKey_.layerId_))
            {
                continue;
            }
            if (request.sourceId_) {
                auto sourceId =
                    impl_->dataSourceSourceIds_.find(
                        dataSource);
                if (sourceId ==
                        impl_->dataSourceSourceIds_.end() ||
                    sourceId->second != *request.sourceId_)
                {
                    continue;
                }
            }
            if (clientHeaders &&
                !dataSource->isDataSourceAuthorized(
                    *clientHeaders))
            {
                continue;
            }
            selectedDataSource = dataSource;
            break;
        }
    }
    if (!selectedDataSource) {
        callback({});
        return;
    }

    struct AttachmentState
    {
        AttachmentRequest request;
        DataSource::Ptr dataSource;
        std::function<void(AttachmentResult)> callback;
        std::mutex mutex;
        std::optional<AttachmentResponse> response;
        std::atomic_bool completed = false;

        void consumeTile(TileFeatureLayer::Ptr const& tile)
        {
            if (!tile || tile->error()) {
                return;
            }
            auto const& name =
                tile->glbAttachmentName();
            if (!name ||
                *name != request.name_)
            {
                return;
            }

            auto produced =
                dataSource->attachment(request);
            if (!produced) {
                return;
            }
            if (produced->name_ != request.name_) {
                log().warn(
                    "Datasource returned attachment '{}' "
                    "for requested attachment '{}'.",
                    produced->name_,
                    request.name_);
                return;
            }
            if (!produced->bytes_) {
                log().warn(
                    "Datasource returned attachment '{}' "
                    "without a byte payload.",
                    request.name_);
                return;
            }
            if (produced->mimeType_.empty()) {
                produced->mimeType_ =
                    "application/octet-stream";
            }
            std::lock_guard lock(mutex);
            response = std::move(produced);
        }

        void finish(RequestStatus status)
        {
            if (completed.exchange(true)) {
                return;
            }
            AttachmentResult result{
                .status_ = status};
            {
                std::lock_guard lock(mutex);
                result.response_ =
                    std::move(response);
            }
            callback(std::move(result));
        }
    };

    auto state =
        std::make_shared<AttachmentState>();
    state->request = std::move(request);
    state->dataSource =
        std::move(selectedDataSource);
    state->callback = std::move(callback);
    auto tileRequest =
        std::make_shared<LayerTilesRequest>(
            state->request.tileKey_.mapId_,
            state->request.tileKey_.layerId_,
            std::vector<TileId>{
                state->request.tileKey_.tileId_});
    tileRequest->sourceId_ =
        state->request.sourceId_;
    tileRequest->onFeatureLayer(
        [state](TileFeatureLayer::Ptr tile) {
            state->consumeTile(tile);
        });
    tileRequest->onDone_ =
        [state](RequestStatus status) {
            state->finish(status);
        };
    this->request(
        std::vector<LayerTilesRequest::Ptr>{
            tileRequest},
        clientHeaders);
}

bool Service::request(
    std::vector<FeatureLayerFilterTilesRequest::Ptr> const& requests,
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
    struct CandidateGroup
    {
        std::string sourceId_;
        MapTileKey tileKey_;
        std::vector<LocateCandidate> candidates_;
    };

    std::vector<CandidateGroup> candidateGroups;
    std::map<
        std::pair<std::string, MapTileKey>,
        size_t>
        groupIndex;
    {
        std::shared_lock lock(impl_->dataSourcesMutex_);
        for (auto const& [dataSource, info] :
             impl_->dataSourceInfo_)
        {
            if (info.mapId_ != req.mapId_ ||
                info.isAddOn_)
            {
                continue;
            }
            auto sourceId =
                impl_->dataSourceSourceIds_.find(
                    dataSource);
            auto const resolvedSourceId =
                sourceId !=
                    impl_->dataSourceSourceIds_.end()
                ? sourceId->second
                : std::string{};
            auto appendCandidates =
                [&](LocateRequest const&
                        resolvedRequest)
                {
                    for (auto&& candidate :
                         dataSource->locate(
                             resolvedRequest))
                    {
                        if (candidate.tileKey_.layer_ !=
                                LayerType::Features ||
                            candidate.tileKey_.mapId_ !=
                                req.mapId_ ||
                            !info.getLayer(
                                candidate.tileKey_
                                    .layerId_))
                        {
                            log().warn(
                                "Datasource returned an invalid locate candidate for {}.",
                                candidate.tileKey_
                                    .toString());
                            continue;
                        }
                        auto key = std::make_pair(
                            resolvedSourceId,
                            candidate.tileKey_);
                        auto [found, inserted] =
                            groupIndex.emplace(
                                key,
                                candidateGroups
                                    .size());
                        if (inserted) {
                            candidateGroups.push_back(
                                CandidateGroup{
                                    resolvedSourceId,
                                    candidate
                                        .tileKey_,
                                    {}});
                        }
                        candidateGroups[
                            found->second]
                            .candidates_
                            .push_back(
                                std::move(
                                    candidate));
                    }
                };
            if (!req.canonicalFeatureId_) {
                appendCandidates(req);
                continue;
            }
            for (auto const& [_, layerInfo] :
                 info.layers_)
            {
                if (!layerInfo ||
                    layerInfo->type_ !=
                        LayerType::Features)
                {
                    continue;
                }
                ParsedFeatureId parsed;
                if (!parseFeatureIdString(
                        *req.canonicalFeatureId_,
                        *layerInfo,
                        parsed))
                {
                    continue;
                }
                appendCandidates(LocateRequest{
                    req.mapId_,
                    std::move(parsed.typeId_),
                    std::move(
                        parsed.keyValuePairs_)});
            }
        }
    }

    struct LocateState
    {
        std::mutex mutex_;
        std::condition_variable cv_;
        size_t pending_ = 0;
        std::vector<LocateResponse> results_;
        std::set<std::string> seen_;
    };
    auto state = std::make_shared<LocateState>();
    state->pending_ = candidateGroups.size();

    for (auto& group : candidateGroups) {
        auto child =
            std::make_shared<LayerTilesRequest>(
                group.tileKey_.mapId_,
                group.tileKey_.layerId_,
                std::vector<TileId>{
                    group.tileKey_.tileId_});
        if (!group.sourceId_.empty()) {
            child->sourceId_ =
                group.sourceId_;
        }
        child->onFeatureLayer(
            [state,
             original = req,
             group = std::move(group)](
                TileFeatureLayer::Ptr tile)
            {
                if (!tile ||
                    tile->id() !=
                        group.tileKey_)
                {
                    return;
                }
                std::lock_guard lock(
                    state->mutex_);
                for (auto const& candidate :
                     group.candidates_)
                {
                    auto selected =
                        resolveLocateCandidate(
                            candidate,
                            *tile);
                    if (!selected) {
                        log().warn(
                            "Could not evaluate locate selector in {}: {}",
                            tile->id().toString(),
                            selected.error().message);
                        continue;
                    }
                    for (auto const& feature :
                         *selected)
                    {
                        LocateResponse response(
                            original);
                        response.tileKey_ =
                            tile->id();
                        response.typeId_ =
                            std::string(
                                feature->typeId());
                        response.featureId_ =
                            castToKeyValue(
                                feature->id()
                                    ->keyValuePairs());
                        response
                            .resolvedCanonicalFeatureId_ =
                            feature->id()
                                ->toString();
                        auto serialized =
                            response.serialize()
                                .dump();
                        if (state->seen_.insert(
                                serialized)
                                .second)
                        {
                            state->results_
                                .push_back(
                                    std::move(
                                        response));
                        }
                    }
                }
            });
        child->onDone_ =
            [state](RequestStatus) {
                std::lock_guard lock(
                    state->mutex_);
                if (state->pending_ > 0) {
                    --state->pending_;
                }
                state->cv_.notify_all();
            };
        (void) this->request(
            std::vector<
                LayerTilesRequest::Ptr>{
                child});
    }

    std::unique_lock lock(state->mutex_);
    state->cv_.wait(
        lock,
        [&] {
            return state->pending_ == 0;
        });
    auto results =
        std::move(state->results_);
    std::ranges::sort(
        results,
        [](auto const& left, auto const& right) {
            return std::tie(
                       left.tileKey_,
                       left.resolvedCanonicalFeatureId_) <
                std::tie(
                       right.tileKey_,
                       right.resolvedCanonicalFeatureId_);
        });
    return results;
}

void Service::abort(const LayerTilesRequest::Ptr& r)
{
    impl_->abortRequest(r);
}

void Service::abort(const FeatureLayerFilterTilesRequest::Ptr& r)
{
    if (!r || r->isDone()) {
        return;
    }
    r->cancel();
    impl_->abortFilterEvalJobs(r);
    std::vector<LayerTilesRequest::Ptr> childRequests;
    {
        std::lock_guard lock(r->childRequestsMutex_);
        childRequests = r->childRequests_;
        r->childRequests_.clear();
    }
    for (auto const& child : childRequests) {
        if (child && !child->isDone()) {
            impl_->abortRequest(child);
        }
    }
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
    std::optional<AuthHeaders> const& clientHeaders,
    std::optional<std::string> const& sourceId) const
{
    LayerRequestContext result;

    std::shared_lock lock(impl_->dataSourcesMutex_);
    bool layerExists = false;
    bool unauthorized = false;
    bool foundAuthorizedLayer = false;
    for (auto const& [ds, info] : impl_->dataSourceInfo_) {
        if (info.isAddOn_) {
            continue;
        }
        if (sourceId) {
            auto id = impl_->dataSourceSourceIds_.find(ds);
            if (id == impl_->dataSourceSourceIds_.end() ||
                id->second != *sourceId)
            {
                continue;
            }
        }
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
    size_t filterEvalWorkers = 0;
    size_t filterEvalConcurrencyLimit = 0;
    size_t queuedFilterEvalJobs = 0;
    size_t runningFilterEvalJobs = 0;
    size_t constructionFailures = 0;
    {
        std::unique_lock lock(impl_->jobsMutex_);
        activeRequests = impl_->requests_.size();
        filterEvalWorkers =
            impl_->filterEvalWorkers_.size();
        filterEvalConcurrencyLimit =
            impl_->filterEvalConcurrentLimitLocked();
        queuedFilterEvalJobs =
            impl_->filterEvalJobs_.size();
        runningFilterEvalJobs =
            impl_->runningFilterEvalJobs_;
    }
    {
        std::shared_lock lock(impl_->dataSourcesMutex_);
        constructionFailures = impl_->dataSourceConstructionFailed_;
    }
    auto configStats = DataSourceConfigService::get().getDataSourceConfigStats();
    auto result = nlohmann::json{
        {"datasources", datasources},
        {"active-requests", activeRequests},
        {"filter-evaluation", nlohmann::json{
            {"workers", filterEvalWorkers},
            {"concurrency-limit", filterEvalConcurrencyLimit},
            {"queued", queuedFilterEvalJobs},
            {"running", runningFilterEvalJobs}
        }},
        {"datasource-config", nlohmann::json{
            {"configured", configStats.configured},
            {"enabled", configStats.enabled},
            {"disabled", configStats.disabled},
            {"construction-failed", constructionFailures}
        }}
    };

    if (auto allocator = detail::allocatorMemoryStatistics(); !allocator.is_null()) {
        result["allocator"] = std::move(allocator);
    }

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

    LayerInfoResolveFun resolveLayerInfo;
    std::function<void(TileLayer::Ptr)> collectFeatureTreeStats;
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

        resolveLayerInfo = [layerInfoByMap](std::string_view mapId, std::string_view layerId)
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

        collectFeatureTreeStats = [&](auto&& parsedLayer) {
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
        };
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
            // A malformed cache value must not poison the framing state used
            // for the next independent value. Construct one reader per blob:
            // Reader::read deliberately retains an incomplete/error phase for
            // incremental streams.
            TileLayerStream::Reader tileReader(
                resolveLayerInfo,
                collectFeatureTreeStats,
                impl_->cache_);
            tileReader.read(blob);
            if (!tileReader.eos()) {
                ++parseErrors;
            }
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

nlohmann::json Service::getMemoryStatistics() const
{
    MemoryUsageBreakdown metadata;
    MemoryUsageBreakdown catalog;
    MemoryUsageBreakdown scheduler;
    MemoryUsageBreakdown telemetry;

    struct DataSourceMemoryCandidate
    {
        DataSource::Ptr dataSource;
        std::string sourceId;
        std::string mapId;
        std::string status;
    };
    std::vector<DataSourceMemoryCandidate> dataSourceCandidates;
    {
        std::shared_lock lock(impl_->dataSourcesMutex_);
        for (auto const& [dataSource, usage] : impl_->dataSourceInfoMemory_) {
            metadata.add("canonical-snapshots", usage);
        }
        for (auto const& [dataSource, usage] : impl_->workerInfoMemory_) {
            metadata.add("worker-snapshot-containers", usage);
        }
        for (auto const& [_, usage] : impl_->sourceCatalogInfoMemory_) {
            metadata.add("catalog-snapshots", usage);
        }

        catalog.add("entries", vectorMemoryUsage(impl_->sourceCatalog_));
        std::unordered_set<DataSource const*> catalogDataSources;
        for (auto const& entry : impl_->sourceCatalog_) {
            catalog.add("descriptors", dataSourceDescriptorMemoryUsage(entry.descriptor));
            catalog.add("status-messages", stringMemoryUsage(entry.statusMessage));
            auto status = std::string("initializing");
            if (entry.status == DataSourceCatalogStatus::Ready) {
                status = "ready";
            }
            else if (entry.status == DataSourceCatalogStatus::Failed) {
                status = "failed";
            }
            dataSourceCandidates.push_back({
                entry.dataSource,
                entry.descriptor.sourceId,
                entry.info ? entry.info->mapId_ : entry.descriptor.displayName,
                std::move(status),
            });
            if (entry.dataSource) {
                catalogDataSources.insert(entry.dataSource.get());
            }
        }
        for (auto const& [dataSource, info] : impl_->dataSourceInfo_) {
            if (catalogDataSources.contains(dataSource.get())) {
                continue;
            }
            auto sourceId = impl_->dataSourceSourceIds_.find(dataSource);
            dataSourceCandidates.push_back({
                dataSource,
                sourceId == impl_->dataSourceSourceIds_.end() ? std::string{} : sourceId->second,
                info.mapId_,
                "ready",
            });
        }

        catalog.add("source-id-index", {
            impl_->dataSourceSourceIds_.size() *
                sizeof(decltype(impl_->dataSourceSourceIds_)::value_type),
            impl_->dataSourceSourceIds_.size() *
                (sizeof(decltype(impl_->dataSourceSourceIds_)::value_type) + 3 * sizeof(void*)),
        });
        for (auto const& [_, sourceId] : impl_->dataSourceSourceIds_) {
            catalog.add("source-ids", stringMemoryUsage(sourceId));
        }
        catalog.add("config-status", stringMemoryUsage(impl_->sourceConfigStatus_));
        catalog.add("config-status-message", stringMemoryUsage(impl_->sourceConfigStatusMessage_));
        catalog.add("construction-threads", vectorMemoryUsage(impl_->dataSourceConstructionThreads_));
        catalog.add("config-datasource-handles", vectorMemoryUsage(impl_->dataSourcesFromConfig_));
        catalog.add("add-on-handles", {
            impl_->addOnDataSources_.size() * sizeof(DataSource::Ptr),
            impl_->addOnDataSources_.size() *
                (sizeof(DataSource::Ptr) + 2 * sizeof(void*)),
        });

        for (auto const& [_, workers] : impl_->dataSourceWorkers_) {
            metadata.add("worker-handles", vectorMemoryUsage(workers));
            metadata.add("worker-objects", {
                workers.size() * (sizeof(Worker) - sizeof(DataSourceInfo)),
                workers.size() * (sizeof(Worker) - sizeof(DataSourceInfo)),
            });
        }
    }

    auto accountMapTileKey = [](MemoryUsageBreakdown& target, MapTileKey const& key) {
        target.add("map-tile-key-strings", stringMemoryUsage(key.mapId_));
        target.add("map-tile-key-strings", stringMemoryUsage(key.layerId_));
    };
    auto accountLayerRequest = [&](LayerTilesRequest const& request) {
        scheduler.add("request-objects", {
            sizeof(LayerTilesRequest),
            sizeof(LayerTilesRequest),
        });
        scheduler.add("request-strings", stringMemoryUsage(request.mapId_));
        scheduler.add("request-strings", stringMemoryUsage(request.layerId_));
        if (request.sourceId_) {
            scheduler.add("request-strings", stringMemoryUsage(*request.sourceId_));
        }
        scheduler.add("request-tile-ids", vectorMemoryUsage(request.tileIds_));
        scheduler.add("priority-tile-index", {
            request.priorityTileIds_.size() * sizeof(TileId),
            request.priorityTileIds_.size() * (sizeof(TileId) + 3 * sizeof(void*)),
        });
        scheduler.add("resolved-tile-keys", vectorMemoryUsage(request.resolvedTileKeys_));
        for (auto const& key : request.resolvedTileKeys_) {
            accountMapTileKey(scheduler, key);
        }
        scheduler.add("not-started-tile-index", {
            request.tileKeysNotStarted_.size() * sizeof(MapTileKey),
            request.tileKeysNotStarted_.size() * (sizeof(MapTileKey) + 3 * sizeof(void*)),
        });
        for (auto const& key : request.tileKeysNotStarted_) {
            accountMapTileKey(scheduler, key);
        }
        scheduler.add("feature-id-restrictions", {
            request.featureIdsByTile_.size() *
                sizeof(decltype(request.featureIdsByTile_)::value_type),
            request.featureIdsByTile_.size() *
                (sizeof(decltype(request.featureIdsByTile_)::value_type) + 3 * sizeof(void*)),
        });
        for (auto const& [_, ids] : request.featureIdsByTile_) {
            scheduler.add("feature-id-vectors", stringVectorMemoryUsage(ids));
        }
    };

    std::vector<std::shared_ptr<FilterMemoryTracker>> filterTrackers;
    {
        std::unique_lock lock(impl_->jobsMutex_);
        scheduler.add("request-list", {
            impl_->requests_.size() * sizeof(LayerTilesRequest::Ptr),
            impl_->requests_.size() *
                (sizeof(LayerTilesRequest::Ptr) + 2 * sizeof(void*)),
        });
        std::unordered_set<LayerTilesRequest const*> measuredRequests;
        for (auto const& request : impl_->requests_) {
            if (request && measuredRequests.insert(request.get()).second) {
                accountLayerRequest(*request);
            }
        }
        scheduler.add("in-flight-job-index", {
            impl_->jobsInProgress_.size() *
                sizeof(decltype(impl_->jobsInProgress_)::value_type),
            impl_->jobsInProgress_.size() *
                (sizeof(decltype(impl_->jobsInProgress_)::value_type) + 3 * sizeof(void*)),
        });
        for (auto const& [key, job] : impl_->jobsInProgress_) {
            accountMapTileKey(scheduler, key);
            if (!job) {
                continue;
            }
            scheduler.add("in-flight-job-objects", {sizeof(Controller::Job), sizeof(Controller::Job)});
            accountMapTileKey(scheduler, job->tileKey);
            scheduler.add("job-waiters", vectorMemoryUsage(job->waitingRequests));
            for (auto const& request : job->waitingRequests) {
                if (request && measuredRequests.insert(request.get()).second) {
                    accountLayerRequest(*request);
                }
            }
        }
        scheduler.add("filter-job-list", {
            impl_->filterEvalJobs_.size() * sizeof(Controller::FilterEvalWork),
            impl_->filterEvalJobs_.size() *
                (sizeof(Controller::FilterEvalWork) + 2 * sizeof(void*)),
        });
        for (auto const& work : impl_->filterEvalJobs_) {
            scheduler.add("filter-job-map-ids", stringMemoryUsage(work.mapId));
        }
        scheduler.add("filter-worker-handles", vectorMemoryUsage(impl_->filterEvalWorkers_));
        scheduler.add("map-epochs", {
            impl_->mapEpochs_.size() * sizeof(decltype(impl_->mapEpochs_)::value_type),
            impl_->mapEpochs_.size() *
                (sizeof(decltype(impl_->mapEpochs_)::value_type) + 3 * sizeof(void*)),
        });
        for (auto const& [mapId, _] : impl_->mapEpochs_) {
            scheduler.add("map-epoch-ids", stringMemoryUsage(mapId));
        }

        auto tracker = impl_->filterMemoryTrackers_.begin();
        while (tracker != impl_->filterMemoryTrackers_.end()) {
            auto active = tracker->lock();
            if (!active) {
                tracker = impl_->filterMemoryTrackers_.erase(tracker);
                continue;
            }
            filterTrackers.push_back(std::move(active));
            ++tracker;
        }
        telemetry.add("filter-tracker-handles", vectorMemoryUsage(impl_->filterMemoryTrackers_));
    }

    // Sampling may briefly acquire a filter execution mutex, so never do it
    // while holding the scheduler mutex used to publish completed work.
    auto activeFilters = nlohmann::json::array();
    uint64_t activeFilterBytes = 0;
    for (auto const& tracker : filterTrackers) {
        auto snapshot = tracker->toJson();
        activeFilterBytes += snapshot.value("current-bytes", uint64_t{0});
        activeFilters.push_back(std::move(snapshot));
    }
    telemetry.add("active-filter-owned", {0, activeFilterBytes});

    auto dataSources = nlohmann::json::array();
    uint64_t measuredDataSourceBytes = 0;
    for (auto const& candidate : dataSourceCandidates) {
        auto row = nlohmann::json{
            {"source-id", candidate.sourceId},
            {"map-id", candidate.mapId},
            {"status", candidate.status},
            {"measurement", "unavailable"},
        };
        if (candidate.dataSource) {
            try {
                if (auto bytes = candidate.dataSource->estimatedRetainedMemoryBytes()) {
                    row["retained-bytes"] = *bytes;
                    row["measurement"] = "datasource-estimate";
                    measuredDataSourceBytes += *bytes;
                }
            }
            catch (std::exception const& error) {
                row["measurement"] = "error";
                row["error"] = error.what();
            }
            catch (...) {
                row["measurement"] = "error";
                row["error"] = "Datasource memory estimator threw a non-standard exception.";
            }
        }
        dataSources.push_back(std::move(row));
    }

    MemoryUsageBreakdown mapgetOwned;
    mapgetOwned.merge("metadata", metadata);
    mapgetOwned.merge("catalog", catalog);
    mapgetOwned.merge("scheduler", scheduler);
    mapgetOwned.merge("telemetry", telemetry);
    auto result = nlohmann::json{
        {"process", detail::processMemoryStatistics()},
        {"mapget", mapgetOwned.toJson()},
        {"active-filters", std::move(activeFilters)},
        {"datasources", std::move(dataSources)},
        {"datasource-measured-bytes", measuredDataSourceBytes},
        {"quality", {
            {"mapget", "capacity-lower-bound"},
            {"datasources", "cooperative-exclusive-estimate"},
            {"allocator-bookkeeping-included", false},
        }},
    };
    if (auto allocator = detail::allocatorMemoryStatistics(); !allocator.is_null()) {
        result["allocator"] = std::move(allocator);
    }
    auto const knownBytes =
        mapgetOwned.total().allocatedBytes + measuredDataSourceBytes;
    result["known-current-bytes"] = knownBytes;
    if (result["process"].contains("resident-bytes")) {
        auto const resident = result["process"]["resident-bytes"].get<uint64_t>();
        result["unattributed-resident-bytes"] =
            resident > knownBytes ? resident - knownBytes : uint64_t{0};
    }
    return result;
}

}  // namespace mapget
