#include "service-filter.h"

#include "fmt/format.h"
#include "mapget/log.h"
#include "mapget/model/featureid.h"
#include "mapget/service/locate.h"
#include "service-impl.h"
#include "service-memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iterator>
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

bool FilterRequestExecution::SourceTileContribution::Lifetime::expiresBefore(Lifetime const& other)
    const
{
    return expiresAt() < other.expiresAt() ||
        (expiresAt() == other.expiresAt() && sourceKey_ < other.sourceKey_);
}

void FilterRequestExecution::SourceTileContribution::addMemoryUsage(MemoryUsageBreakdown& usage)
    const
{
    usage.add("point-group-members", vectorMemoryUsage(pointGroupMembers_));
    for (auto const& member : pointGroupMembers_) {
        if (member.geometryName_) {
            usage.add("point-group-geometry-names", stringMemoryUsage(*member.geometryName_));
        }
    }
    usage.add("relation-descriptors", vectorMemoryUsage(relationDescriptors_));
    usage.add("issues", vectorMemoryUsage(issues_));
    for (auto const& issue : issues_) {
        usage.add("issue-strings", stringMemoryUsage(issue.channelId_));
        usage.add("issue-strings", stringMemoryUsage(issue.expression_));
        usage.add("issue-strings", stringMemoryUsage(issue.message_));
    }
    usage.add("diagnostics", diagnosticsMemoryUsage(diagnostics_));
    usage.add(
        "trace-index",
        {
            traces_.size() * sizeof(decltype(traces_)::value_type),
            traces_.size() * (sizeof(decltype(traces_)::value_type) + 3 * sizeof(void*)),
        });
    for (auto const& [name, trace] : traces_) {
        usage.add("trace-names", stringMemoryUsage(name));
        usage.add("trace-values", vectorMemoryUsage(trace.values));
    }
}

void FilterRequestExecution::OutputTileState::addMemoryUsage(MemoryUsageBreakdown& usage) const
{
    usage.add("output-source-ids", vectorMemoryUsage(sourceTileIds_));
    usage.add("output-contributions", vectorMemoryUsage(contributions_));
    for (auto const& contribution : contributions_) {
        if (contribution) {
            contribution->addMemoryUsage(usage);
        }
    }
}

void FilterRequestExecution::ReadyOutput::addIssue(FilterIssue issue)
{
    auto const duplicate = std::ranges::find_if(
        issues_,
        [&](FilterIssue const& existing)
        {
            return existing.channelId_ == issue.channelId_ &&
                existing.expression_ == issue.expression_ && existing.scope_ == issue.scope_ &&
                existing.message_ == issue.message_;
        });
    if (duplicate == issues_.end()) {
        issues_.push_back(std::move(issue));
        return;
    }
    auto const remaining = std::numeric_limits<uint64_t>::max() - duplicate->occurrenceCount_;
    duplicate->occurrenceCount_ += std::min(remaining, issue.occurrenceCount_);
}

void FilterRequestExecution::ReadyOutput::addIssues(std::vector<FilterIssue> issues)
{
    for (auto&& issue : issues) {
        addIssue(std::move(issue));
    }
}

void FilterRequestExecution::ReadyOutput::addTraces(std::map<std::string, simfil::Trace> traces)
{
    for (auto&& [name, trace] : traces) {
        traces_[name].append(std::move(trace));
    }
}

void FilterRequestExecution::ReadyOutput::installMetadata()
{
    std::ranges::sort(
        issues_,
        [](FilterIssue const& left, FilterIssue const& right)
        {
            return std::tie(left.channelId_, left.expression_, left.scope_, left.message_) <
                std::tie(right.channelId_, right.expression_, right.scope_, right.message_);
        });
    for (auto&& issue : issues_) {
        layer_->addIssue(std::move(issue));
    }
    layer_->setTraces(std::move(traces_));
}

void FilterRequestExecution::ReadyOutput::considerLifetime(
    std::optional<SourceTileContribution::Lifetime> const& lifetime)
{
    if (lifetime && (!limitingLifetime_ || lifetime->expiresBefore(*limitingLifetime_))) {
        limitingLifetime_ = lifetime;
    }
}

void FilterRequestExecution::ReadyOutput::addMemoryUsage(MemoryUsageBreakdown& usage) const
{
    usage.add("ready-contributions", vectorMemoryUsage(contributions_));
    for (auto const& contribution : contributions_) {
        contribution.addMemoryUsage(usage);
    }
    usage.add(
        "dynamic-contributions",
        {
            dynamicContributions_.size() * sizeof(decltype(dynamicContributions_)::value_type),
            dynamicContributions_.size() *
                (sizeof(decltype(dynamicContributions_)::value_type) + 3 * sizeof(void*)),
        });
    for (auto const& [_, contribution] : dynamicContributions_) {
        contribution.addMemoryUsage(usage);
    }
    usage.add("ready-issues", vectorMemoryUsage(issues_));
    for (auto const& issue : issues_) {
        usage.add("issue-strings", stringMemoryUsage(issue.channelId_));
        usage.add("issue-strings", stringMemoryUsage(issue.expression_));
        usage.add("issue-strings", stringMemoryUsage(issue.message_));
    }
    usage.add(
        "ready-trace-index",
        {
            traces_.size() * sizeof(decltype(traces_)::value_type),
            traces_.size() * (sizeof(decltype(traces_)::value_type) + 3 * sizeof(void*)),
        });
    for (auto const& [name, trace] : traces_) {
        usage.add("trace-names", stringMemoryUsage(name));
        usage.add("trace-values", vectorMemoryUsage(trace.values));
    }
}

void FilterRequestExecution::PendingRelationOutput::addMemoryUsage(MemoryUsageBreakdown& usage)
    const
{
    ready_.addMemoryUsage(usage);
    usage.add(
        "relation-output-targets",
        {
            (targetTiles_.size() + pendingTargetTiles_.size()) * sizeof(MapTileKey),
            (targetTiles_.size() + pendingTargetTiles_.size()) *
                (sizeof(MapTileKey) + 3 * sizeof(void*)),
        });
}

