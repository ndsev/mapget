#include "service-tiles.h"

#include "locate.h"
#include "mapget/log.h"
#include "mapget/model/mapschema.h"
#include "service-impl.h"

#include <tuple>

namespace mapget::detail
{
namespace
{

/** Zero disables expiry, even for a positive producer TTL; positive limits only shorten it. */
void capTileTtl(PartitionLayer& tile, std::chrono::milliseconds limit)
{
    if (!tile.ttl() || tile.ttl()->count() <= 0 || *tile.ttl() > limit)
        tile.setTtl(limit);
}

/** Bound empty source lifetimes; otherwise apply ordinary datasource/service defaults. */
void applySuccessfulTileTtl(
    PartitionLayer& tile,
    DataSource& dataSource,
    std::optional<std::chrono::milliseconds> const& defaultTtl,
    std::chrono::milliseconds emptyTileTtl)
{
    auto const type = tile.layerInfo()->type_;
    auto const empty = (type == LayerType::Features &&
                        static_cast<PartitionFeatureLayer&>(tile).numRoots() == 0) ||
        (type == LayerType::SourceData &&
         static_cast<PartitionSourceDataLayer&>(tile).numRoots() == 0);
    if (empty) {
        // Empty subsets are not source layers and never enter here. Source overrides
        // distinguish explicit zero (no retries) from omission (service policy).
        capTileTtl(tile, dataSource.emptyTileTtl().value_or(emptyTileTtl));
        return;
    }
    if (tile.ttl()) {
        return;
    }
    auto fallback = dataSource.ttl();
    if (!fallback) {
        fallback = defaultTtl;
    }
    if (fallback) {
        tile.setTtl(fallback);
    }
}

/** Carry the earliest finite dependency expiry into the composed result. */
void includeLifetime(PartitionLayer& composite, PartitionLayer const& contributor)
{
    auto const contributorTtl = contributor.ttl();
    if (!contributorTtl || contributorTtl->count() <= 0) {
        return;
    }
    auto const compositeTtl = composite.ttl();
    auto const contributorOrder = std::tuple{
        contributor.timestamp() + *contributorTtl,
        contributor.timestamp(),
        contributor.stringPoolId(),
    };
    auto const compositeOrder = compositeTtl && compositeTtl->count() > 0 ?
        std::optional{std::tuple{
            composite.timestamp() + *compositeTtl,
            composite.timestamp(),
            composite.stringPoolId(),
        }} :
        std::nullopt;
    if (!compositeOrder || contributorOrder < *compositeOrder) {
        composite.setTimestamp(contributor.timestamp());
        composite.setTtl(*contributorTtl);
    }
}

}  // namespace

TileLoadJob::TileLoadJob(
    ServiceScheduler& scheduler,
    std::shared_ptr<SourceConcurrency> source,
    std::shared_ptr<TileLoadState> state)
    : scheduler_(scheduler),
      source_(std::move(source)),
      registration_(source_->source),
      state_(std::move(state))
{
    // Constructed under the scheduler mutex. SourceConcurrency may publish a newer
    // registration while this job runs; only its permit counter remains shared.
}

TileLoadJob::TileLoadJob(
    ServiceScheduler& scheduler,
    std::shared_ptr<TileLoadState> state,
    PartitionLayer::Ptr cachedLayer)
    : scheduler_(scheduler), state_(std::move(state)), cachedLayer_(std::move(cachedLayer))
{
}

void TileLoadJob::run() noexcept
{
    if (cachedLayer_) {
        try {
            scheduler_.completeTileJob(*state_, cachedLayer_, false);
        }
        catch (std::exception const& error) {
            log().error(
                "Could not process cached tile {}: {}",
                state_->tileKey.toString(),
                error.what());
            scheduler_.failTileJob(*state_);
        }
        catch (...) {
            log().error(
                "Could not process cached tile {}: non-standard exception.",
                state_->tileKey.toString());
            scheduler_.failTileJob(*state_);
        }
        return;
    }

    auto permitHeld = true;
    auto releasePermit = [&]() noexcept
    {
        if (!permitHeld) {
            return;
        }
        scheduler_.releaseSourcePermit(source_);
        permitHeld = false;
    };
    TileCancellationCheck isCancelled = [state = state_] { return state->cancelled.load(); };
    try {
        if (isCancelled())
            throw TileLoadCancelled();
        if (state_->cacheExpiredAt) {
            registration_->dataSource->onCacheExpired(state_->tileKey, *state_->cacheExpiredAt);
        }

        scheduler_.notifyTileLoadState(*state_, PartitionLayer::LoadState::BackendFetching);
        auto notifyWaitingRequests = [this](PartitionLayer::LoadState state)
        {
            scheduler_.notifyTileLoadState(*state_, state);
        };
        auto effectiveInfo = registration_->info->getLayer(state_->tileKey.layerId_);
        auto nativeInfo = registration_->nativeInfo->getLayer(state_->tileKey.layerId_, false);
        PartitionLayer::Ptr layer;
        if (nativeInfo) {
            layer = registration_->dataSource->get(
                state_->tileKey,
                scheduler_.cache_,
                *registration_->nativeInfo,
                std::move(notifyWaitingRequests),
                isCancelled);
        }
        else {
            // An add-on may publish a new feature layer which the primary converter
            // cannot fill. Its contributions still run in the primary's source job.
            if (effectiveInfo->type_ != LayerType::Features)
                raise("Only feature layers may be introduced by add-ons.");
            layer = std::make_shared<PartitionFeatureLayer>(
                state_->tileKey.partitionId_,
                registration_->info->stringPoolId_,
                registration_->info->mapId_,
                effectiveInfo,
                scheduler_.cache_->getStringPool(registration_->info->stringPoolId_));
        }
        if (!layer) {
            raise("DataSource::get() returned null.");
        }
        if (nativeInfo && layer->schemaId() != nativeInfo->schemaOwner()->schemaId())
            raise("DataSource::get() returned a model with a different native schema.");
        if (layer->schemaId() != effectiveInfo->schemaOwner()->schemaId()) {
            if (layer.use_count() != 1)
                raise(
                    "DataSource::get() must return an exclusively owned model for schema "
                    "composition.");
            if (effectiveInfo->type_ == LayerType::Features)
                std::static_pointer_cast<PartitionFeatureLayer>(layer)->rebindSchema(effectiveInfo);
            else if (effectiveInfo->type_ == LayerType::SourceData)
                std::static_pointer_cast<PartitionSourceDataLayer>(layer)
                    ->rebindSchema(effectiveInfo);
            else
                raise("Unsupported native partition kind for schema composition.");
        }

        if (layer->error()) {
            capTileTtl(
                *layer,
                registration_->dataSource->errorTileTtl().value_or(scheduler_.errorTileTtl_));
        }
        else if (nativeInfo) {
            applySuccessfulTileTtl(
                *layer,
                *registration_->dataSource,
                scheduler_.defaultTtl_,
                scheduler_.emptyTileTtl_);
        }

        if (layer->layerInfo()->type_ == LayerType::Features) {
            loadAddOnTiles(
                std::static_pointer_cast<PartitionFeatureLayer>(layer),
                *registration_,
                scheduler_.cache_,
                scheduler_.defaultTtl_,
                scheduler_.emptyTileTtl_,
                scheduler_.errorTileTtl_,
                isCancelled);
        }

        // The worker continues with cache publication and every attached
        // filter/direct consumer, but those CPU/transport phases must not
        // consume the datasource's backend-concurrency budget.
        releasePermit();
        scheduler_.completeTileJob(*state_, layer, true);
    }
    catch (TileLoadCancelled const&) {
        releasePermit();
        scheduler_.failTileJob(*state_);
    }
    catch (std::exception const& error) {
        releasePermit();
        log().error("Could not load tile {}: {}", state_->tileKey.toString(), error.what());
        scheduler_.failTileJob(*state_);
    }
    catch (...) {
        releasePermit();
        log().error("Could not load tile {}: non-standard exception.", state_->tileKey.toString());
        scheduler_.failTileJob(*state_);
    }
}

void loadAddOnTiles(
    PartitionFeatureLayer::Ptr const& baseTile,
    RegisteredDataSource const& baseSource,
    Cache::Ptr& cache,
    std::optional<std::chrono::milliseconds> const& defaultTtl,
    std::chrono::milliseconds emptyTileTtl,
    std::chrono::milliseconds errorTileTtl,
    TileCancellationCheck const& isCancelled)
{
    for (auto const& addOn : baseSource.addOns) {
        auto layer = addOn->nativeInfo->getLayer(baseTile->layerInfo()->layerId_, false);
        if (!layer)
            continue;
        auto const errorTtl = addOn->dataSource->errorTileTtl().value_or(errorTileTtl);
        // Missing or incompatible contributions have no payload lifetime to merge.
        // Zero adds no retry deadline; it must not erase other contributors' expiry.
        auto boundIncomplete = [&]
        {
            if (errorTtl.count() > 0)
                capTileTtl(*baseTile, errorTtl);
        };
        if (layer->type_ != LayerType::Features) {
            baseTile->addWarning("An advertised add-on provider has an incompatible layer type.");
            boundIncomplete();
            continue;
        }
        if (isCancelled && isCancelled())
            throw TileLoadCancelled();
        auto loaded =
            addOn->dataSource->get(baseTile->id(), cache, *addOn->nativeInfo, {}, isCancelled);
        if (!loaded) {
            log().warn("Add-on datasource returned null for {}.", baseTile->id().toString());
            baseTile->addWarning("An advertised add-on provider returned no tile.");
            boundIncomplete();
            continue;
        }
        if (loaded->error()) {
            log().warn(
                "Error while fetching add-on tile {}: {}",
                baseTile->id().toString(),
                *loaded->error());
            baseTile->addWarning(fmt::format("Add-on provider failed: {}", *loaded->error()));
            capTileTtl(*loaded, errorTtl);
            includeLifetime(*baseTile, *loaded);
            continue;
        }
        if (loaded->layerInfo()->type_ != LayerType::Features) {
            log().warn("Add-on tile {} is not a feature layer.", baseTile->id().toString());
            baseTile->addWarning("An advertised add-on provider returned an incompatible layer.");
            boundIncomplete();
            continue;
        }
        auto addOnTile = std::static_pointer_cast<PartitionFeatureLayer>(loaded);
        if (addOnTile->schemaId() != addOn->nativeInfo->schemaId_) {
            baseTile->addWarning(fmt::format(
                "Add-on '{}' skipped: returned schema '{}' differs from its advertised "
                "schema '{}'. Fix the provider's native schema binding.",
                addOn->sourceId,
                addOnTile->schemaId(),
                addOn->nativeInfo->schemaId_));
            boundIncomplete();
            continue;
        }
        // Even an empty add-on may gain features later. Its lifetime must reach
        // the composite rather than disappearing during the merge.
        applySuccessfulTileTtl(*addOnTile, *addOn->dataSource, defaultTtl, emptyTileTtl);
        includeLifetime(*baseTile, *addOnTile);

        for (auto const& warning : addOnTile->warnings())
            baseTile->addWarning(fmt::format("{}: {}", addOnTile->id().toString(), warning));

        // Add-on strings must be cloned into a writable namespace shared by
        // both models; datasource-owned pools remain authoritative and frozen.
        auto combinedPoolId = baseTile->stringPoolId() + "|" + addOnTile->stringPoolId();
        baseTile->setStrings(cache->getStringPool(combinedPoolId));
        baseTile->setStringPoolId(combinedPoolId);

        PartitionFeatureLayer::CloneCache cloneCache;
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
    RegisteredDataSource::Ptr registration;
    std::function<void(AttachmentResult)> callback;

    /** Capture a valid datasource attachment advertised by the source tile. */
    void consumeTile(PartitionFeatureLayer::Ptr const& tile)
    {
        if (!tile || tile->error() || tile->glbAttachmentName() != request.name_) {
            return;
        }
        auto produced = registration->dataSource->attachment(request);
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
PartitionFeatureLayer::Ptr restrictFeatureLayerForResponse(
    PartitionFeatureLayer::Ptr const& source,
    std::span<std::string const> featureIds)
{
    return cloneInspectionFeatures(source, featureIds);
}

}  // namespace

LayerTilesRequest::LayerTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<PartitionId> tiles)
    : LayerTilesRequest(std::move(mapId), std::move(layerId), std::move(tiles), {})
{
}

LayerTilesRequest::LayerTilesRequest(
    std::string mapId,
    std::string layerId,
    std::vector<PartitionId> tiles,
    std::vector<PartitionId> const& priorityPartitionIds)
    : mapId_(std::move(mapId)),
      layerId_(std::move(layerId)),
      tileIds_(std::move(tiles)),
      priorityPartitionIds_({priorityPartitionIds.begin(), priorityPartitionIds.end()})
{
    std::set<PartitionId> seenPartitionIds;
    std::vector<PartitionId> uniquePartitionIds;
    uniquePartitionIds.reserve(tileIds_.size());
    for (auto const& tileId : tileIds_) {
        if (seenPartitionIds.insert(tileId).second) {
            uniquePartitionIds.push_back(tileId);
        }
    }
    tileIds_.swap(uniquePartitionIds);
    for (auto const& priorityPartitionId : priorityPartitionIds_) {
        if (!seenPartitionIds.contains(priorityPartitionId)) {
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
    resolvedTileKeys_.clear();
    tileKeysNotStarted_.clear();
    {
        std::lock_guard lock(statusMutex_);
        liveTileKeys_.clear();
        claimedTileKeys_.clear();
        completedTileKeys_.clear();
    }

    const auto isPriorityTile = [this](PartitionId const& tileId)
    {
        return priorityPartitionIds_.find(tileId) != priorityPartitionIds_.end();
    };
    const auto appendKey = [this](MapPartitionKey key)
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
            appendKey(MapPartitionKey(layerType, mapId_, layerId_, tileId));
        }
    };
    if (priorityPartitionIds_.empty()) {
        appendTiles(std::nullopt);
    }
    else {
        appendTiles(true);
        appendTiles(false);
    }

    {
        std::lock_guard lock(statusMutex_);
        liveTileKeys_.insert(resolvedTileKeys_.begin(), resolvedTileKeys_.end());
    }

    status_ = resolvedTileKeys_.empty() ? RequestStatus::Success : RequestStatus::Open;
}

std::tuple<bool, bool, bool>
LayerTilesRequest::retainOutputPartitionIds(std::set<PartitionId> const& retained)
{
    std::lock_guard lock(statusMutex_);
    auto const previousSize = liveTileKeys_.size();
    std::erase_if(
        liveTileKeys_,
        [&](MapPartitionKey const& key) { return !retained.contains(key.partitionId_); });
    std::erase_if(
        claimedTileKeys_,
        [&](MapPartitionKey const& key) { return !liveTileKeys_.contains(key); });
    std::erase_if(
        completedTileKeys_,
        [&](MapPartitionKey const& key) { return !liveTileKeys_.contains(key); });
    std::erase_if(
        featureIdsByTile_,
        [&](auto const& item) { return !retained.contains(item.first); });
    std::erase_if(
        priorityPartitionIds_,
        [&](PartitionId const& tileId) { return !retained.contains(tileId); });
    tileIds_.erase(
        std::remove_if(
            tileIds_.begin(),
            tileIds_.end(),
            [&](PartitionId const& tileId) { return !retained.contains(tileId); }),
        tileIds_.end());
    return {
        !liveTileKeys_.empty(),
        !liveTileKeys_.empty() && claimedTileKeys_.empty() &&
            completedTileKeys_.size() == liveTileKeys_.size(),
        liveTileKeys_.size() != previousSize,
    };
}

void LayerTilesRequest::notifyResult(PartitionLayer::Ptr r)
{
    if (!r) {
        return;
    }

    auto const key = MapPartitionKey(*r);
    auto const type = r->layerInfo()->type_;
    std::optional<std::vector<std::string>> featureRestriction;
    bool completesRequest = false;
    {
        std::lock_guard lock(statusMutex_);
        if (isDone() || !liveTileKeys_.contains(key) ||
            completedTileKeys_.contains(key) || !claimedTileKeys_.insert(key).second)
        {
            return;
        }
        if (type == LayerType::Features) {
            if (auto restriction = featureIdsByTile_.find(key.partitionId_);
                restriction != featureIdsByTile_.end())
            {
                featureRestriction = restriction->second;
            }
        }
    }

    // Invoke consumers outside statusMutex_; a claimed key prevents duplicate delivery.
    try {
        switch (type) {
        case LayerType::Features:
            if (onFeatureLayer_) {
                auto featureLayer = std::static_pointer_cast<PartitionFeatureLayer>(r);
                if (featureRestriction) {
                    featureLayer =
                        restrictFeatureLayerForResponse(featureLayer, *featureRestriction);
                }
                onFeatureLayer_(std::move(featureLayer));
            }
            break;
        case LayerType::SourceData:
            if (onSourceDataLayer_) {
                onSourceDataLayer_(
                    std::move(std::static_pointer_cast<PartitionSourceDataLayer>(r)));
            }
            break;
        default:
            log().error(
                fmt::format(
                    "Unhandled layer type {}, no matching callback!",
                    static_cast<int>(type)));
            break;
        }
    }
    catch (...) {
        std::lock_guard lock(statusMutex_);
        claimedTileKeys_.erase(key);
        throw;
    }

    {
        std::lock_guard lock(statusMutex_);
        claimedTileKeys_.erase(key);
        if (!isDone() && liveTileKeys_.contains(key)) {
            completedTileKeys_.insert(key);
            completesRequest = claimedTileKeys_.empty() &&
                completedTileKeys_.size() == liveTileKeys_.size();
        }
    }

    if (completesRequest) {
        setStatus(RequestStatus::Success);
    }
}

void LayerTilesRequest::notifyLoadState(MapPartitionKey const& key, PartitionLayer::LoadState state)
    const
{
    {
        std::lock_guard lock(statusMutex_);
        if (isDone() || !liveTileKeys_.contains(key)) {
            return;
        }
    }
    if (onLoadStateChanged_) {
        onLoadStateChanged_(key, state);
    }
}

void LayerTilesRequest::setStatus(RequestStatus s)
{
    auto expected = RequestStatus::Open;
    if (!status_.compare_exchange_strong(expected, s)) {
        return;
    }
    notifyStatus();
}

/** Wake request waiters even when the terminal consumer callback fails. */
void LayerTilesRequest::notifyStatus()
{
    try {
        if (isDone() && onDone_)
            onDone_(this->status_);
    }
    catch (...) {
        statusConditionVariable_.notify_all();
        throw;
    }
    statusConditionVariable_.notify_all();
}

void LayerTilesRequest::wait()
{
    std::unique_lock doneLock(statusMutex_);
    // Check before waiting to avoid an indefinite block: empty requests can
    // already be terminal before any notifier runs.
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
        tileIds.emplace_back(tileId.toJson());
    }
    requestJson["partitions"] = std::move(tileIds);
    if (!priorityPartitionIds_.empty()) {
        auto priorityPartitionIds = nlohmann::json::array();
        for (auto const& tileId : priorityPartitionIds_) {
            priorityPartitionIds.emplace_back(tileId.toJson());
        }
        requestJson["priorityPartitions"] = std::move(priorityPartitionIds);
    }
    if (!featureIdsByTile_.empty()) {
        auto featureIds = nlohmann::json::array();
        for (auto const& [tileId, ids] : featureIdsByTile_) {
            featureIds.push_back({
                {"partition", tileId.toJson()},
                {"ids", ids},
            });
        }
        requestJson["featureIds"] = std::move(featureIds);
    }
    return requestJson;
}

RequestStatus LayerTilesRequest::getStatus() const
{
    return this->status_;
}

bool LayerTilesRequest::isDone() const
{
    return status_ != RequestStatus::Open;
}

LayerTilesRequest&
LayerTilesRequest::setWorkAdmissionGate(std::shared_ptr<std::atomic_bool const> gate)
{
    workAdmissionGate_ = std::move(gate);
    return *this;
}

bool LayerTilesRequest::admitsNewWork() const
{
    return !workAdmissionGate_ || workAdmissionGate_->load(std::memory_order_acquire);
}

bool Service::Impl::requestTiles(
    std::vector<LayerTilesRequest::Ptr> const& requests,
    std::optional<ClientAuthorization> const& clientAuthorization,
    detail::RegisteredDataSource::Ptr const& expectedSource)
{
    auto allAvailable = true;
    std::vector<detail::RegisteredDataSource::Ptr> registrations;
    registrations.reserve(requests.size());
    for (auto const& request : requests) {
        if (!request) {
            raise("Attempt to request a null LayerTilesRequest.");
        }
        auto& registration = registrations.emplace_back();
        auto const context = resolveLayerRequest(
            request->mapId_,
            request->layerId_,
            clientAuthorization,
            request->sourceId_,
            &registration);
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
            // Descendants must use the generation that authorized their parent,
            // even if its replacement publishes an identical schema.
            if (expectedSource && registration != expectedSource) {
                allAvailable = false;
                request->setStatus(RequestStatus::Aborted);
                break;
            }
            if (std::ranges::any_of(
                    request->tileIds_,
                    [&](auto const& id) { return id.kind() != context.partitionKind_; }))
                throw std::invalid_argument(
                    "Requested partition kind does not match layer metadata.");
            request->prepareResolvedLayer(context.layerType_);
            if (request->isDone()) {
                request->notifyStatus();
            }
            break;
        }
    }

