#include "service-tiles.h"

#include "locate.h"
#include "mapget/log.h"
#include "service-impl.h"

namespace mapget::detail
{

TileLoadJob::TileLoadJob(
    ServiceScheduler& scheduler,
    std::shared_ptr<SourceConcurrency> source,
    std::shared_ptr<TileLoadState> state)
    : scheduler_(scheduler), source_(std::move(source)), state_(std::move(state))
{
}

void TileLoadJob::run() noexcept
{
    try {
        if (state_->cacheExpiredAt) {
            source_->source->dataSource->onCacheExpired(state_->tileKey, *state_->cacheExpiredAt);
        }

        scheduler_.notifyTileLoadState(*state_, TileLayer::LoadState::BackendFetching);
        auto notifyWaitingRequests = [this](TileLayer::LoadState state)
        {
            scheduler_.notifyTileLoadState(*state_, state);
        };
        auto layer = source_->source->dataSource->get(
            state_->tileKey,
            scheduler_.cache_,
            *source_->source->info,
            std::move(notifyWaitingRequests));
        if (!layer) {
            raise("DataSource::get() returned null.");
        }

        if (layer->layerInfo()->type_ == LayerType::Features) {
            loadAddOnTiles(
                std::static_pointer_cast<TileFeatureLayer>(layer),
                *source_->source,
                scheduler_.dataSources_,
                scheduler_.cache_);
        }

        if (!layer->ttl()) {
            auto ttl = source_->source->dataSource->ttl();
            if (!ttl) {
                ttl = scheduler_.defaultTtl_;
            }
            if (ttl) {
                layer->setTtl(ttl);
            }
        }

        scheduler_.completeTileJob(*state_, layer);
    }
    catch (std::exception const& error) {
        log().error("Could not load tile {}: {}", state_->tileKey.toString(), error.what());
        scheduler_.failTileJob(*state_);
    }
    catch (...) {
        log().error("Could not load tile {}: non-standard exception.", state_->tileKey.toString());
        scheduler_.failTileJob(*state_);
    }
}

void TileLoadJob::discard() noexcept
{
    // Tile jobs are materialized only when a worker accepts them, so they are
    // never left in the scheduler's discardable queue.
}

bool TileLoadJob::cancelled() const
{
    return false;
}

std::shared_ptr<SourceConcurrency> TileLoadJob::sourceAffinity() const
{
    return source_;
}

std::string_view TileLoadJob::mapId() const
{
    return state_->tileKey.mapId_;
}

ServiceJobKind TileLoadJob::kind() const
{
    return ServiceJobKind::TileLoad;
}

void loadAddOnTiles(
    TileFeatureLayer::Ptr const& baseTile,
    RegisteredDataSource const& baseSource,
    DataSourceRegistry const& dataSources,
    Cache::Ptr& cache)
{
    for (auto const& addOn : dataSources.addOnSources()) {
        if (addOn->info->mapId_ != baseTile->mapId()) {
            continue;
        }

        auto loaded = addOn->dataSource->get(baseTile->id(), cache, *addOn->info);
        if (!loaded) {
            log().warn("Add-on datasource returned null for {}.", baseTile->id().toString());
            continue;
        }
        if (loaded->error()) {
            log().warn(
                "Error while fetching add-on tile {}: {}",
                baseTile->id().toString(),
                *loaded->error());
            continue;
        }
        if (loaded->layerInfo()->type_ != LayerType::Features) {
            log().warn("Add-on tile {} is not a feature layer.", baseTile->id().toString());
            continue;
        }
        auto addOnTile = std::static_pointer_cast<TileFeatureLayer>(loaded);

        // Add-on strings must be cloned into a writable namespace shared by
        // both models; datasource-owned pools remain authoritative and frozen.
        auto combinedPoolId = baseTile->stringPoolId() + "|" + addOnTile->stringPoolId();
        baseTile->setStrings(cache->getStringPool(combinedPoolId));
        baseTile->setStringPoolId(combinedPoolId);

        TileFeatureLayer::CloneCache cloneCache;
        for (auto const& addOnFeature : *addOnTile) {
            std::vector<std::pair<std::string, KeyValuePairs>> targetIds = {{
                std::string(addOnFeature->id()->typeId()),
                castToKeyValue(addOnFeature->id()->keyValuePairs()),
            }};

            auto const indirect = !baseTile->layerInfo()->validFeatureId(
                targetIds.front().first,
                castToKeyValueView(targetIds.front().second),
                true);
            if (indirect) {
                auto candidates = baseSource.dataSource->locate(LocateRequest(
                    addOnTile->mapId(),
                    targetIds.front().first,
                    targetIds.front().second));
                if (candidates.empty()) {
                    log().warn(
                        "Could not locate indirect add-on feature ID {}.",
                        addOnFeature->id()->toString());
                    continue;
                }
                targetIds.clear();
                for (auto const& candidate : candidates) {
                    if (candidate.tileKey_ != baseTile->id()) {
                        continue;
                    }
                    auto selected = resolveLocateCandidate(candidate, *baseTile);
                    if (!selected) {
                        log().warn(
                            "Could not evaluate indirect add-on selector for {}: {}",
                            addOnFeature->id()->toString(),
                            selected.error().message);
                        continue;
                    }
                    for (auto const& feature : *selected) {
                        targetIds.emplace_back(
                            std::string(feature->typeId()),
                            castToKeyValue(feature->id()->keyValuePairs()));
                    }
                }
            }

            for (auto const& [typeId, idParts] : targetIds) {
                baseTile->clone(
                    cloneCache,
                    addOnTile,
                    *addOnFeature,
                    typeId,
                    castToKeyValueView(idParts));
            }
        }
    }
}

/** Owns one asynchronous attachment request until its source tile is terminal. */
class AttachmentRequestExecution
{
public:
    AttachmentRequest request;
    DataSource::Ptr dataSource;
    std::function<void(AttachmentResult)> callback;

