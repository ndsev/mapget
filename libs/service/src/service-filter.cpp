#include "service-filter.h"

#include "fmt/format.h"
#include "mapget/log.h"
#include "mapget/model/featureid.h"
#include "service-impl.h"
#include "service-memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace mapget
{
namespace
{

using detail::FilterMemoryTracker;

/** Map a point-grid cell center to the output tile that permanently owns it. */
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
    auto const& group = request.channels_[member.channelIndex_].group_;
    if (!group) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InternalError,
            "Point-group member references a channel without grouping.",
        });
    }

    auto const longitude = group->origin_.x +
        (static_cast<double>(member.key_.x_) + 0.5) * group->cellSize_.x;
    auto const latitude = group->origin_.y +
        (static_cast<double>(member.key_.y_) + 0.5) * group->cellSize_.y;
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
    auto const boundedLatitude =
        std::clamp(latitude, -90.0, std::nextafter(90.0, -std::numeric_limits<double>::infinity()));
    return TileId::fromWgs84(wrappedLongitude, boundedLatitude, level);
}

/** Append trace fragments with equal names while preserving source order. */
void mergeFilterTraces(
    std::map<std::string, simfil::Trace>& target,
    std::map<std::string, simfil::Trace> source)
{
    for (auto&& [name, trace] : source) {
        target[name].append(std::move(trace));
    }
}

/** Build the common generation-aware filter status payload. */
nlohmann::json
makeFilterStatusJson(FeatureLayerFilterTilesRequest const& request, std::string state)
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

}  // namespace