void FilterRequestExecution::RelationTargetTileState::addMemoryUsage(MemoryUsageBreakdown& usage)
    const
{
    usage.add(
        "relation-target-dependents",
        {
            dependentOutputs_.size() * sizeof(size_t),
            dependentOutputs_.size() * (sizeof(size_t) + 3 * sizeof(void*)),
        });
    if (failureMessage_) {
        usage.add("relation-target-errors", stringMemoryUsage(*failureMessage_));
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
    {
        std::lock_guard lock(request->outputMembershipMutex_);
        usage.add(
            "live-output-index",
            {
                request->liveOutputTileIds_.size() * sizeof(TileId),
                request->liveOutputTileIds_.size() * (sizeof(TileId) + 3 * sizeof(void*)),
            });
    }
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
    usage.add("simfil-expression-cache", expressionCache.memoryUsage());
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
        output.addMemoryUsage(usage);
    }
    usage.add("dependent-output-lists", vectorMemoryUsage(dependentOutputsBySource));
    for (auto const& dependents : dependentOutputsBySource) {
        usage.add("dependent-output-slots", vectorMemoryUsage(dependents));
    }
    usage.add(
        "live-dependent-output-counts",
        {
            dependentOutputsBySource.size() * sizeof(std::atomic_size_t),
            dependentOutputsBySource.size() * sizeof(std::atomic_size_t),
        });
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
    for (auto const& [_, pending] : pendingRelationOutputs) {
        pending.addMemoryUsage(usage);
    }
    usage.add(
        "relation-target-tiles",
        {
            relationTargetTiles.size() * sizeof(decltype(relationTargetTiles)::value_type),
            relationTargetTiles.size() *
                (sizeof(decltype(relationTargetTiles)::value_type) + 3 * sizeof(void*)),
        });
    for (auto const& [_, target] : relationTargetTiles) {
        target.addMemoryUsage(usage);
    }
    {
        std::lock_guard cacheLock(relationSelectorCacheMutex);
        usage.add(
            "relation-selector-cache",
            {
                relationSelectorCache.size() * sizeof(decltype(relationSelectorCache)::value_type),
                relationSelectorCache.size() *
                    (sizeof(decltype(relationSelectorCache)::value_type) + 3 * sizeof(void*)),
            });
        for (auto const& [key, resolution] : relationSelectorCache) {
            usage.add("relation-selector-keys", stringMemoryUsage(key));
            if (*resolution) {
                usage.add("relation-selector-results", vectorMemoryUsage(resolution->value()));
            }
            else {
                usage.add(
                    "relation-selector-errors",
                    stringMemoryUsage(resolution->error().message));
            }
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

        OutputTileState output;
        output.tileId_ = outputTileId;
        if (hasPointGroups) {
            std::vector<std::pair<size_t, TileId>> dependenciesBySourceIndex;
            dependenciesBySourceIndex.reserve(9);
            for (int32_t offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int32_t offsetX = -1; offsetX <= 1; ++offsetX) {
                    auto const sourceTileId = outputTileId.neighbour(offsetX, offsetY);
                    // Sorting nine direct lookups by source index preserves
                    // processing order without scanning the complete source
                    // union once for every output.
                    dependenciesBySourceIndex
                        .emplace_back(sourceIndexByTile.at(sourceTileId), sourceTileId);
                }
            }
            std::ranges::sort(
                dependenciesBySourceIndex,
                {},
                [](auto const& dependency) { return dependency.first; });
            std::optional<size_t> previousSourceIndex;
            output.sourceTileIds_.reserve(dependenciesBySourceIndex.size());
            for (auto const& [sourceIndex, sourceTileId] : dependenciesBySourceIndex) {
                // Wrapped neighbours coincide at the lowest tile levels.
                if (previousSourceIndex == sourceIndex) {
                    continue;
                }
                previousSourceIndex = sourceIndex;
                output.sourceTileIds_.push_back(sourceTileId);
            }
        }
        else {
            output.sourceTileIds_.push_back(outputTileId);
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

    liveDependentOutputsBySource =
        std::make_unique<std::atomic_size_t[]>(dependentOutputsBySource.size());
    for (size_t sourceIndex = 0; sourceIndex < dependentOutputsBySource.size(); ++sourceIndex) {
        liveDependentOutputsBySource[sourceIndex]
            .store(dependentOutputsBySource[sourceIndex].size(), std::memory_order_relaxed);
    }
}

bool FilterRequestExecution::outputLive(size_t outputIndex)
{
    std::lock_guard lock(mutex);
    if (outputIndex >= outputs.size()) {
        return false;
    }
    auto const state = outputs[outputIndex].state_;
    return !terminal &&
        (state == OutputTileState::State::Pending || state == OutputTileState::State::Taken);
}

bool FilterRequestExecution::sourceNeeded(size_t sourceIndex) const
{
    return sourceIndex < dependentOutputsBySource.size() && liveDependentOutputsBySource &&
        liveDependentOutputsBySource[sourceIndex].load(std::memory_order_acquire) != 0;
}

void FilterRequestExecution::releaseOutputDependenciesLocked(OutputTileState const& output)
{
    for (auto const& sourceTileId : output.sourceTileIds_) {
        auto& count = liveDependentOutputsBySource[sourceIndexByTile.at(sourceTileId)];
        auto const previous = count.load(std::memory_order_relaxed);
        if (previous == 0) {
            raise("Filter source dependency was released more than once.");
        }
        count.store(previous - 1, std::memory_order_release);
    }
}

void FilterRequestExecution::releaseReadyOutput(ReadyOutput& output)
{
    if (output.layer_) {
        memory->outputSubsetModels.subtract(output.layerBytes_);
        output.layer_.reset();
        output.layerBytes_ = 0;
    }
}

[[nodiscard]] nlohmann::json FilterRequestExecution::progress(std::string state)
{
    std::lock_guard lock(mutex);
    auto status = makeFilterStatusJson(*request, std::move(state));
    status["outputTilesRequested"] = outputs.size() - prunedOutputTiles;
    status["sourceTilesQueued"] = sourceTileIds.size();
    status["sourceTilesLoaded"] = loadedSourceTiles;
    status["sourceTilesEvaluated"] = evaluatedSourceTiles;
    status["outputTilesReady"] = readyOutputTiles;
    status["outputTilesEmitted"] = emittedOutputTiles;
    status["entriesEmitted"] = entriesEmitted;
    auto const expressions = expressionCache.statistics();
    status["simfilExpressions"] = {
        {"entries", expressions.entries},
        {"hits", expressions.hits},
        {"misses", expressions.misses},
        {"compiles", expressions.compiles},
        {"failedCompiles", expressions.failedCompiles},
        {"compileMicroseconds", expressions.compileMicroseconds},
    };
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
    {
        std::lock_guard lock(request->childRequestsMutex_);
        request->childRequests_.clear();
    }
    std::lock_guard lock(mutex);
    sourceRequest.reset();
    for (auto& [_, target] : relationTargetTiles) {
        target.childRequest_.reset();
    }
}

void FilterRequestExecution::abortChildRequests()
{
    std::vector<LayerTilesRequest::Ptr> children;
    {
        std::lock_guard lock(request->childRequestsMutex_);
        children = request->childRequests_;
        request->childRequests_.clear();
    }
    {
        std::lock_guard lock(mutex);
        sourceRequest.reset();
        for (auto& [_, target] : relationTargetTiles) {
            target.childRequest_.reset();
        }
    }
    for (auto const& child : children) {
        if (child && !child->isDone()) {
            impl->scheduler_.abortRequest(child);
        }
    }
}

void FilterRequestExecution::cancel()
{
    {
        std::lock_guard lock(mutex);
        terminal = true;
        sourceChildDetached = true;
    }
    abortChildRequests();
}

void FilterRequestExecution::retainOutputs(std::set<TileId> const& retainedTileIds)
{
    if (request->isCancelled()) {
        return;
    }
    auto const [hasLiveOutputs, membershipChanged] = request->retainOutputTileIds(retainedTileIds);
    if (!hasLiveOutputs || !membershipChanged) {
        return;
    }

    std::set<TileId> retainedSourceTileIds;
    std::vector<LayerTilesRequest::Ptr> childrenToAbort;
    LayerTilesRequest::Ptr sourceRequestToPrune;
    bool detachSourceChild = false;
    {
        std::lock_guard lock(mutex);
        if (terminal) {
            return;
        }

        for (size_t outputIndex = 0; outputIndex < outputs.size(); ++outputIndex) {
            auto& output = outputs[outputIndex];
            if (retainedTileIds.contains(output.tileId_) ||
                output.state_ == OutputTileState::State::Emitted ||
                output.state_ == OutputTileState::State::Pruned)
            {
                continue;
            }

            // Taken outputs released these counters when their final source
            // contribution arrived. Only a Pending -> Pruned transition
            // removes a still-live source dependency here.
            if (output.state_ == OutputTileState::State::Pending) {
                releaseOutputDependenciesLocked(output);
            }
            output.state_ = OutputTileState::State::Pruned;
            ++prunedOutputTiles;
            if (output.wipSubset_) {
                memory->outputSubsetModels.subtract(output.wipSubsetBytes_);
                output.wipSubset_.reset();
                output.wipSubsetBytes_ = 0;
            }
            decltype(output.sourceTileIds_){}.swap(output.sourceTileIds_);
            decltype(output.contributions_){}.swap(output.contributions_);

            // Relation-finalization state owns the moved WIP model after the
            // fixed source contributions have completed.
            if (auto pending = pendingRelationOutputs.find(outputIndex);
                pending != pendingRelationOutputs.end())
            {
                releaseReadyOutput(pending->second.ready_);
                pendingRelationOutputs.erase(pending);
            }
        }

        for (auto target = relationTargetTiles.begin(); target != relationTargetTiles.end();) {
            std::erase_if(
                target->second.dependentOutputs_,
                [&](size_t outputIndex)
                {
                    return outputIndex >= outputs.size() ||
                        outputs[outputIndex].state_ == OutputTileState::State::Pruned;
                });
            if (!target->second.dependentOutputs_.empty()) {
                ++target;
                continue;
            }
            if (target->second.layer_) {
                memory->relationTargetModels
                    .subtract(target->second.layer_->memoryUsage().total().allocatedBytes);
            }
            if (target->second.childRequest_ && !target->second.childRequest_->isDone()) {
                childrenToAbort.push_back(target->second.childRequest_);
            }
            target = relationTargetTiles.erase(target);
        }

        for (size_t sourceIndex = 0; sourceIndex < sourceTileIds.size(); ++sourceIndex) {
            if (sourceNeeded(sourceIndex)) {
                retainedSourceTileIds.insert(sourceTileIds[sourceIndex]);
            }
        }
        if (sourceRequest && !sourceRequest->isDone() && retainedSourceTileIds.empty()) {
            // Every retained output already owns complete source contributions.
            // Remaining source callbacks are obsolete and must not turn their
            // deliberate abort into a parent request failure.
            sourceChildDetached = true;
            childRequestDone = true;
            detachSourceChild = true;
        }
        sourceRequestToPrune = sourceRequest;
    }

    // releaseChildRequests() may clear execution-owned child pointers as soon
    // as another callback completes the request. Keep a local shared owner for
    // all scheduler calls performed outside the coordination mutex.
    if (sourceRequestToPrune && !sourceRequestToPrune->isDone()) {
        if (detachSourceChild) {
            impl->scheduler_.abortRequest(sourceRequestToPrune);
        }
        else if (!retainedSourceTileIds.empty()) {
            impl->scheduler_.retainRequestOutputs(sourceRequestToPrune, retainedSourceTileIds);
        }
    }
    for (auto const& child : childrenToAbort) {
        impl->scheduler_.abortRequest(child);
    }
    if (!childrenToAbort.empty() || detachSourceChild) {
        std::lock_guard lock(request->childRequestsMutex_);
        std::erase_if(
            request->childRequests_,
            [&](LayerTilesRequest::Ptr const& child)
            {
                return (detachSourceChild && child == sourceRequestToPrune) ||
                    std::ranges::find(childrenToAbort, child) != childrenToAbort.end();
            });
    }

    emitProgress("OutputsPruned", true);
    finishIfComplete();
}

void FilterRequestExecution::finishIfComplete()
{
    if (request->isCancelled()) {
        return;
    }
    RequestStatus finalStatus = RequestStatus::Open;
    {
        std::lock_guard lock(mutex);
        if (terminal || !childRequestDone || activeEvaluations != 0 ||
            emittedOutputTiles + prunedOutputTiles < outputs.size())
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
    if (request->isCancelled()) {
        return;
    }
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
        if (!sourceNeeded(sourceIndex)) {
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
            ++activeEvaluations;
        }
    }
    if (duplicate) {
        fail(simfil::Error{
            simfil::Error::InternalError,
            "Filter source tile was delivered more than once.",
        });
        return;
    }

    // The TileLoadJob has released its datasource permit before invoking this
    // callback. Keep the source model on this worker through evaluation so
    // loaded tiles never accumulate in a second scheduler queue.
    auto const sourceBytes = layer->memoryUsage().total().allocatedBytes;
    memory->sourceTileModels.add(sourceBytes);
    emitProgress("SourceTileLoaded");
    evaluate(sourceIndex, sourceBytes, std::move(layer));
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
            auto selected = resolveLocateCandidate(
                location,
                source,
                [request = this->request] { return request->isCancelled(); });
            if (request->isCancelled()) {
                return {};
            }
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
            return std::move(ready);
        }
        if (sourceIndex >= committedSourceTiles.size() || committedSourceTiles[sourceIndex]) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InternalError,
                "Filter source tile committed more than once.",
            });
        }

        auto outputForSource = outputIndexByTile.find(source.tileId());
        if (outputForSource != outputIndexByTile.end()) {
            auto& output = outputs[outputForSource->second];
            if (output.state_ == OutputTileState::State::Pending && !result.layer_) {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InternalError,
                    "Requested filter output source produced no WIP subset.",
                });
            }
            if (output.state_ == OutputTileState::State::Pending) {
                output.wipSubset_ = std::move(result.layer_);
                output.wipSubsetBytes_ = outputModelBytes;
            }
            else if (result.layer_) {
                // Pruning may race an already-running SIMFIL evaluation. The
                // produced model is intentionally discarded at this commit
                // boundary and its request-memory gauge is released here.
                memory->outputSubsetModels.subtract(outputModelBytes);
                result.layer_.reset();
            }
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
            if (output.state_ != OutputTileState::State::Pending) {
                continue;
            }
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
                releaseOutputDependenciesLocked(output);
                output.state_ = OutputTileState::State::Taken;
                ReadyOutput item{
                    dependent.outputIndex_,
                    std::move(output.wipSubset_),
                    output.wipSubsetBytes_,
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
                // A completed output receives no further source writes. Drop
                // its fixed dependency storage instead of retaining every
                // moved-from slot until the whole viewport has completed.
                decltype(output.sourceTileIds_){}.swap(output.sourceTileIds_);
                decltype(output.contributions_){}.swap(output.contributions_);
                ready.push_back(std::move(item));
            }
        }

        committedSourceTiles[sourceIndex] = true;
        ++evaluatedSourceTiles;
    }
    std::ranges::sort(ready, {}, &ReadyOutput::outputIndex_);
    return std::move(ready);
}