    /** Capture a valid datasource attachment advertised by the source tile. */
    void consumeTile(TileFeatureLayer::Ptr const& tile)
    {
        if (!tile || tile->error() || tile->glbAttachmentName() != request.name_) {
            return;
        }
        auto produced = dataSource->attachment(request);
        if (!produced) {
            return;
        }
        if (produced->name_ != request.name_) {
            log().warn(
                "Datasource returned attachment '{}' for requested attachment '{}'.",
                produced->name_,
                request.name_);
            return;
        }
        if (!produced->bytes_) {
            log()
                .warn("Datasource returned attachment '{}' without a byte payload.", request.name_);
            return;
        }
        if (produced->mimeType_.empty()) {
            produced->mimeType_ = "application/octet-stream";
        }
        std::lock_guard lock(mutex_);
        response_ = std::move(produced);
    }

    /** Invoke the user callback exactly once with the terminal tile status. */
    void finish(RequestStatus status)
    {
        if (completed_.exchange(true)) {
            return;
        }
        AttachmentResult result{.status_ = status};
        {
            std::lock_guard lock(mutex_);
            result.response_ = std::move(response_);
        }
        callback(std::move(result));
    }

private:
    std::mutex mutex_;
    std::optional<AttachmentResponse> response_;
    std::atomic_bool completed_ = false;
};

}  // namespace mapget::detail

namespace mapget
{

namespace
{

/** Clone only explicitly requested features into a response-local tile model. */
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
    result->setGlbAttachmentName(source->glbAttachmentName());

    TileFeatureLayer::CloneCache cloneCache;
    for (auto const& canonicalId : featureIds) {
        auto feature = source->find(canonicalId);
        if (!feature) {
            continue;
        }
        auto featureId = feature->id();
        result
            ->clone(cloneCache, source, *feature, featureId->typeId(), featureId->keyValuePairs());
    }
    return result;
}

}  // namespace

LayerTilesRequest::LayerTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<TileId> tiles)
    : LayerTilesRequest(std::move(mapId), std::move(layerId), std::move(tiles), {})
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
    for (auto const& priorityTileId : priorityTileIds_) {
        if (!seenTileIds.contains(priorityTileId)) {
            raise("Priority tile IDs must be contained in the request tile IDs.");
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

    const auto isPriorityTile = [this](TileId const& tileId)
    {
        return priorityTileIds_.find(tileId) != priorityTileIds_.end();
    };
    const auto appendKey = [this](MapTileKey key)
    {
        if (tileKeysNotStarted_.insert(key).second) {
            resolvedTileKeys_.push_back(std::move(key));
        }
    };

    const auto appendTiles = [&](std::optional<bool> priorityFilter)
    {
        for (auto const& tileId : tileIds_) {
            if (priorityFilter && isPriorityTile(tileId) != *priorityFilter) {
                continue;
            }
            appendKey(MapTileKey(layerType, mapId_, layerId_, tileId));
        }
    };
    if (priorityTileIds_.empty()) {
        appendTiles(std::nullopt);
    }
    else {
        appendTiles(true);
        appendTiles(false);
    }

    status_ = resolvedTileKeys_.empty() ? RequestStatus::Success : RequestStatus::Open;
}

void LayerTilesRequest::notifyResult(TileLayer::Ptr r)
{
    if (isDone()) {
        return;
    }

    const auto type = r->layerInfo()->type_;
    switch (type) {
    case LayerType::Features:
        if (onFeatureLayer_) {
            auto featureLayer = std::static_pointer_cast<TileFeatureLayer>(r);
            if (auto restriction = featureIdsByTile_.find(featureLayer->tileId());
                restriction != featureIdsByTile_.end())
            {
                featureLayer = restrictFeatureLayerForResponse(featureLayer, restriction->second);
            }
            onFeatureLayer_(std::move(featureLayer));
        }
        break;
    case LayerType::SourceData:
        if (onSourceDataLayer_)
            onSourceDataLayer_(std::move(std::static_pointer_cast<TileSourceDataLayer>(r)));
        break;
    default:
        log().error(
            fmt::format("Unhandled layer type {}, no matching callback!", static_cast<int>(type)));
        break;
    }

    const auto resultCount = resultCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (resultCount == resolvedTileKeys_.size()) {
        setStatus(RequestStatus::Success);
    }
}

void LayerTilesRequest::notifyLoadState(MapTileKey const& key, TileLayer::LoadState state) const
{
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
        statusConditionVariable_.wait(doneLock, [this] { return isDone(); });
    }
}