namespace detail
{

FilterEvaluationJob::FilterEvaluationJob(
    std::string mapId,
    FeatureLayerFilterTilesRequest::Ptr const& owner,
    std::function<void()> work,
    std::function<void()> discard)
    : mapId_(std::move(mapId)), owner_(owner), work_(std::move(work)), discard_(std::move(discard))
{
}

void FilterEvaluationJob::run() noexcept
{
    auto owner = owner_.lock();
    if (!owner || owner->isDone() || !work_) {
        discard();
        return;
    }
    try {
        work_();
        discard_ = {};
    }
    catch (std::exception const& error) {
        log().error("Unhandled filter evaluation failure: {}", error.what());
        owner->setStatus(RequestStatus::Aborted);
        discard();
    }
    catch (...) {
        log().error("Unhandled non-standard filter evaluation failure.");
        owner->setStatus(RequestStatus::Aborted);
        discard();
    }
}

void FilterEvaluationJob::discard() noexcept
{
    if (!discard_) {
        return;
    }
    try {
        auto callback = std::exchange(discard_, {});
        callback();
    }
    catch (std::exception const& error) {
        log().error("Filter evaluation discard failed: {}", error.what());
    }
    catch (...) {
        log().error("Filter evaluation discard failed with a non-standard exception.");
    }
}

bool FilterEvaluationJob::cancelled() const
{
    auto owner = owner_.lock();
    return !owner || owner->isDone() || !work_;
}

std::shared_ptr<SourceConcurrency> FilterEvaluationJob::sourceAffinity() const
{
    return {};
}

std::string_view FilterEvaluationJob::mapId() const
{
    return mapId_;
}

FeatureLayerFilterTilesRequest::Ptr FilterEvaluationJob::filterOwner() const
{
    return owner_.lock();
}

ServiceJobKind FilterEvaluationJob::kind() const
{
    return ServiceJobKind::FilterEvaluation;
}

FilterRequestExecution::~FilterRequestExecution()
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
[[nodiscard]] uint64_t FilterRequestExecution::orchestrationBytesLocked() const
{
    MemoryUsageBreakdown usage;
    usage.add("state", {sizeof(FilterRequestExecution), sizeof(FilterRequestExecution)});
    usage.add(
        "request",
        {
            sizeof(FeatureLayerFilterTilesRequest),
            sizeof(FeatureLayerFilterTilesRequest),
        });
    usage.add("request-strings", stringMemoryUsage(request->mapId_));
    usage.add("request-strings", stringMemoryUsage(request->layerId_));
    if (request->sourceId_) {
        usage.add("request-strings", stringMemoryUsage(*request->sourceId_));
    }
    usage.add("requested-tile-ids", vectorMemoryUsage(request->tileIds_));
    usage.add(
        "delivery-epochs",
        {
            request->deliveryEpochs_.size() * sizeof(std::pair<TileId const, uint64_t>),
            request->deliveryEpochs_.size() *
                (sizeof(std::pair<TileId const, uint64_t>) + 3 * sizeof(void*)),
        });
    usage.add(
        "priority-tile-index",
        {
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
            usage.add(
                "filter-channel-strings",
                stringMemoryUsage(*channel.relation_->relationNamePattern_));
        }
        usage.add("filter-feature-types", stringVectorMemoryUsage(channel.featureTypes_));
        usage.add("filter-feature-fields", stringVectorMemoryUsage(channel.featureFields_));
        usage.add("filter-entry-fields", stringVectorMemoryUsage(channel.entryFields_));
    }
    usage.add(
        "filter-bindings",
        {
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
    usage.add(
        "source-index",
        {
            sourceIndexByTile.size() * sizeof(decltype(sourceIndexByTile)::value_type),
            sourceIndexByTile.size() *
                (sizeof(decltype(sourceIndexByTile)::value_type) + 3 * sizeof(void*)),
        });
    usage.add(
        "output-index",
        {
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
            usage
                .add("relation-descriptors", vectorMemoryUsage(contribution->relationDescriptors_));
            usage.add("issues", vectorMemoryUsage(contribution->issues_));
            for (auto const& issue : contribution->issues_) {
                usage.add("issue-strings", stringMemoryUsage(issue.channelId_));
                usage.add("issue-strings", stringMemoryUsage(issue.expression_));
                usage.add("issue-strings", stringMemoryUsage(issue.message_));
            }
            usage.add("diagnostics", diagnosticsMemoryUsage(contribution->diagnostics_));
            usage.add(
                "trace-index",
                {
                    contribution->traces_.size() *
                        sizeof(decltype(contribution->traces_)::value_type),
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
    usage.add(
        "received-source-flags",
        {
            (receivedSourceTiles.size() + 7) / 8,
            (receivedSourceTiles.capacity() + 7) / 8,
        });
    usage.add(
        "committed-source-flags",
        {
            (committedSourceTiles.size() + 7) / 8,
            (committedSourceTiles.capacity() + 7) / 8,
        });
    usage.add(
        "group-channel-index",
        {
            groupChannelIds.size() * sizeof(decltype(groupChannelIds)::value_type),
            groupChannelIds.size() *
                (sizeof(decltype(groupChannelIds)::value_type) + 3 * sizeof(void*)),
        });
    for (auto const& id : groupChannelIds) {
        usage.add("group-channel-ids", stringMemoryUsage(id));
    }
    usage.add(
        "pending-relation-outputs",
        {
            pendingRelationOutputs.size() * sizeof(decltype(pendingRelationOutputs)::value_type),
            pendingRelationOutputs.size() *
                (sizeof(decltype(pendingRelationOutputs)::value_type) + 3 * sizeof(void*)),
        });
    usage.add(
        "relation-target-tiles",
        {
            relationTargetTiles.size() * sizeof(decltype(relationTargetTiles)::value_type),
            relationTargetTiles.size() *
                (sizeof(decltype(relationTargetTiles)::value_type) + 3 * sizeof(void*)),
        });
    for (auto const& [_, target] : relationTargetTiles) {
        usage.add(
            "relation-target-dependents",
            {
                target.dependentOutputs_.size() * sizeof(size_t),
                target.dependentOutputs_.size() * (sizeof(size_t) + 3 * sizeof(void*)),
            });
        if (target.failureMessage_) {
            usage.add("relation-target-errors", stringMemoryUsage(*target.failureMessage_));
        }
    }
    return usage.total().allocatedBytes;
}

/** Measure source-local vectors that exist between SIMFIL evaluation and state commit. */
[[nodiscard]] uint64_t
FilterRequestExecution::sourceResultAuxiliaryBytes(FeatureLayerFilterSourceResult const& result)
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
    usage.add(
        "trace-index",
        {
            result.traces_.size() * sizeof(decltype(result.traces_)::value_type),
            result.traces_.size() *
                (sizeof(decltype(result.traces_)::value_type) + 3 * sizeof(void*)),
        });
    for (auto const& [name, _] : result.traces_) {
        usage.add("trace-names", stringMemoryUsage(name));
    }
    return usage.total().allocatedBytes;
}

void FilterRequestExecution::configure(
    std::vector<TileId> const& outputTileIds,
    std::vector<TileId> processingTileIds)
{
    sourceTileIds = std::move(processingTileIds);
    receivedSourceTiles.resize(sourceTileIds.size(), false);
    committedSourceTiles.resize(sourceTileIds.size(), false);
    dependentOutputsBySource.resize(sourceTileIds.size());
    for (size_t index = 0; index < sourceTileIds.size(); ++index) {
        sourceIndexByTile.emplace(sourceTileIds[index], index);
    }
    for (auto const& channel : request->filter_.channels_) {
        if (channel.group_) {
            groupChannelIds.insert(channel.channelId_);
        }
    }

    outputs.reserve(outputTileIds.size());
    for (size_t outputIndex = 0; outputIndex < outputTileIds.size(); ++outputIndex) {
        auto const outputTileId = outputTileIds[outputIndex];
        outputIndexByTile.emplace(outputTileId, outputIndex);

        std::set<TileId> dependencyMembership{outputTileId};
        if (hasPointGroups) {
            for (int32_t offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int32_t offsetX = -1; offsetX <= 1; ++offsetX) {
                    dependencyMembership.insert(outputTileId.neighbour(offsetX, offsetY));
                }
            }
        }

        OutputTileState output;
        output.tileId_ = outputTileId;
        for (auto const& sourceTileId : sourceTileIds) {
            if (dependencyMembership.contains(sourceTileId)) {
                output.sourceTileIds_.push_back(sourceTileId);
            }
        }
        output.contributions_.resize(output.sourceTileIds_.size());
        output.missingContributions_ = output.sourceTileIds_.size();
        outputs.push_back(std::move(output));
    }

    for (size_t outputIndex = 0; outputIndex < outputs.size(); ++outputIndex) {
        auto const& output = outputs[outputIndex];
        for (size_t slotIndex = 0; slotIndex < output.sourceTileIds_.size(); ++slotIndex) {
            dependentOutputsBySource.at(sourceIndexByTile.at(output.sourceTileIds_[slotIndex]))
                .push_back({
                    outputIndex,
                    slotIndex,
                });
        }
    }
}

[[nodiscard]] nlohmann::json FilterRequestExecution::progress(std::string state)
{
    std::lock_guard lock(mutex);
    auto status = makeFilterStatusJson(*request, std::move(state));
    status["outputTilesRequested"] = outputs.size();
    status["sourceTilesQueued"] = sourceTileIds.size();
    status["sourceTilesLoaded"] = loadedSourceTiles;
    status["sourceTilesEvaluated"] = evaluatedSourceTiles;
    status["outputTilesReady"] = readyOutputTiles;
    status["outputTilesEmitted"] = emittedOutputTiles;
    status["entriesEmitted"] = entriesEmitted;
    return status;
}

void FilterRequestExecution::emitProgress(std::string state, bool force)
{
    // Per-source callbacks can outnumber useful UI refreshes by
    // thousands in a large viewport. Keep exact counters in request
    // state, but serialize only periodic intermediate snapshots.
    constexpr size_t ProgressEventStride = 32;
    if (!force) {
        auto const event = progressEventCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (event % ProgressEventStride != 0) {
            return;
        }
    }
    request->notifyProgress(progress(std::move(state)));
}

void FilterRequestExecution::releaseChildRequests()
{
    std::lock_guard lock(request->childRequestsMutex_);
    request->childRequests_.clear();
}

void FilterRequestExecution::abortChildRequests()
{
    std::vector<LayerTilesRequest::Ptr> children;
    {
        std::lock_guard lock(request->childRequestsMutex_);
        children = request->childRequests_;
        request->childRequests_.clear();
    }
    for (auto const& child : children) {
        if (child && !child->isDone()) {
            impl->scheduler_.abortRequest(child);
        }
    }
}

void FilterRequestExecution::finishIfComplete()
{
    RequestStatus finalStatus = RequestStatus::Open;
    {
        std::lock_guard lock(mutex);
        if (terminal || !childRequestDone || pendingEvaluationJobs != 0 ||
            evaluatedSourceTiles < sourceTileIds.size() || emittedOutputTiles < outputs.size())
        {
            return;
        }
        terminal = true;
        finalStatus = request->isCancelled() ? RequestStatus::Aborted : RequestStatus::Success;
    }
    releaseChildRequests();
    emitProgress(finalStatus == RequestStatus::Success ? "Success" : "Aborted", true);
    request->setStatus(finalStatus);
}

void FilterRequestExecution::fail(simfil::Error const& error)
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
    auto status = makeFilterStatusJson(*request, "Failed");
    status["error"] = error.message;
    request->notifyProgress(status);
    abortChildRequests();
    request->setStatus(RequestStatus::Aborted);
}

void FilterRequestExecution::collect(TileFeatureLayer::Ptr layer)
{
    if (!layer || request->isCancelled()) {
        return;
    }
    auto found = sourceIndexByTile.find(layer->tileId());
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
    impl->scheduler_.enqueueJob(std::make_unique<detail::FilterEvaluationJob>(
        sourceInfo->mapId_,
        request,
        [self, sourceIndex, sourceBytes, source = std::move(layer)]() mutable
        { self->evaluate(sourceIndex, sourceBytes, std::move(source)); },
        [self, sourceBytes]() { self->discardEvaluationJob(sourceBytes); }));
    emitProgress("SourceTileLoaded");
}

void FilterRequestExecution::discardEvaluationJob(uint64_t sourceBytes)
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

tl::expected<void, simfil::Error> FilterRequestExecution::locateRelationTargets(
    TileFeatureLayer const& source,
    FeatureLayerFilterSourceResult& result)
{
    if (result.relationDescriptors_.size() > 100000) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InvalidArguments,
            "Stored-relation traversal exceeded the initial 100000 directed-relation limit.",
        });
    }

    std::vector<FeatureLayerRelationDescriptor> retained;
    retained.reserve(result.relationDescriptors_.size());
    for (auto&& descriptor : result.relationDescriptors_) {
        if (descriptor.target_) {
            retained.push_back(std::move(descriptor));
            continue;
        }
        auto targetId = descriptor.relation_ ?
            descriptor.relation_->target() :
            model_ptr<FeatureId>{};
        auto const channelId = descriptor.channelIndex_ < request->filter_.channels_.size() ?
            request->filter_.channels_[descriptor.channelIndex_].channelId_ :
            std::string{};
        auto addIssue = [&](std::string message)
        {
            result.issues_.push_back(FilterIssue{
                channelId,
                "<relation-target>",
                Scope::Relation,
                std::move(message),
                1,
            });
        };
        if (!targetId) {
            addIssue("Stored relation has no target feature identity.");
            continue;
        }
        if (auto externalMap = targetId->externalMapId();
            externalMap && *externalMap != request->mapId_) {
            addIssue(fmt::format(
                "Cross-map relation target '{}' is unsupported.",
                targetId->toString()));
            continue;
        }

        LocateRequest
            locateRequest(request->mapId_, descriptor.targetTypeId_, descriptor.targetFeatureId_);
        auto const locationKey = locateRequest.serialize().dump();
        std::vector<LocateCandidate> locations;
        bool locationCached = false;
        {
            std::lock_guard lock(relationLocationMutex);
            auto found = relationLocationCache.find(locationKey);
            if (found != relationLocationCache.end()) {
                locations = found->second;
                locationCached = true;
            }
        }
        if (!locationCached) {
            // Never invoke a potentially remote datasource callback
            // while holding the shared location-cache mutex. Racing
            // misses may perform duplicate idempotent locate work;
            // the first normalized value installed wins.
            auto located = sourceDataSource->locate(locateRequest);
            std::map<std::string, LocateCandidate> unique;
            for (auto&& location : located) {
                if (location.tileKey_.layer_ != LayerType::Features ||
                    location.tileKey_.mapId_ != request->mapId_) {
                    continue;
                }
                unique.try_emplace(location.serialize().dump(), std::move(location));
            }
            std::vector<LocateCandidate> normalized;
            normalized.reserve(unique.size());
            for (auto&& [_, location] : unique) {
                normalized.push_back(std::move(location));
            }
            std::lock_guard lock(relationLocationMutex);
            auto [found, _] = relationLocationCache.try_emplace(locationKey, std::move(normalized));
            locations = found->second;
        }

        if (locations.empty()) {
            addIssue(fmt::format("Could not locate relation target '{}'.", targetId->toString()));
            continue;
        }
        bool selectorFailed = false;
        for (auto const& location : locations) {
            auto& candidate = descriptor.targetCandidates_.emplace_back(
                FeatureLayerRelationTargetCandidate{location.tileKey_, location.selector_, false});
            if (location.tileKey_ != source.id()) {
                continue;
            }
            auto selected = resolveLocateCandidate(location, source);
            if (!selected) {
                addIssue(fmt::format(
                    "Could not evaluate relation-target selector for '{}': {}",
                    targetId->toString(),
                    selected.error().message));
                selectorFailed = true;
                break;
            }
            candidate.resolved_ = true;
            descriptor.targetMatches_
                .insert(descriptor.targetMatches_.end(), selected->begin(), selected->end());
        }
        if (selectorFailed) {
            continue;
        }
        if (std::ranges::all_of(
                descriptor.targetCandidates_,
                &FeatureLayerRelationTargetCandidate::resolved_))
        {
            std::map<std::pair<MapTileKey, std::string>, model_ptr<Feature>> uniqueMatches;
            for (auto const& match : descriptor.targetMatches_) {
                uniqueMatches.emplace(
                    std::make_pair(MapTileKey(match->model()), match->id()->toString()),
                    match);
            }
            if (uniqueMatches.size() != 1) {
                addIssue(
                    uniqueMatches.empty() ?
                        fmt::format(
                            "Located relation target '{}' was not found in its candidate tiles.",
                            targetId->toString()) :
                        fmt::format(
                            "Relation target '{}' resolved ambiguously to {} features.",
                            targetId->toString(),
                            uniqueMatches.size()));
                continue;
            }
            descriptor.target_ = uniqueMatches.begin()->second;
            descriptor.targetTileKey_ = uniqueMatches.begin()->first.first;
            descriptor.targetTypeId_ = std::string(descriptor.target_->typeId());
            descriptor.targetFeatureId_ = castToKeyValue(descriptor.target_->id()->keyValuePairs());
        }
        retained.push_back(std::move(descriptor));
    }
    result.relationDescriptors_ = std::move(retained);
    return {};
}

tl::expected<std::vector<FilterRequestExecution::ReadyOutput>, simfil::Error>
FilterRequestExecution::commitSource(
    size_t sourceIndex,
    TileFeatureLayer const& source,
    uint64_t outputModelBytes,
    FeatureLayerFilterSourceResult result)
{
    std::map<size_t, std::vector<FeatureLayerPointGroupMember>> membersByOutput;
    for (auto&& member : result.pointGroupMembers_) {
        auto owner = pointGroupOwnerTile(member, request->filter_, outputLevel);
        if (!owner) {
            return tl::unexpected(owner.error());
        }
        auto output = outputIndexByTile.find(*owner);
        if (output != outputIndexByTile.end()) {
            auto const& dependents = dependentOutputsBySource[sourceIndex];
            if (std::ranges::none_of(
                    dependents,
                    [&](auto const& dependent)
                    { return dependent.outputIndex_ == output->second; }))
            {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InternalError,
                    "Point-group member escaped the configured source halo.",
                });
            }
            membersByOutput[output->second].push_back(std::move(member));
        }
    }

    std::vector<ReadyOutput> ready;
    {
        std::lock_guard lock(mutex);
        if (terminal || request->isCancelled()) {
            return ready;
        }
        if (sourceIndex >= committedSourceTiles.size() || committedSourceTiles[sourceIndex]) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InternalError,
                "Filter source tile committed more than once.",
            });
        }

        auto outputForSource = outputIndexByTile.find(source.tileId());
        if (outputForSource != outputIndexByTile.end()) {
            if (!result.layer_) {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InternalError,
                    "Requested filter output source produced no WIP subset.",
                });
            }
            outputs[outputForSource->second].wipSubset_ = std::move(result.layer_);
            outputs[outputForSource->second].wipSubsetBytes_ = outputModelBytes;
        }
        else if (result.layer_) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InternalError,
                "Halo-only filter source produced an output subset.",
            });
        }

        auto const dependency = TileSubsetDependency{
            MapTileKey(source),
            result.sourceFeatureCount_,
        };
        for (auto const& dependent : dependentOutputsBySource[sourceIndex]) {
            auto& output = outputs[dependent.outputIndex_];
            auto& slot = output.contributions_[dependent.slotIndex_];
            if (slot) {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InternalError,
                    "Filter contribution slot was written more than once.",
                });
            }

            std::vector<FilterIssue> issues;
            issues.reserve(result.issues_.size());
            auto const isLocalOutput = output.tileId_ == source.tileId();
            for (auto const& issue : result.issues_) {
                if (isLocalOutput || groupChannelIds.contains(issue.channelId_)) {
                    issues.push_back(issue);
                }
            }
            simfil::Diagnostics contributionDiagnostics;
            contributionDiagnostics.append(result.diagnostics_);
            slot.emplace(SourceTileContribution{
                dependency,
                std::move(membersByOutput[dependent.outputIndex_]),
                isLocalOutput ? std::move(result.relationDescriptors_) :
                                std::vector<FeatureLayerRelationDescriptor>{},
                std::move(issues),
                result.traces_,
                std::move(contributionDiagnostics),
                source.ttl() && source.ttl()->count() > 0 ?
                    std::optional<SourceTileContribution::Lifetime>{
                        SourceTileContribution::Lifetime{
                            MapTileKey(source),
                            source.timestamp(),
                            *source.ttl(),
                        }} :
                    std::nullopt,
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
                item.contributions_.reserve(output.contributions_.size());
                for (auto& contribution : output.contributions_) {
                    if (!contribution) {
                        return tl::unexpected(simfil::Error{
                            simfil::Error::InternalError,
                            "Complete filter output has an empty contribution slot.",
                        });
                    }
                    item.contributions_.push_back(std::move(*contribution));
                }
                ready.push_back(std::move(item));
            }
        }

        committedSourceTiles[sourceIndex] = true;
        ++evaluatedSourceTiles;
    }
    std::ranges::sort(ready, {}, &ReadyOutput::outputIndex_);
    return ready;
}