tl::expected<void, simfil::Error> FilterRequestExecution::addRelationTargetContribution(
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
            std::optional<SourceTileContribution::Lifetime>{SourceTileContribution::Lifetime{
                targetKey,
                targetLayer.timestamp(),
                *targetLayer.ttl(),
            }} :
            std::nullopt,
    };

    output.dynamicContributions_.erase(targetKey);
    output.dynamicContributions_.emplace(targetKey, std::move(targetContribution));
    return {};
}

tl::expected<FilterRequestExecution::RelationReadyOutput, simfil::Error>
FilterRequestExecution::takeRelationReadyOutputLocked(size_t outputIndex)
{
    auto pending = pendingRelationOutputs.find(outputIndex);
    if (pending == pendingRelationOutputs.end() || !pending->second.pendingTargetTiles_.empty()) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InternalError,
            "A relation output was released before all target tiles became terminal.",
        });
    }

    RelationReadyOutput result(std::move(pending->second.ready_));
    result.targets_.reserve(pending->second.targetTiles_.size());
    for (auto const& targetKey : pending->second.targetTiles_) {
        auto target = relationTargetTiles.find(targetKey);
        if (target == relationTargetTiles.end() || !target->second.terminal_) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InternalError,
                "A relation output references a non-terminal target tile.",
            });
        }
        result.targets_.push_back(RelationTargetSnapshot{
            targetKey,
            target->second.layer_,
            target->second.failureMessage_,
        });
    }
    pendingRelationOutputs.erase(pending);
    ++readyOutputTiles;
    return std::move(result);
}