nlohmann::json LayerTilesRequest::toJson()
{
    auto requestJson = nlohmann::json::object({{"mapId", mapId_}, {"layerId", layerId_}});
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
        for (auto const& [tileId, ids] : featureIdsByTile_) {
            featureIds.push_back({
                {"tileId", tileId.value()},
                {"ids", ids},
            });
        }
        requestJson["featureIds"] = std::move(featureIds);
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

bool Service::Impl::requestTiles(
    std::vector<LayerTilesRequest::Ptr> const& requests,
    std::optional<AuthHeaders> const& clientHeaders)
{
    auto allAvailable = true;
    for (auto const& request : requests) {
        if (!request) {
            raise("Attempt to request a null LayerTilesRequest.");
        }
        auto const context = resolveLayerRequest(
            request->mapId_,
            request->layerId_,
            clientHeaders,
            request->sourceId_);
        switch (context.status_) {
        case RequestStatus::NoDataSource:
            allAvailable = false;
            log().debug(
                "No data source can provide requested map and layer: {}::{}",
                request->mapId_,
                request->layerId_);
            request->setStatus(RequestStatus::NoDataSource);
            break;
        case RequestStatus::Unauthorized:
            allAvailable = false;
            log().debug(
                "Not authorized to access requested map and layer: {}::{}",
                request->mapId_,
                request->layerId_);
            request->setStatus(RequestStatus::Unauthorized);
            break;
        default:
            request->prepareResolvedLayer(context.layerType_);
            if (request->isDone()) {
                request->notifyStatus();
            }
            break;
        }
    }

    // Bundles are atomic: one rejected member aborts every otherwise-open
    // sibling rather than exposing a partially fulfilled logical request.
    for (auto const& request : requests) {
        if (!allAvailable) {
            if (request->getStatus() == RequestStatus::Open) {
                request->setStatus(RequestStatus::Aborted);
            }
        }
        else if (!request->isDone()) {
            scheduler_.enqueueRequest(request);
        }
    }
    return allAvailable;
}

void Service::Impl::requestAttachment(
    AttachmentRequest request,
    std::function<void(AttachmentResult)> callback,
    std::optional<AuthHeaders> const& clientHeaders)
{
    if (!callback) {
        return;
    }
    if (request.tileKey_.layer_ != LayerType::Features || request.name_.empty()) {
        callback({});
        return;
    }

    auto const context = resolveLayerRequest(
        request.tileKey_.mapId_,
        request.tileKey_.layerId_,
        clientHeaders,
        request.sourceId_);
    if (context.status_ != RequestStatus::Success || context.layerType_ != LayerType::Features) {
        callback(AttachmentResult{.status_ = context.status_});
        return;
    }

    detail::RegisteredDataSource::Ptr selectedSource;
    for (auto const& source : dataSources_.matchingPrimarySources(
             request.tileKey_.mapId_,
             request.tileKey_.layerId_,
             request.sourceId_))
    {
        if (!clientHeaders || source->dataSource->isDataSourceAuthorized(*clientHeaders)) {
            selectedSource = source;
            break;
        }
    }
    if (!selectedSource) {
        callback({});
        return;
    }

    auto execution = std::make_shared<detail::AttachmentRequestExecution>();
    execution->request = std::move(request);
    execution->dataSource = selectedSource->dataSource;
    execution->callback = std::move(callback);
    auto tileRequest = std::make_shared<LayerTilesRequest>(
        execution->request.tileKey_.mapId_,
        execution->request.tileKey_.layerId_,
        std::vector<TileId>{execution->request.tileKey_.tileId_});
    tileRequest->sourceId_ = execution->request.sourceId_;
    tileRequest
        ->onFeatureLayer([execution](TileFeatureLayer::Ptr tile) { execution->consumeTile(tile); });
    tileRequest->onDone_ = [execution](RequestStatus status)
    {
        execution->finish(status);
    };
    (void)requestTiles({tileRequest}, clientHeaders);
}

}  // namespace mapget