tl::expected<void, simfil::Error> FilterRequestExecution::resolveRelationTargetInOutput(
    ReadyOutput& output,
    MapTileKey const& targetKey,
    TileFeatureLayer const& targetLayer)
{
    if (targetLayer.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InvalidArguments,
            "Relation target feature count exceeds the subset dependency representation.",
        });
    }
    SourceTileContribution targetContribution{
        TileSubsetDependency{
            targetKey,
            static_cast<uint32_t>(targetLayer.size()),
        },
        {},
        {},
        {},
        {},
        {},
        targetLayer.ttl() && targetLayer.ttl()->count() > 0 ?
            std::optional<SourceTileContribution::Lifetime>{
                SourceTileContribution::Lifetime{
                    targetKey,
                    targetLayer.timestamp(),
                    *targetLayer.ttl(),
                }} :
            std::nullopt,
    };

    for (auto& contribution : output.contributions_) {
        for (auto& descriptor : contribution.relationDescriptors_) {
            if (descriptor.target_) {
                continue;
            }
            for (auto& candidate : descriptor.targetCandidates_) {
                if (candidate.resolved_ || candidate.tileKey_ != targetKey) {
                    continue;
                }
                auto selected = targetLayer.find(candidate.selector_);
                if (!selected) {
                    return tl::unexpected(selected.error());
                }
                candidate.resolved_ = true;
                descriptor.targetMatches_
                    .insert(descriptor.targetMatches_.end(), selected->begin(), selected->end());
            }
        }
    }
    output.dynamicContributions_.erase(targetKey);
    output.dynamicContributions_.emplace(targetKey, std::move(targetContribution));
    return {};
}