tl::expected<std::vector<FilterRequestExecution::ReadyOutput>, simfil::Error>
FilterRequestExecution::resolveRelationReadyOutputs(std::vector<RelationReadyOutput> ready)
{
    using SelectorConsumer =
        std::tuple<FeatureLayerRelationTargetCandidate*, FeatureLayerRelationDescriptor*, size_t>;
    using TargetBatch =
        std::tuple<TileFeatureLayer::Ptr, std::optional<std::string>, std::vector<ReadyOutput*>>;

    std::map<MapTileKey, TargetBatch> targetBatches;
    for (auto output = ready.begin(); output != ready.end();) {
        if (!outputLive(output->ready_.outputIndex_)) {
            releaseReadyOutput(output->ready_);
            output = ready.erase(output);
            continue;
        }
        for (auto const& target : output->targets_) {
            auto [batch, inserted] = targetBatches.try_emplace(
                target.key_,
                TargetBatch{target.layer_, target.failureMessage_, {}});
            auto& [layer, _, outputs] = batch->second;
            if (!inserted && layer != target.layer_) {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InternalError,
                    "One relation target tile acquired inconsistent terminal states.",
                });
            }
            outputs.push_back(&output->ready_);
        }
        ++output;
    }

    for (auto& [targetKey, batch] : targetBatches) {
        auto& [targetLayer, failureMessage, outputs] = batch;
        if (request->isCancelled()) {
            return std::vector<ReadyOutput>{};
        }
        if (!targetLayer) {
            auto const message = failureMessage.value_or(
                fmt::format("Could not load relation target tile {}.", targetKey.toString()));
            for (auto* output : outputs) {
                markRelationTargetUnavailableInOutput(*output, targetKey, message);
            }
            continue;
        }

        std::map<std::string, size_t> selectorIndexByKey;
        std::vector<std::string> selectorKeys;
        std::vector<FeatureLayerSelector> selectors;
        std::vector<SelectorConsumer> consumers;
        for (auto* output : outputs) {
            for (auto& contribution : output->contributions_) {
                for (auto& descriptor : contribution.relationDescriptors_) {
                    if (descriptor.target_) {
                        continue;
                    }
                    for (auto& candidate : descriptor.targetCandidates_) {
                        if (candidate.resolved_ || candidate.tileKey_ != targetKey) {
                            continue;
                        }
                        auto selectorKey =
                            LocateCandidate(targetKey, candidate.selector_).serialize().dump();
                        auto [found, inserted] =
                            selectorIndexByKey.try_emplace(selectorKey, selectors.size());
                        if (inserted) {
                            selectorKeys.push_back(std::move(selectorKey));
                            selectors.push_back(candidate.selector_);
                        }
                        consumers.emplace_back(&candidate, &descriptor, found->second);
                    }
                }
            }
        }

        std::vector<SharedSelectorResolution> resolutions(selectors.size());
        std::vector<size_t> missingIndexes;
        std::vector<FeatureLayerSelector> missingSelectors;
        {
            std::lock_guard cacheLock(relationSelectorCacheMutex);
            for (size_t index = 0; index < selectors.size(); ++index) {
                auto found = relationSelectorCache.find(selectorKeys[index]);
                if (found != relationSelectorCache.end()) {
                    resolutions[index] = found->second;
                    continue;
                }
                missingIndexes.push_back(index);
                missingSelectors.push_back(selectors[index]);
            }
        }

        if (!missingSelectors.empty()) {
            tl::expected<std::vector<std::vector<model_ptr<Feature>>>, simfil::Error> selected =
                tl::unexpected(simfil::Error{
                    simfil::Error::InternalError,
                    "Relation-target selector evaluation did not complete.",
                });
            try {
                selected = targetLayer->find(
                    std::span<FeatureLayerSelector const>{missingSelectors},
                    [request = this->request] { return request->isCancelled(); },
                    &expressionCache);
            }
            catch (std::exception const& exception) {
                selected = tl::unexpected(simfil::Error{
                    simfil::Error::InternalError,
                    fmt::format("Relation-target selector evaluation failed: {}", exception.what()),
                });
            }
            catch (...) {
                selected = tl::unexpected(simfil::Error{
                    simfil::Error::InternalError,
                    "Relation-target selector evaluation failed with a non-standard exception.",
                });
            }

            if (request->isCancelled()) {
                return std::vector<ReadyOutput>{};
            }

            if (selected && selected->size() != missingIndexes.size()) {
                selected = tl::unexpected(simfil::Error{
                    simfil::Error::InternalError,
                    "Batched relation-target selector results lost positional alignment.",
                });
            }

            for (size_t index = 0; index < missingIndexes.size(); ++index) {
                SharedSelectorResolution resolution;
                if (selected) {
                    resolution =
                        std::make_shared<SelectorResolution>(std::move((*selected)[index]));
                }
                else {
                    resolution =
                        std::make_shared<SelectorResolution>(tl::unexpected(selected.error()));
                }
                auto const selectorIndex = missingIndexes[index];
                // Never hold this cache mutex during SIMFIL. A concurrent miss
                // may do the same work, but whichever completes first becomes
                // the request-wide reusable result without blocking workers
                // behind another selector scan.
                std::lock_guard cacheLock(relationSelectorCacheMutex);
                auto [cached, _] = relationSelectorCache.try_emplace(
                    selectorKeys[selectorIndex],
                    std::move(resolution));
                resolutions[selectorIndex] = cached->second;
            }
        }

        if (request->isCancelled()) {
            return std::vector<ReadyOutput>{};
        }
        for (auto const& [candidate, descriptor, selectorIndex] : consumers) {
            auto const& resolution = resolutions[selectorIndex];
            if (!*resolution) {
                return tl::unexpected(resolution->error());
            }
            candidate->resolved_ = true;
            descriptor->targetMatches_.insert(
                descriptor->targetMatches_.end(),
                resolution->value().begin(),
                resolution->value().end());
        }
        for (auto* output : outputs) {
            auto added = addRelationTargetContribution(*output, targetKey, *targetLayer);
            if (!added) {
                return tl::unexpected(added.error());
            }
        }
    }

    std::vector<ReadyOutput> result;
    result.reserve(ready.size());
    for (auto&& output : ready) {
        if (outputLive(output.ready_.outputIndex_)) {
            result.push_back(std::move(output.ready_));
        }
        else {
            releaseReadyOutput(output.ready_);
        }
    }
    std::ranges::sort(result, {}, &ReadyOutput::outputIndex_);
    return std::move(result);
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
        output.addIssue(FilterIssue{
            channelId,
            "<relation-target>",
            Scope::Relation,
            failureMessage,
            count,
        });
    }
}