    // Bundles are atomic: one rejected member aborts every otherwise-open
    // sibling rather than exposing a partially fulfilled logical request.
    if (!allAvailable) {
        for (auto const& request : requests) {
            if (request->getStatus() == RequestStatus::Open) {
                request->setStatus(RequestStatus::Aborted);
            }
        }
        return false;
    }
    return scheduler_.enqueueRequests(requests, registrations);
}

void Service::Impl::requestAttachment(
    AttachmentRequest request,
    std::function<void(AttachmentResult)> callback,
    std::optional<ClientAuthorization> const& clientAuthorization)
{
    if (!callback) {
        return;
    }
    if (request.tileKey_.layer_ != LayerType::Features || request.name_.empty()) {
        callback({});
        return;
    }

    detail::RegisteredDataSource::Ptr selectedSource;
    auto const context = resolveLayerRequest(
        request.tileKey_.mapId_,
        request.tileKey_.layerId_,
        clientAuthorization,
        request.sourceId_,
        &selectedSource);
    if (context.status_ != RequestStatus::Success || context.layerType_ != LayerType::Features) {
        callback(AttachmentResult{.status_ = context.status_});
        return;
    }

    auto execution = std::make_shared<detail::AttachmentRequestExecution>();
    execution->request = std::move(request);
    execution->registration = selectedSource;
    execution->callback = std::move(callback);
    auto tileRequest = std::make_shared<LayerTilesRequest>(
        execution->request.tileKey_.mapId_,
        execution->request.tileKey_.layerId_,
        std::vector<PartitionId>{execution->request.tileKey_.partitionId_});
    tileRequest->sourceId_ = execution->request.sourceId_;
    tileRequest->onFeatureLayer(
        [execution](PartitionFeatureLayer::Ptr tile) { execution->consumeTile(tile); });
    tileRequest->onDone_ = [execution](RequestStatus status)
    {
        execution->finish(status);
    };
    (void)requestTiles({tileRequest}, clientAuthorization, selectedSource);
}

}  // namespace mapget