void FilterRequestExecution::markRelationTargetUnavailableInOutput(
    ReadyOutput& output,
    MapTileKey const& targetKey,
    std::string const& failureMessage)
{
    std::map<std::string, uint64_t> affectedByChannel;
    for (auto& contribution : output.contributions_) {
        for (auto& descriptor : contribution.relationDescriptors_) {
            if (descriptor.target_) {
                continue;
            }
            bool affected = false;
            for (auto& candidate : descriptor.targetCandidates_) {
                if (candidate.resolved_ || candidate.tileKey_ != targetKey) {
                    continue;
                }
                candidate.resolved_ = true;
                affected = true;
            }
            if (!affected) {
                continue;
            }
            auto const channelId = descriptor.channelIndex_ < request->filter_.channels_.size() ?
                request->filter_.channels_[descriptor.channelIndex_].channelId_ :
                std::string{};
            ++affectedByChannel[channelId];
        }
    }
    for (auto const& [channelId, count] : affectedByChannel) {
        output.issues_.push_back(FilterIssue{
            channelId,
            "<relation-target>",
            Scope::Relation,
            failureMessage,
            count,
        });
    }
}

tl::expected<FilterRequestExecution::PreparedRelationOutputs, simfil::Error>
FilterRequestExecution::prepareRelationOutputs(std::vector<ReadyOutput> fixedReady)
{
    PreparedRelationOutputs prepared;
    std::lock_guard lock(mutex);
    if (terminal || request->isCancelled()) {
        return prepared;
    }

    for (auto&& ready : fixedReady) {
        std::set<MapTileKey> targetKeys;
        for (auto const& contribution : ready.contributions_) {
            for (auto const& descriptor : contribution.relationDescriptors_) {
                if (descriptor.target_) {
                    continue;
                }
                for (auto const& candidate : descriptor.targetCandidates_) {
                    if (!candidate.resolved_) {
                        targetKeys.insert(candidate.tileKey_);
                    }
                }
            }
        }

        std::set<MapTileKey> pendingKeys;
        for (auto const& targetKey : targetKeys) {
            auto [targetState, inserted] = relationTargetTiles.try_emplace(targetKey);
            if (inserted && relationTargetTiles.size() > 2048) {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InvalidArguments,
                    "Stored-relation traversal exceeded the initial 2048 unique-target-tile limit.",
                });
            }
            if (targetState->second.terminal_) {
                if (targetState->second.layer_) {
                    auto resolved = resolveRelationTargetInOutput(
                        ready,
                        targetKey,
                        *targetState->second.layer_);
                    if (!resolved) {
                        return tl::unexpected(resolved.error());
                    }
                }
                else {
                    markRelationTargetUnavailableInOutput(
                        ready,
                        targetKey,
                        targetState->second.failureMessage_.value_or(fmt::format(
                            "Could not load relation target tile {}.",
                            targetKey.toString())));
                }
                continue;
            }
            pendingKeys.insert(targetKey);
            targetState->second.dependentOutputs_.insert(ready.outputIndex_);
            if (!targetState->second.scheduled_) {
                targetState->second.scheduled_ = true;
                prepared.targetsToSchedule_.push_back(targetKey);
            }
        }

        if (pendingKeys.empty()) {
            prepared.ready_.push_back(std::move(ready));
            ++readyOutputTiles;
            continue;
        }
        auto [pending, inserted] = pendingRelationOutputs.emplace(
            ready.outputIndex_,
            PendingRelationOutput{
                std::move(ready),
                std::move(pendingKeys),
            });
        if (!inserted) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InternalError,
                "Filter output entered relation-target resolution more than once.",
            });
        }
    }
    return prepared;
}