tl::expected<std::vector<FilterRequestExecution::ReadyOutput>, simfil::Error>
FilterRequestExecution::prepareRelationOutputs(std::vector<ReadyOutput> fixedReady)
{
    std::vector<ReadyOutput> ready;
    std::vector<RelationReadyOutput> relationReady;
    std::vector<MapTileKey> targetsToSchedule;
    {
        std::lock_guard lock(mutex);
        if (terminal || request->isCancelled()) {
            for (auto& output : fixedReady) {
                releaseReadyOutput(output);
            }
            return ready;
        }

        for (auto&& output : fixedReady) {
            if (output.outputIndex_ >= outputs.size() ||
                outputs[output.outputIndex_].state_ != OutputTileState::State::Taken)
            {
                releaseReadyOutput(output);
                continue;
            }
            std::set<MapTileKey> targetKeys;
            for (auto const& contribution : output.contributions_) {
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

            if (targetKeys.empty()) {
                ready.push_back(std::move(output));
                ++readyOutputTiles;
                continue;
            }

            std::set<MapTileKey> pendingKeys;
            for (auto const& targetKey : targetKeys) {
                auto [targetState, inserted] = relationTargetTiles.try_emplace(targetKey);
                if (inserted && relationTargetTiles.size() > 2048) {
                    return tl::unexpected(simfil::Error{
                        simfil::Error::InvalidArguments,
                        "Stored-relation traversal exceeded the initial 2048 unique-target-tile "
                        "limit.",
                    });
                }
                if (targetState->second.terminal_) {
                    continue;
                }
                pendingKeys.insert(targetKey);
                targetState->second.dependentOutputs_.insert(output.outputIndex_);
                if (!targetState->second.scheduled_) {
                    targetState->second.scheduled_ = true;
                    targetsToSchedule.push_back(targetKey);
                }
            }

            auto [pending, inserted] = pendingRelationOutputs.emplace(
                output.outputIndex_,
                PendingRelationOutput{
                    std::move(output),
                    std::move(targetKeys),
                    std::move(pendingKeys),
                });
            if (!inserted) {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InternalError,
                    "Filter output entered relation-target resolution more than once.",
                });
            }
            if (pending->second.pendingTargetTiles_.empty()) {
                auto resolved = takeRelationReadyOutputLocked(pending->first);
                if (!resolved) {
                    return tl::unexpected(resolved.error());
                }
                relationReady.push_back(std::move(*resolved));
            }
        }
    }

    // Child requests can complete synchronously, so scheduling must happen
    // only after relation coordination state is fully published and unlocked.
    for (auto const& targetKey : targetsToSchedule) {
        scheduleRelationTarget(targetKey);
    }
    auto resolved = resolveRelationReadyOutputs(std::move(relationReady));
    if (!resolved) {
        return tl::unexpected(resolved.error());
    }
    ready.insert(
        ready.end(),
        std::make_move_iterator(resolved->begin()),
        std::make_move_iterator(resolved->end()));
    std::ranges::sort(ready, {}, &ReadyOutput::outputIndex_);
    return ready;
}

void FilterRequestExecution::scheduleRelationTarget(MapTileKey const& targetKey)
{
    {
        std::lock_guard lock(mutex);
        auto target = relationTargetTiles.find(targetKey);
        if (terminal || target == relationTargetTiles.end() ||
            target->second.dependentOutputs_.empty()) {
            return;
        }
    }
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
        std::lock_guard lock(mutex);
        auto target = relationTargetTiles.find(targetKey);
        if (terminal || target == relationTargetTiles.end() ||
            target->second.dependentOutputs_.empty()) {
            return;
        }
        target->second.childRequest_ = child;
    }
    {
        std::lock_guard lock(request->childRequestsMutex_);
        // Abort clears children under the same mutex. Refuse late ownership so
        // a cancelled request cannot regain a child/callback reference cycle.
        if (request->isCancelled()) {
            std::lock_guard executionLock(mutex);
            if (auto target = relationTargetTiles.find(targetKey);
                target != relationTargetTiles.end() && target->second.childRequest_ == child)
            {
                target->second.childRequest_.reset();
            }
            return;
        }
        request->childRequests_.push_back(child);
    }
    auto const accepted =
        impl->requestTiles(std::vector<LayerTilesRequest::Ptr>{child}, clientHeaders);
    // If cancellation cleared child ownership immediately after insertion,
    // close the narrow enqueue race explicitly. Otherwise a now-unowned child
    // could continue loading after its filter generation was cancelled.
    if (request->isCancelled()) {
        impl->scheduler_.abortRequest(child);
        return;
    }
    bool targetStillNeeded = false;
    {
        std::lock_guard lock(mutex);
        auto target = relationTargetTiles.find(targetKey);
        targetStillNeeded = target != relationTargetTiles.end() &&
            target->second.childRequest_ == child && !target->second.dependentOutputs_.empty();
    }
    if (!targetStillNeeded) {
        impl->scheduler_.abortRequest(child);
        return;
    }
    if (!accepted) {
        completeUnavailableRelationTarget(
            targetKey,
            fmt::format("Could not schedule relation target tile {}.", targetKey.toString()));
    }
}

void FilterRequestExecution::completeUnavailableRelationTarget(
    MapTileKey const& targetKey,
    std::string message)
{
    if (request->isCancelled()) {
        return;
    }
    std::vector<RelationReadyOutput> relationReady;
    std::optional<simfil::Error> error;
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
            pending->second.pendingTargetTiles_.erase(targetKey);
            if (pending->second.pendingTargetTiles_.empty()) {
                auto released = takeRelationReadyOutputLocked(outputIndex);
                if (!released) {
                    error = released.error();
                    break;
                }
                relationReady.push_back(std::move(*released));
            }
        }
    }
    if (error) {
        fail(*error);
        return;
    }
    auto ready = resolveRelationReadyOutputs(std::move(relationReady));
    if (!ready) {
        fail(ready.error());
        return;
    }
    emitCompletedOutputs(std::move(*ready));
    emitProgress("RelationTargetUnavailable");
    finishIfComplete();
}

void FilterRequestExecution::collectRelationTarget(
    MapTileKey const& targetKey,
    TileFeatureLayer::Ptr layer)
{
    if (request->isCancelled()) {
        return;
    }
    if (!layer || layer->id() != targetKey) {
        fail(simfil::Error{
            simfil::Error::InternalError,
            fmt::format(
                "Relation target request returned the wrong tile for {}.",
                targetKey.toString()),
        });
        return;
    }

    std::vector<RelationReadyOutput> relationReady;
    std::optional<simfil::Error> error;
    {
        std::lock_guard lock(mutex);
        if (terminal || request->isCancelled()) {
            return;
        }
        auto found = relationTargetTiles.find(targetKey);
        if (found == relationTargetTiles.end()) {
            // Pruning may remove the final dependent while a copied child
            // callback is already in flight. That late value is obsolete.
            return;
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
                pending->second.pendingTargetTiles_.erase(targetKey);
                if (pending->second.pendingTargetTiles_.empty()) {
                    auto released = takeRelationReadyOutputLocked(outputIndex);
                    if (!released) {
                        error = released.error();
                        break;
                    }
                    relationReady.push_back(std::move(*released));
                }
            }
        }
    }
    if (error) {
        fail(*error);
        return;
    }
    auto ready = resolveRelationReadyOutputs(std::move(relationReady));
    if (!ready) {
        fail(ready.error());
        return;
    }
    emitCompletedOutputs(std::move(*ready));
    emitProgress("RelationTargetResolved");
    finishIfComplete();
}

tl::expected<void, simfil::Error> FilterRequestExecution::resolveStoredRelationDescriptors(
    ReadyOutput& output,
    std::vector<FeatureLayerRelationDescriptor>& descriptors)
{
    if (!hasStoredRelations) {
        return {};
    }

    std::vector<FeatureLayerRelationDescriptor> resolved;
    resolved.reserve(descriptors.size());
    for (auto&& descriptor : descriptors) {
        if (descriptor.target_) {
            resolved.push_back(std::move(descriptor));
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
            resolved.push_back(std::move(descriptor));
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
        output.addIssue(FilterIssue{
            channelId,
            "<relation-target>",
            Scope::Relation,
            std::move(message),
            1,
        });
    }
    descriptors = std::move(resolved);
    return {};
}

std::vector<MapTileKey> FilterRequestExecution::liveOutputKeys()
{
    std::vector<MapTileKey> result;
    std::lock_guard lock(mutex);
    result.reserve(outputs.size() - prunedOutputTiles);
    for (auto const& output : outputs) {
        // A pruned south-west owner must not suppress a relation in an output
        // that still belongs to the current request.
        if (output.state_ != OutputTileState::State::Pruned) {
            result.emplace_back(
                LayerType::Features,
                request->mapId_,
                request->layerId_,
                output.tileId_);
        }
    }
    return result;
}

tl::expected<bool, simfil::Error> FilterRequestExecution::finalizePointGroups(
    ReadyOutput& output,
    std::span<FeatureLayerPointGroupMember const> members)
{
    if (!hasPointGroups) {
        return true;
    }

    auto const startedAt = std::chrono::steady_clock::now();
    auto completion = request->filter_.completePointGroups(
        *output.layer_,
        members,
        [request = this->request] { return request->isCancelled(); },
        &expressionCache);
    if (!completion) {
        return tl::unexpected(completion.error());
    }
    if (request->isCancelled()) {
        return false;
    }
    output.addIssues(std::move(completion->issues_));
    output.addTraces(std::move(completion->traces_));
    output.layer_->setInfo(
        "Filter/Process-Groups#ms",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt)
            .count());
    return true;
}

tl::expected<bool, simfil::Error> FilterRequestExecution::finalizeRelations(
    ReadyOutput& output,
    std::span<FeatureLayerRelationDescriptor const> descriptors)
{
    if (!hasStoredRelations) {
        return true;
    }

    auto const requestedOutputKeys = liveOutputKeys();
    auto const startedAt = std::chrono::steady_clock::now();
    auto completion = request->filter_.completeRelations(
        *output.layer_,
        descriptors,
        requestedOutputKeys,
        request->exactRoots_,
        [request = this->request] { return request->isCancelled(); },
        &expressionCache);
    if (!completion) {
        return tl::unexpected(completion.error());
    }
    if (request->isCancelled()) {
        return false;
    }
    output.addIssues(std::move(completion->issues_));
    output.addTraces(std::move(completion->traces_));
    auto const priorMilliseconds = output.layer_->info().value("Filter/Process-Relations#ms", 0.0);
    output.layer_->setInfo(
        "Filter/Process-Relations#ms",
        priorMilliseconds +
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt)
                .count());
    return true;
}