void FilterRequestExecution::scheduleRelationTarget(MapTileKey const& targetKey)
{
    auto child = std::make_shared<LayerTilesRequest>(
        targetKey.mapId_,
        targetKey.layerId_,
        std::vector<TileId>{targetKey.tileId_});
    child->sourceId_ = request->sourceId_;
    auto self = shared_from_this();
    child->onFeatureLayer([self, targetKey](TileFeatureLayer::Ptr layer)
                          { self->collectRelationTarget(targetKey, std::move(layer)); });
    child->onDone_ = [self, targetKey](RequestStatus status)
    {
        self->completeUnavailableRelationTarget(
            targetKey,
            status == RequestStatus::Success ?
                fmt::format(
                    "Relation target request completed without tile {}.",
                    targetKey.toString()) :
                fmt::format("Could not load relation target tile {}.", targetKey.toString()));
    };
    {
        std::lock_guard lock(request->childRequestsMutex_);
        request->childRequests_.push_back(child);
    }
    if (!impl->requestTiles(std::vector<LayerTilesRequest::Ptr>{child}, clientHeaders)) {
        completeUnavailableRelationTarget(
            targetKey,
            fmt::format("Could not schedule relation target tile {}.", targetKey.toString()));
    }
}

void FilterRequestExecution::completeUnavailableRelationTarget(
    MapTileKey const& targetKey,
    std::string message)
{
    std::vector<ReadyOutput> ready;
    {
        std::lock_guard lock(mutex);
        if (terminal || request->isCancelled()) {
            return;
        }
        auto found = relationTargetTiles.find(targetKey);
        if (found == relationTargetTiles.end()) {
            return;
        }
        if (found->second.terminal_) {
            return;
        }
        found->second.terminal_ = true;
        found->second.failureMessage_ = std::move(message);
        for (auto const outputIndex : found->second.dependentOutputs_) {
            auto pending = pendingRelationOutputs.find(outputIndex);
            if (pending == pendingRelationOutputs.end()) {
                continue;
            }
            markRelationTargetUnavailableInOutput(
                pending->second.ready_,
                targetKey,
                *found->second.failureMessage_);
            pending->second.pendingTargetTiles_.erase(targetKey);
            if (pending->second.pendingTargetTiles_.empty()) {
                ready.push_back(std::move(pending->second.ready_));
                pendingRelationOutputs.erase(pending);
                ++readyOutputTiles;
            }
        }
    }
    std::ranges::sort(ready, {}, &ReadyOutput::outputIndex_);
    emitCompletedOutputs(std::move(ready));
    emitProgress("RelationTargetUnavailable");
    finishIfComplete();
}

void FilterRequestExecution::collectRelationTarget(
    MapTileKey const& targetKey,
    TileFeatureLayer::Ptr layer)
{
    if (!layer || layer->id() != targetKey) {
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
        if (terminal || request->isCancelled()) {
            return;
        }
        auto found = relationTargetTiles.find(targetKey);
        if (found == relationTargetTiles.end()) {
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
            memory->relationTargetModels.add(layer->memoryUsage().total().allocatedBytes);
            for (auto const outputIndex : found->second.dependentOutputs_) {
                auto pending = pendingRelationOutputs.find(outputIndex);
                if (pending == pendingRelationOutputs.end()) {
                    continue;
                }
                auto resolved =
                    resolveRelationTargetInOutput(pending->second.ready_, targetKey, *layer);
                if (!resolved) {
                    error = resolved.error();
                    break;
                }
                pending->second.pendingTargetTiles_.erase(targetKey);
                if (pending->second.pendingTargetTiles_.empty()) {
                    ready.push_back(std::move(pending->second.ready_));
                    pendingRelationOutputs.erase(pending);
                    ++readyOutputTiles;
                }
            }
        }
    }
    if (error) {
        fail(*error);
        return;
    }
    std::ranges::sort(ready, {}, &ReadyOutput::outputIndex_);
    emitCompletedOutputs(std::move(ready));
    emitProgress("RelationTargetResolved");
    finishIfComplete();
}