tl::expected<std::optional<size_t>, simfil::Error>
FilterRequestExecution::finalizeOutput(ReadyOutput ready)
{
    if (!outputLive(ready.outputIndex_)) {
        releaseReadyOutput(ready);
        return std::nullopt;
    }
    std::vector<TileSubsetDependency> dependencies;
    std::vector<FeatureLayerPointGroupMember> members;
    std::vector<FeatureLayerRelationDescriptor> relationDescriptors;
    simfil::Diagnostics diagnostics;

    for (auto& contribution : ready.contributions_) {
        ready.considerLifetime(contribution.lifetime_);
        dependencies.push_back(std::move(contribution.dependency_));
        members.insert(
            members.end(),
            std::make_move_iterator(contribution.pointGroupMembers_.begin()),
            std::make_move_iterator(contribution.pointGroupMembers_.end()));
        relationDescriptors.insert(
            relationDescriptors.end(),
            std::make_move_iterator(contribution.relationDescriptors_.begin()),
            std::make_move_iterator(contribution.relationDescriptors_.end()));
        ready.addIssues(std::move(contribution.issues_));
        ready.addTraces(std::move(contribution.traces_));
        if (!contribution.diagnostics_.exprIndex_.empty() ||
            !contribution.diagnostics_.fieldData_.empty() ||
            !contribution.diagnostics_.comparisonData_.empty())
        {
            diagnostics.append(contribution.diagnostics_);
        }
    }
    for (auto& [_, contribution] : ready.dynamicContributions_) {
        ready.considerLifetime(contribution.lifetime_);
        dependencies.push_back(std::move(contribution.dependency_));
        ready.addIssues(std::move(contribution.issues_));
        ready.addTraces(std::move(contribution.traces_));
    }

    auto resolvedRelations = resolveStoredRelationDescriptors(ready, relationDescriptors);
    if (!resolvedRelations) {
        return tl::unexpected(resolvedRelations.error());
    }

    if (ready.limitingLifetime_) {
        // Preserve the limiting source's original positive TTL pair. Deriving a
        // duration from the output timestamp could turn an already-expired
        // dependency into a negative TTL, which means non-expiring on the wire.
        ready.layer_->setTimestamp(ready.limitingLifetime_->timestamp_);
        ready.layer_->setTtl(ready.limitingLifetime_->ttl_);
    }
    else {
        ready.layer_->setTtl(std::nullopt);
    }
    ready.layer_->setDependencies(std::move(dependencies));
    auto groupsFinalized = finalizePointGroups(ready, members);
    if (!groupsFinalized) {
        return tl::unexpected(groupsFinalized.error());
    }
    if (!*groupsFinalized) {
        releaseReadyOutput(ready);
        return std::nullopt;
    }
    auto relationsFinalized = finalizeRelations(ready, relationDescriptors);
    if (!relationsFinalized) {
        return tl::unexpected(relationsFinalized.error());
    }
    if (!*relationsFinalized) {
        releaseReadyOutput(ready);
        return std::nullopt;
    }

    ready.installMetadata();
    ready.layer_->setDiagnostics(diagnostics);

    size_t entryCount = 0;
    ready.layer_->forEachChannel(
        [&](model_ptr<TileSubsetChannel> const& channel)
        {
            entryCount += channel->entryCount();
            return true;
        });
    if (request->isCancelled()) {
        releaseReadyOutput(ready);
        return std::nullopt;
    }
    auto const finalLayerBytes = ready.layer_->memoryUsage().total().allocatedBytes;
    if (finalLayerBytes > ready.layerBytes_) {
        memory->outputSubsetModels.add(finalLayerBytes - ready.layerBytes_);
    }
    else {
        memory->outputSubsetModels.subtract(ready.layerBytes_ - finalLayerBytes);
    }
    bool emit = false;
    {
        std::lock_guard lock(mutex);
        if (!terminal && ready.outputIndex_ < outputs.size() &&
            outputs[ready.outputIndex_].state_ == OutputTileState::State::Taken)
        {
            outputs[ready.outputIndex_].state_ = OutputTileState::State::Emitted;
            ++emittedOutputTiles;
            entriesEmitted += entryCount;
            emit = true;
        }
    }
    if (emit) {
        request->notifyResult(std::move(ready.layer_));
    }
    memory->outputSubsetModels.subtract(finalLayerBytes);
    return emit ? std::optional<size_t>{entryCount} : std::nullopt;
}

bool FilterRequestExecution::emitCompletedOutputs(std::vector<ReadyOutput> ready)
{
    for (auto&& output : ready) {
        if (request->isCancelled()) {
            return false;
        }
        auto entryCount = finalizeOutput(std::move(output));
        if (!entryCount) {
            fail(entryCount.error());
            return false;
        }
        if (!*entryCount) {
            continue;
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
    bool evaluationActive = true;
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
    auto finishEvaluation = [&]()
    {
        if (!evaluationActive) {
            return;
        }
        {
            std::lock_guard lock(mutex);
            if (activeEvaluations > 0) {
                --activeEvaluations;
            }
        }
        memory->sourceTileModels.subtract(sourceBytes);
        evaluationActive = false;
    };
    try {
        if (!source || request->isCancelled()) {
            finishEvaluation();
            finishIfComplete();
            return;
        }

        bool sourceStillNeeded = false;
        bool materializeOutput = false;
        {
            std::lock_guard lock(mutex);
            sourceStillNeeded = !terminal && sourceNeeded(sourceIndex);
            if (auto output = outputIndexByTile.find(source->tileId());
                output != outputIndexByTile.end()) {
                materializeOutput = outputs[output->second].state_ ==
                    OutputTileState::State::Pending;
            }
        }
        if (!sourceStillNeeded) {
            finishEvaluation();
            finishIfComplete();
            return;
        }

        auto const filterStartedAt = std::chrono::steady_clock::now();
        auto sourceResult = request->filter_.filterSource(
            *source,
            materializeOutput,
            request->exactRoots_,
            [this, sourceIndex]
            { return request->isCancelled() || request->isDone() || !sourceNeeded(sourceIndex); },
            &expressionCache);
        if (!sourceResult) {
            finishEvaluation();
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
        if (request->isCancelled() || request->isDone() || !sourceNeeded(sourceIndex)) {
            releaseTemporary();
            releaseUntransferredOutputModel();
            finishEvaluation();
            finishIfComplete();
            return;
        }
        auto const relationStartedAt = std::chrono::steady_clock::now();
        auto locatedRelations = locateRelationTargets(*source, *sourceResult);
        if (!locatedRelations) {
            releaseTemporary();
            releaseUntransferredOutputModel();
            finishEvaluation();
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
            finishEvaluation();
            fail(ready.error());
            return;
        }
        outputModelTransferred = trackedOutputModelBytes != 0;
        releaseTemporary();
        emitProgress("SourceTileEvaluated");

        auto prepared = prepareRelationOutputs(std::move(*ready));
        if (!prepared) {
            finishEvaluation();
            fail(prepared.error());
            return;
        }
        if (!emitCompletedOutputs(std::move(*prepared))) {
            finishEvaluation();
            return;
        }
        finishEvaluation();
        finishIfComplete();
    }
    catch (std::exception const& exception) {
        releaseTemporary();
        releaseUntransferredOutputModel();
        finishEvaluation();
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
    catch (...) {
        releaseTemporary();
        releaseUntransferredOutputModel();
        finishEvaluation();
        auto const sourceTileId = source ? source->tileId() : TileId{};
        fail(simfil::Error{
            simfil::Error::InternalError,
            fmt::format(
                "Filter evaluation failed for {}::{} tile {:x}: non-standard exception",
                request->mapId_,
                request->layerId_,
                sourceTileId.value())});
    }
}

void FilterRequestExecution::childFinished(RequestStatus status)
{
    if (request->isCancelled()) {
        return;
    }
    {
        std::lock_guard lock(mutex);
        if (sourceChildDetached) {
            return;
        }
    }
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
    liveOutputTileIds_.insert(tileIds_.begin(), tileIds_.end());
    for (auto const& priorityTileId : priorityTileIds_) {
        if (!seenTileIds.contains(priorityTileId)) {
            raise("Priority tile IDs must be contained in the request tile IDs.");
        }
    }
    if (tileIds_.empty()) {
        status_ = RequestStatus::Success;
    }
}

std::pair<bool, bool>
FeatureLayerFilterTilesRequest::retainOutputTileIds(std::set<TileId> const& retained)
{
    std::lock_guard lock(outputMembershipMutex_);
    auto const previousSize = liveOutputTileIds_.size();
    std::erase_if(
        liveOutputTileIds_,
        [&](TileId const& tileId) { return !retained.contains(tileId); });
    return {
        !liveOutputTileIds_.empty(),
        liveOutputTileIds_.size() != previousSize,
    };
}

bool FeatureLayerFilterTilesRequest::acceptsOutputTile(TileId tileId) const
{
    std::lock_guard lock(outputMembershipMutex_);
    return liveOutputTileIds_.contains(tileId);
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
    if (!result || cancelled_ || isDone() || !acceptsOutputTile(result->tileId())) {
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
    auto expected = RequestStatus::Open;
    if (!status_.compare_exchange_strong(expected, s)) {
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

bool detail::FilterRequestExecution::rejectStart(
    FeatureLayerFilterTilesRequest::Ptr const& request,
    std::string message)
{
    auto status = makeFilterStatusJson(*request, "Failed");
    status["error"] = std::move(message);
    request->notifyProgress(status);
    request->setStatus(RequestStatus::Aborted);
    return false;
}

tl::expected<void, simfil::Error> detail::FilterRequestExecution::prepareExactRoots(
    FeatureLayerFilterTilesRequest::Ptr const& request,
    bool hasStoredRelations)
{
    if (request->exactRoots_.size() > 4096) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InvalidArguments,
            "Stored-relation traversal exceeded the initial 4096 exact-root limit.",
        });
    }
    for (size_t rootIndex = 0; rootIndex < request->exactRoots_.size(); ++rootIndex) {
        request->exactRoots_[rootIndex].requestOrdinal_ = rootIndex;
    }
    if (!request->exactRoots_.empty() && !hasStoredRelations) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InvalidArguments,
            "Exact roots are valid only for a relation-scope filter bundle.",
        });
    }

    auto const requestedOutputs =
        std::set<TileId>(request->tileIds_.begin(), request->tileIds_.end());
    if (std::ranges::any_of(
            request->exactRoots_,
            [&](auto const& root) { return !requestedOutputs.contains(root.tileId_); }))
    {
        return tl::unexpected(simfil::Error{
            simfil::Error::InvalidArguments,
            "Every exact relation root must belong to an original requested output tile.",
        });
    }
    return {};
}

tl::expected<std::vector<TileId>, simfil::Error> detail::FilterRequestExecution::processingTileIds(
    FeatureLayerFilterTilesRequest const& request,
    bool hasPointGroups,
    std::set<TileId>& prioritySourceMembership)
{
    if (!hasPointGroups) {
        return request.tileIds_;
    }

    auto const level = request.tileIds_.front().level();
    for (auto const& tileId : request.tileIds_) {
        if (!tileId.isValid() || tileId.level() != level) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InvalidArguments,
                "Point-grid outputs must be valid tiles at one common level.",
            });
        }
    }
    auto const [tileWidth, tileHeight] = request.tileIds_.front().wgs84Size();
    for (auto const& channel : request.filter_.channels_) {
        if (!channel.group_) {
            continue;
        }
        if (!std::isfinite(channel.group_->cellSize_.x) ||
            !std::isfinite(channel.group_->cellSize_.y) || channel.group_->cellSize_.x <= 0.0 ||
            channel.group_->cellSize_.y <= 0.0 || channel.group_->cellSize_.x > tileWidth ||
            channel.group_->cellSize_.y > tileHeight)
        {
            return tl::unexpected(simfil::Error{
                simfil::Error::InvalidArguments,
                fmt::format(
                    "Point-grid channel '{}' exceeds the initial one-tile halo span.",
                    channel.channelId_),
            });
        }
    }

    std::set<TileId> sourceTileMembership;
    std::vector<TileId> result;
    result.reserve(request.tileIds_.size());

    // Traverse outputs in caller order and insert every source at its first
    // point of need. Appending all halo sources after all outputs retains a
    // viewport-sized set of mutable subsets while sparse dependencies wait.
    for (auto const& outputTileId : request.tileIds_) {
        if (sourceTileMembership.insert(outputTileId).second) {
            result.push_back(outputTileId);
        }
        auto const priorityOutput = request.priorityTileIds_.contains(outputTileId);
        for (int32_t offsetY = -1; offsetY <= 1; ++offsetY) {
            for (int32_t offsetX = -1; offsetX <= 1; ++offsetX) {
                if (offsetX == 0 && offsetY == 0) {
                    continue;
                }
                auto const sourceTileId = outputTileId.neighbour(offsetX, offsetY);
                if (priorityOutput) {
                    // A prioritized output needs its complete halo, so those
                    // dependencies inherit the same scheduling priority.
                    prioritySourceMembership.insert(sourceTileId);
                }
                if (sourceTileMembership.insert(sourceTileId).second) {
                    result.push_back(sourceTileId);
                }
            }
        }
    }
    return result;
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
    auto exactRootsPrepared = prepareExactRoots(request, hasStoredRelations);
    if (!exactRootsPrepared) {
        return rejectStart(request, exactRootsPrepared.error().message);
    }

    auto prioritySourceMembership = request->priorityTileIds_;
    auto tileIdsToProcess = processingTileIds(*request, hasPointGroups, prioritySourceMembership);
    if (!tileIdsToProcess) {
        return rejectStart(request, tileIdsToProcess.error().message);
    }

    std::vector<TileId> prioritySourceTileIds;
    prioritySourceTileIds.reserve(prioritySourceMembership.size());
    std::ranges::copy_if(
        *tileIdsToProcess,
        std::back_inserter(prioritySourceTileIds),
        [&](auto const& tileId) { return prioritySourceMembership.contains(tileId); });

    auto childRequest = std::make_shared<LayerTilesRequest>(
        request->mapId_,
        request->layerId_,
        *tileIdsToProcess,
        prioritySourceTileIds);
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
    state->sourceRequest = childRequest;
    state->memory = std::make_shared<FilterMemoryTracker>();
    state->memory->mapId = request->mapId_;
    state->memory->layerId = request->layerId_;
    state->memory->filterId = request->filter_.filterId_;
    state->memory->generation = request->filter_.generation_;
    state->memory->requestedTiles = request->tileIds_.size();
    state->configure(request->tileIds_, std::move(*tileIdsToProcess));
    {
        std::lock_guard lock(request->childRequestsMutex_);
        request->execution_ = state;
    }
    state->memory->sampleOrchestration =
        [weakState = std::weak_ptr<detail::FilterRequestExecution>{state}]
    {
        auto active = weakState.lock();
        if (!active) {
            return uint64_t{0};
        }
        // Operational status must not queue behind filter coordination. A
        // contended sample reuses the last exact gauge value and retries on
        // the next poll instead of blocking an HTTP event-loop thread.
        std::unique_lock lock(active->mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return active->memory->orchestration.current();
        }
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