tl::expected<size_t, simfil::Error> FilterRequestExecution::finalizeOutput(ReadyOutput ready)
{
    std::vector<TileSubsetDependency> dependencies;
    std::vector<FeatureLayerPointGroupMember> members;
    std::vector<FeatureLayerRelationDescriptor> relationDescriptors;
    std::map<std::tuple<std::string, std::string, Scope, std::string>, FilterIssue> issues;
    std::map<std::string, simfil::Trace> traces;
    simfil::Diagnostics diagnostics;
    std::optional<SourceTileContribution::Lifetime> limitingLifetime;
    auto includeLifetime = [&](auto const& contribution)
    {
        if (contribution.lifetime_ &&
            (!limitingLifetime ||
             contribution.lifetime_->expiresAt() < limitingLifetime->expiresAt() ||
             (contribution.lifetime_->expiresAt() == limitingLifetime->expiresAt() &&
              contribution.lifetime_->sourceKey_ < limitingLifetime->sourceKey_)))
        {
            limitingLifetime = contribution.lifetime_;
        }
    };

    for (auto&& issue : ready.issues_) {
        auto key =
            std::make_tuple(issue.channelId_, issue.expression_, issue.scope_, issue.message_);
        auto [found, inserted] = issues.emplace(key, issue);
        if (!inserted) {
            auto const remaining = std::numeric_limits<uint64_t>::max() -
                found->second.occurrenceCount_;
            found->second.occurrenceCount_ += std::min(remaining, issue.occurrenceCount_);
        }
    }

    for (auto& contribution : ready.contributions_) {
        includeLifetime(contribution);
        dependencies.push_back(std::move(contribution.dependency_));
        members.insert(
            members.end(),
            std::make_move_iterator(contribution.pointGroupMembers_.begin()),
            std::make_move_iterator(contribution.pointGroupMembers_.end()));
        relationDescriptors.insert(
            relationDescriptors.end(),
            std::make_move_iterator(contribution.relationDescriptors_.begin()),
            std::make_move_iterator(contribution.relationDescriptors_.end()));
        for (auto&& issue : contribution.issues_) {
            auto key =
                std::make_tuple(issue.channelId_, issue.expression_, issue.scope_, issue.message_);
            auto [found, inserted] = issues.emplace(key, issue);
            if (!inserted) {
                auto const remaining = std::numeric_limits<uint64_t>::max() -
                    found->second.occurrenceCount_;
                found->second.occurrenceCount_ += std::min(remaining, issue.occurrenceCount_);
            }
        }
        mergeFilterTraces(traces, std::move(contribution.traces_));
        if (!contribution.diagnostics_.exprIndex_.empty() ||
            !contribution.diagnostics_.fieldData_.empty() ||
            !contribution.diagnostics_.comparisonData_.empty())
        {
            diagnostics.append(contribution.diagnostics_);
        }
    }
    for (auto& [_, contribution] : ready.dynamicContributions_) {
        includeLifetime(contribution);
        dependencies.push_back(std::move(contribution.dependency_));
        for (auto&& issue : contribution.issues_) {
            auto key =
                std::make_tuple(issue.channelId_, issue.expression_, issue.scope_, issue.message_);
            auto [found, inserted] = issues.emplace(key, issue);
            if (!inserted) {
                auto const remaining = std::numeric_limits<uint64_t>::max() -
                    found->second.occurrenceCount_;
                found->second.occurrenceCount_ += std::min(remaining, issue.occurrenceCount_);
            }
        }
        mergeFilterTraces(traces, std::move(contribution.traces_));
    }

    if (hasStoredRelations) {
        std::vector<FeatureLayerRelationDescriptor> resolvedDescriptors;
        resolvedDescriptors.reserve(relationDescriptors.size());
        for (auto&& descriptor : relationDescriptors) {
            if (descriptor.target_) {
                resolvedDescriptors.push_back(std::move(descriptor));
                continue;
            }
            if (std::ranges::any_of(
                    descriptor.targetCandidates_,
                    [](auto const& candidate) { return !candidate.resolved_; }))
            {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InternalError,
                    "Relation target reached finalization with unresolved locate candidates.",
                });
            }

            std::map<std::pair<MapTileKey, std::string>, model_ptr<Feature>> uniqueMatches;
            for (auto const& match : descriptor.targetMatches_) {
                uniqueMatches.emplace(
                    std::make_pair(MapTileKey(match->model()), match->id()->toString()),
                    match);
            }
            if (uniqueMatches.size() == 1) {
                auto const& [identity, match] = *uniqueMatches.begin();
                descriptor.target_ = match;
                descriptor.targetTileKey_ = identity.first;
                descriptor.targetTypeId_ = std::string(match->typeId());
                descriptor.targetFeatureId_ = castToKeyValue(match->id()->keyValuePairs());
                resolvedDescriptors.push_back(std::move(descriptor));
                continue;
            }

            auto const channelId = descriptor.channelIndex_ < request->filter_.channels_.size() ?
                request->filter_.channels_[descriptor.channelIndex_].channelId_ :
                std::string{};
            auto const targetIdentity = descriptor.relation_ && descriptor.relation_->target() ?
                descriptor.relation_->target()->toString() :
                formatFeatureIdString(descriptor.targetTypeId_, descriptor.targetFeatureId_);
            auto message = uniqueMatches.empty() ?
                fmt::format(
                    "Located relation target '{}' was not found in its candidate tiles.",
                    targetIdentity) :
                fmt::format(
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
            auto key =
                std::make_tuple(issue.channelId_, issue.expression_, issue.scope_, issue.message_);
            auto [found, inserted] = issues.emplace(std::move(key), issue);
            if (!inserted) {
                ++found->second.occurrenceCount_;
            }
        }
        relationDescriptors = std::move(resolvedDescriptors);
    }

    if (limitingLifetime) {
        // Preserve the limiting source's original positive TTL pair. Deriving a
        // duration from the output timestamp could turn an already-expired
        // dependency into a negative TTL, which means non-expiring on the wire.
        ready.layer_->setTimestamp(limitingLifetime->timestamp_);
        ready.layer_->setTtl(limitingLifetime->ttl_);
    }
    else {
        ready.layer_->setTtl(std::nullopt);
    }
    ready.layer_->setDependencies(std::move(dependencies));
    if (hasPointGroups) {
        auto const startedAt = std::chrono::steady_clock::now();
        auto completion = request->filter_.completePointGroups(
            *ready.layer_,
            members,
            [request = this->request] { return request->isCancelled(); });
        if (!completion) {
            return tl::unexpected(completion.error());
        }
        if (request->isCancelled()) {
            return size_t{0};
        }
        for (auto&& issue : completion->issues_) {
            auto key =
                std::make_tuple(issue.channelId_, issue.expression_, issue.scope_, issue.message_);
            auto [found, inserted] = issues.emplace(key, issue);
            if (!inserted) {
                auto const remaining = std::numeric_limits<uint64_t>::max() -
                    found->second.occurrenceCount_;
                found->second.occurrenceCount_ += std::min(remaining, issue.occurrenceCount_);
            }
        }
        mergeFilterTraces(traces, std::move(completion->traces_));
        ready.layer_->setInfo(
            "Filter/Process-Groups#ms",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt)
                .count());
    }
    if (hasStoredRelations) {
        std::vector<MapTileKey> requestedOutputKeys;
        requestedOutputKeys.reserve(outputs.size());
        for (auto const& output : outputs) {
            requestedOutputKeys.emplace_back(
                LayerType::Features,
                request->mapId_,
                request->layerId_,
                output.tileId_);
        }
        auto const startedAt = std::chrono::steady_clock::now();
        auto completion = request->filter_.completeRelations(
            *ready.layer_,
            relationDescriptors,
            requestedOutputKeys,
            request->exactRoots_,
            [request = this->request] { return request->isCancelled(); });
        if (!completion) {
            return tl::unexpected(completion.error());
        }
        if (request->isCancelled()) {
            return size_t{0};
        }
        for (auto&& issue : completion->issues_) {
            auto key =
                std::make_tuple(issue.channelId_, issue.expression_, issue.scope_, issue.message_);
            auto [found, inserted] = issues.emplace(key, issue);
            if (!inserted) {
                auto const remaining = std::numeric_limits<uint64_t>::max() -
                    found->second.occurrenceCount_;
                found->second.occurrenceCount_ += std::min(remaining, issue.occurrenceCount_);
            }
        }
        mergeFilterTraces(traces, std::move(completion->traces_));
        auto const priorMilliseconds =
            ready.layer_->info().value("Filter/Process-Relations#ms", 0.0);
        ready.layer_->setInfo(
            "Filter/Process-Relations#ms",
            priorMilliseconds +
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - startedAt)
                    .count());
    }

    for (auto&& [key, issue] : issues) {
        ready.layer_->addIssue(std::move(issue));
    }
    ready.layer_->setTraces(std::move(traces));
    ready.layer_->setDiagnostics(diagnostics);

    size_t entryCount = 0;
    ready.layer_->forEachChannel(
        [&](model_ptr<TileSubsetChannel> const& channel)
        {
            entryCount += channel->entryCount();
            return true;
        });
    if (request->isCancelled()) {
        return size_t{0};
    }
    auto const finalLayerBytes = ready.layer_->memoryUsage().total().allocatedBytes;
    if (finalLayerBytes > ready.layerBytes_) {
        memory->outputSubsetModels.add(finalLayerBytes - ready.layerBytes_);
    }
    else {
        memory->outputSubsetModels.subtract(ready.layerBytes_ - finalLayerBytes);
    }
    request->notifyResult(std::move(ready.layer_));
    memory->outputSubsetModels.subtract(finalLayerBytes);
    return entryCount;
}

bool FilterRequestExecution::emitCompletedOutputs(std::vector<ReadyOutput> ready)
{
    for (auto&& output : ready) {
        auto entryCount = finalizeOutput(std::move(output));
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
                entriesEmitted += *entryCount;
            }
        }
        if (stopped) {
            return false;
        }
        emitProgress("OutputTileEmitted");
    }
    return true;
}

void FilterRequestExecution::evaluate(
    size_t sourceIndex,
    uint64_t sourceBytes,
    TileFeatureLayer::Ptr source)
{
    bool evaluationJobPending = true;
    uint64_t trackedOutputModelBytes = 0;
    uint64_t trackedTemporaryBytes = 0;
    bool outputModelTransferred = false;
    auto releaseUntransferredOutputModel = [&]()
    {
        if (!outputModelTransferred && trackedOutputModelBytes) {
            memory->outputSubsetModels.subtract(trackedOutputModelBytes);
            trackedOutputModelBytes = 0;
        }
    };
    auto releaseTemporary = [&]()
    {
        if (trackedTemporaryBytes) {
            memory->evaluationTemporaries.subtract(trackedTemporaryBytes);
            trackedTemporaryBytes = 0;
        }
    };
    auto replaceTemporary = [&](uint64_t replacement)
    {
        if (replacement >= trackedTemporaryBytes) {
            memory->evaluationTemporaries.add(replacement - trackedTemporaryBytes);
        }
        else {
            memory->evaluationTemporaries.subtract(trackedTemporaryBytes - replacement);
        }
        trackedTemporaryBytes = replacement;
    };
    auto finishEvaluationJob = [&]()
    {
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

        auto const filterStartedAt = std::chrono::steady_clock::now();
        auto effectiveFilter = request->filter_;
        if (auto const deliveryEpoch = request->deliveryEpochs_.find(source->tileId());
            deliveryEpoch != request->deliveryEpochs_.end())
        {
            effectiveFilter.deliveryEpoch_ = deliveryEpoch->second;
        }
        auto sourceResult = effectiveFilter.filterSource(
            *source,
            outputIndexByTile.contains(source->tileId()),
            request->exactRoots_,
            [request = this->request] { return request->isCancelled(); });
        if (!sourceResult) {
            finishEvaluationJob();
            fail(sourceResult.error());
            return;
        }
        replaceTemporary(sourceResultAuxiliaryBytes(*sourceResult));
        trackedOutputModelBytes = sourceResult->layer_ ?
            sourceResult->layer_->memoryUsage().total().allocatedBytes :
            uint64_t{0};
        if (trackedOutputModelBytes) {
            memory->outputSubsetModels.add(trackedOutputModelBytes);
        }
        if (sourceResult->layer_) {
            sourceResult->layer_->setInfo(
                "Filter/Process-Entries#ms",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - filterStartedAt)
                    .count());
        }
        if (request->isCancelled()) {
            releaseTemporary();
            releaseUntransferredOutputModel();
            finishEvaluationJob();
            finishIfComplete();
            return;
        }
        auto const relationStartedAt = std::chrono::steady_clock::now();
        auto locatedRelations = locateRelationTargets(*source, *sourceResult);
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
        if (sourceResult->layer_ && !sourceResult->relationDescriptors_.empty()) {
            sourceResult->layer_->setInfo(
                "Filter/Process-Relations#ms",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - relationStartedAt)
                    .count());
        }

        auto ready =
            commitSource(sourceIndex, *source, trackedOutputModelBytes, std::move(*sourceResult));
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

        auto prepared = prepareRelationOutputs(std::move(*ready));
        if (!prepared) {
            finishEvaluationJob();
            fail(prepared.error());
            return;
        }
        for (auto const& targetKey : prepared->targetsToSchedule_) {
            scheduleRelationTarget(targetKey);
        }
        if (!emitCompletedOutputs(std::move(prepared->ready_))) {
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
        auto const sourceTileId = source ? source->tileId() : TileId{};
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

void FilterRequestExecution::childFinished(RequestStatus status)
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

}  // namespace detail

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
    for (auto const& priorityTileId : priorityTileIds_) {
        if (!seenTileIds.contains(priorityTileId)) {
            raise("Priority tile IDs must be contained in the request tile IDs.");
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

bool detail::FilterRequestExecution::start(
    Service::Impl& service,
    FeatureLayerFilterTilesRequest::Ptr const& request,
    std::optional<AuthHeaders> const& clientHeaders)
{
    if (!request) {
        raise("Attempt to request a null FeatureLayerFilterTilesRequest.");
    }
    if (request->isDone()) {
        request->notifyStatus();
        return true;
    }

    auto context = service.resolveLayerRequest(
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

    std::shared_ptr<DataSourceInfo const> sourceInfo;
    DataSource::Ptr sourceDataSource;
    for (auto const& source :
         service.dataSources_
             .matchingPrimarySources(request->mapId_, request->layerId_, request->sourceId_))
    {
        if (clientHeaders && !source->dataSource->isDataSourceAuthorized(*clientHeaders)) {
            continue;
        }
        sourceInfo = source->info;
        sourceDataSource = source->dataSource;
        break;
    }
    if (!sourceInfo) {
        request->setStatus(RequestStatus::NoDataSource);
        return false;
    }

    auto const hasPointGroups = std::ranges::any_of(
        request->filter_.channels_,
        [](auto const& channel) { return channel.group_.has_value(); });
    auto const hasStoredRelations = std::ranges::any_of(
        request->filter_.channels_,
        [](auto const& channel) { return channel.scope_ == FeatureLayerFilterScope::Relation; });
    if (request->exactRoots_.size() > 4096) {
        auto status = makeFilterStatusJson(*request, "Failed");
        status["error"] = "Stored-relation traversal exceeded the initial 4096 exact-root limit.";
        request->notifyProgress(status);
        request->setStatus(RequestStatus::Aborted);
        return false;
    }
    for (size_t rootIndex = 0; rootIndex < request->exactRoots_.size(); ++rootIndex) {
        request->exactRoots_[rootIndex].requestOrdinal_ = rootIndex;
    }
    if (!request->exactRoots_.empty() && !hasStoredRelations) {
        auto status = makeFilterStatusJson(*request, "Failed");
        status["error"] = "Exact roots are valid only for a relation-scope filter bundle.";
        request->notifyProgress(status);
        request->setStatus(RequestStatus::Aborted);
        return false;
    }
    auto const requestedOutputMembership =
        std::set<TileId>(request->tileIds_.begin(), request->tileIds_.end());
    if (std::ranges::any_of(
            request->exactRoots_,
            [&](auto const& root) { return !requestedOutputMembership.contains(root.tileId_); }))
    {
        auto status = makeFilterStatusJson(*request, "Failed");
        status["error"] =
            "Every exact relation root must belong to an original requested output tile.";
        request->notifyProgress(status);
        request->setStatus(RequestStatus::Aborted);
        return false;
    }

    std::vector<TileId> tileIdsToProcess = request->tileIds_;
    std::set<TileId> sourceTileMembership(tileIdsToProcess.begin(), tileIdsToProcess.end());
    if (hasPointGroups) {
        auto const level = request->tileIds_.front().level();
        for (auto const& tileId : request->tileIds_) {
            if (!tileId.isValid() || tileId.level() != level) {
                auto status = makeFilterStatusJson(*request, "Failed");
                status["error"] = "Point-grid outputs must be valid tiles at one common level.";
                request->notifyProgress(status);
                request->setStatus(RequestStatus::Aborted);
                return false;
            }
        }
        auto const [tileWidth, tileHeight] = request->tileIds_.front().wgs84Size();
        for (auto const& channel : request->filter_.channels_) {
            if (!channel.group_) {
                continue;
            }
            if (!std::isfinite(channel.group_->cellSize_.x) ||
                !std::isfinite(channel.group_->cellSize_.y) || channel.group_->cellSize_.x <= 0.0 ||
                channel.group_->cellSize_.y <= 0.0 || channel.group_->cellSize_.x > tileWidth ||
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
                    auto const sourceTileId = outputTileId.neighbour(offsetX, offsetY);
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
        std::vector<TileId>(request->priorityTileIds_.begin(), request->priorityTileIds_.end()));
    childRequest->sourceId_ = request->sourceId_;
    {
        std::lock_guard lock(request->childRequestsMutex_);
        request->childRequests_.push_back(childRequest);
    }

    auto state =
        std::shared_ptr<detail::FilterRequestExecution>(new detail::FilterRequestExecution());
    state->impl = &service;
    state->request = request;
    state->sourceInfo = std::move(sourceInfo);
    state->sourceDataSource = std::move(sourceDataSource);
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
    state->configure(request->tileIds_, tileIdsToProcess);
    state->memory->sampleOrchestration =
        [weakState = std::weak_ptr<detail::FilterRequestExecution>{state}]
    {
        auto active = weakState.lock();
        if (!active) {
            return uint64_t{0};
        }
        std::lock_guard lock(active->mutex);
        return active->orchestrationBytesLocked();
    };
    service.scheduler_.addFilterMemoryTracker(state->memory);

    childRequest->onFeatureLayer(
        [state](TileFeatureLayer::Ptr layer) { state->collect(std::move(layer)); });
    childRequest->onDone_ = [state](RequestStatus status)
    {
        state->childFinished(status);
    };

    request->notifyProgress(state->progress("Open"));

    return service.requestTiles(std::vector<LayerTilesRequest::Ptr>{childRequest}, clientHeaders);
}

bool Service::Impl::requestFilter(
    FeatureLayerFilterTilesRequest::Ptr const& request,
    std::optional<AuthHeaders> const& clientHeaders)
{
    return detail::FilterRequestExecution::start(*this, request, clientHeaders);
}

}  // namespace mapget
