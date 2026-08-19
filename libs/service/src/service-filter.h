#pragma once

#include "service-scheduler.h"

#include <atomic>
#include <map>
#include <set>
#include <utility>

namespace mapget::detail
{

/**
 * Owns one coordinated feature-filter request from admission through emission.
 *
 * Source tiles are loaded through ordinary service requests and evaluated by
 * the same worker immediately after backend access releases its datasource
 * permit. This object coordinates cross-tile groups, relations, progress,
 * cancellation, and memory accounting.
 */
class FilterRequestExecution final : public std::enable_shared_from_this<FilterRequestExecution>
{
public:
    /** Validate, admit, and start one filter request. */
    static bool start(
        Service::Impl& service,
        FeatureLayerFilterTilesRequest::Ptr const& request,
        std::optional<AuthHeaders> const& clientHeaders);

    /** Prune removed outputs while retaining shared source and relation work. */
    void retainOutputs(std::set<TileId> const& retainedTileIds);

    /** Stop coordinated work and break request/child callback ownership cycles. */
    void cancel();

    /** Clear active request-memory gauges when the last callback releases this execution. */
    ~FilterRequestExecution();

private:
    /** Construct only through start(), after admission inputs are known. */
    FilterRequestExecution() = default;

    /** Source-local output retained until every dependent output is complete. */
    struct SourceTileContribution
    {
        struct Lifetime
        {
            MapTileKey sourceKey_;
            std::chrono::system_clock::time_point timestamp_;
            std::chrono::milliseconds ttl_;

            [[nodiscard]] auto expiresAt() const { return timestamp_ + ttl_; }
        };

        TileSubsetDependency dependency_;
        std::vector<FeatureLayerPointGroupMember> pointGroupMembers_;
        std::vector<FeatureLayerRelationDescriptor> relationDescriptors_;
        std::vector<FilterIssue> issues_;
        std::map<std::string, simfil::Trace> traces_;
        simfil::Diagnostics diagnostics_;
        std::optional<Lifetime> lifetime_;
    };

    /** Mutable assembly state for one requested output tile. */
    struct OutputTileState
    {
        enum class State {
            Pending,
            Taken,
            Emitted,
            Pruned,
        };

        TileId tileId_;
        TileSubsetLayer::Ptr wipSubset_;
        std::vector<TileId> sourceTileIds_;
        std::vector<std::optional<SourceTileContribution>> contributions_;
        size_t missingContributions_ = 0;
        State state_ = State::Pending;
        uint64_t wipSubsetBytes_ = 0;
    };

    /** One source contribution slot in a dependent output. */
    struct DependentOutputSlot
    {
        size_t outputIndex_ = 0;
        size_t slotIndex_ = 0;
    };

    /** Fully source-complete output ready for relation/group finalization. */
    struct ReadyOutput
    {
        ReadyOutput(size_t outputIndex, TileSubsetLayer::Ptr layer, uint64_t layerBytes)
            : outputIndex_(outputIndex), layer_(std::move(layer)), layerBytes_(layerBytes)
        {
        }
        ReadyOutput(ReadyOutput&&) = default;
        ReadyOutput& operator=(ReadyOutput&&) = default;
        ReadyOutput(ReadyOutput const&) = delete;
        ReadyOutput& operator=(ReadyOutput const&) = delete;

        size_t outputIndex_ = 0;
        TileSubsetLayer::Ptr layer_;
        uint64_t layerBytes_ = 0;
        std::vector<SourceTileContribution> contributions_;
        std::map<MapTileKey, SourceTileContribution> dynamicContributions_;
        std::vector<FilterIssue> issues_;
    };

    /** Output waiting for one or more dynamically located relation tiles. */
    struct PendingRelationOutput
    {
        ReadyOutput ready_;
        std::set<MapTileKey> targetTiles_;
        std::set<MapTileKey> pendingTargetTiles_;
    };

    /** Immutable terminal target state carried beyond the coordination lock. */
    struct RelationTargetSnapshot
    {
        MapTileKey key_;
        TileFeatureLayer::Ptr layer_;
        std::optional<std::string> failureMessage_;
    };

    /** One output whose complete target set can now be resolved independently. */
    struct RelationReadyOutput
    {
        explicit RelationReadyOutput(ReadyOutput ready) : ready_(std::move(ready)) {}
        RelationReadyOutput(RelationReadyOutput&&) = default;
        RelationReadyOutput& operator=(RelationReadyOutput&&) = default;
        RelationReadyOutput(RelationReadyOutput const&) = delete;
        RelationReadyOutput& operator=(RelationReadyOutput const&) = delete;

        ReadyOutput ready_;
        std::vector<RelationTargetSnapshot> targets_;
    };

    /** Shared load state for one dynamically located relation target tile. */
    struct RelationTargetTileState
    {
        bool scheduled_ = false;
        bool terminal_ = false;
        TileFeatureLayer::Ptr layer_;
        std::optional<std::string> failureMessage_;
        std::set<size_t> dependentOutputs_;
        LayerTilesRequest::Ptr childRequest_;
    };

    /** Outputs and target loads produced by relation preparation. */
    struct PreparedRelationOutputs
    {
        PreparedRelationOutputs() = default;
        PreparedRelationOutputs(PreparedRelationOutputs&&) = default;
        PreparedRelationOutputs& operator=(PreparedRelationOutputs&&) = default;
        PreparedRelationOutputs(PreparedRelationOutputs const&) = delete;
        PreparedRelationOutputs& operator=(PreparedRelationOutputs const&) = delete;

        std::vector<ReadyOutput> ready_;
        std::vector<RelationReadyOutput> relationReady_;
        std::vector<MapTileKey> targetsToSchedule_;
    };

    using SelectorResolution =
        tl::expected<std::vector<model_ptr<Feature>>, simfil::Error>;
    using SharedSelectorResolution = std::shared_ptr<SelectorResolution const>;

    Service::Impl* impl = nullptr;
    FeatureLayerFilterTilesRequest::Ptr request;
    std::shared_ptr<DataSourceInfo const> sourceInfo;
    DataSource::Ptr sourceDataSource;
    std::optional<AuthHeaders> clientHeaders;
    bool hasPointGroups = false;
    bool hasStoredRelations = false;
    int outputLevel = 0;

    std::vector<TileId> sourceTileIds;
    std::map<TileId, size_t> sourceIndexByTile;
    std::map<TileId, size_t> outputIndexByTile;
    std::vector<OutputTileState> outputs;
    std::vector<std::vector<DependentOutputSlot>> dependentOutputsBySource;
    std::unique_ptr<std::atomic_size_t[]> liveDependentOutputsBySource;
    std::vector<bool> receivedSourceTiles;
    std::vector<bool> committedSourceTiles;
    LayerTilesRequest::Ptr sourceRequest;
    std::set<std::string> groupChannelIds;
    std::map<std::string, std::vector<LocateCandidate>> relationLocationCache;
    std::mutex relationLocationMutex;
    std::map<size_t, PendingRelationOutput> pendingRelationOutputs;
    std::map<MapTileKey, RelationTargetTileState> relationTargetTiles;
    std::map<std::string, SharedSelectorResolution> relationSelectorCache;
    mutable std::mutex relationSelectorCacheMutex;
    std::mutex mutex;
    size_t loadedSourceTiles = 0;
    size_t evaluatedSourceTiles = 0;
    size_t readyOutputTiles = 0;
    size_t emittedOutputTiles = 0;
    size_t prunedOutputTiles = 0;
    size_t entriesEmitted = 0;
    size_t activeEvaluations = 0;
    std::atomic_size_t progressEventCount = 0;
    bool childRequestDone = false;
    bool sourceChildDetached = false;
    bool terminal = false;
    std::shared_ptr<FilterMemoryTracker> memory;
    /** Recompute a conservative lower bound for request orchestration containers. */
    [[nodiscard]] uint64_t orchestrationBytesLocked() const;

    /** Measure source-local vectors that exist between SIMFIL evaluation and state commit. */
    [[nodiscard]] static uint64_t
    sourceResultAuxiliaryBytes(FeatureLayerFilterSourceResult const& result);

    /** Build source/output dependency indexes before child loading starts. */
    void configure(std::vector<TileId> const& outputTileIds, std::vector<TileId> processingTileIds);

    /** Return whether one output can still be completed and emitted. */
    [[nodiscard]] bool outputLive(size_t outputIndex);

    /** Return whether at least one pending output still needs this source. */
    [[nodiscard]] bool sourceNeeded(size_t sourceIndex) const;

    /** Remove one output from every source-liveness counter it contributed to. */
    void releaseOutputDependenciesLocked(OutputTileState const& output);

    /** Release a moved output model that became obsolete before emission. */
    void releaseReadyOutput(ReadyOutput& output);

    /** Snapshot exact request counters for a transport progress event. */
    [[nodiscard]] nlohmann::json progress(std::string state);

    /** Emit a throttled update, or an exact terminal update when forced. */
    void emitProgress(std::string state, bool force = false);

    /** Drop completed child-request ownership without changing its status. */
    void releaseChildRequests();

    /** Abort and detach every source or relation-target child request. */
    void abortChildRequests();

    /** Publish terminal success once all source evaluations and outputs are complete. */
    void finishIfComplete();

    /** Abort the coordinated request and publish a structured failure status. */
    void fail(simfil::Error const& error);

    /** Evaluate one source tile on the worker that delivered it. */
    void collect(TileFeatureLayer::Ptr layer);

    /** Locate unresolved stored-relation targets without loading their tiles. */
    tl::expected<void, simfil::Error>
    locateRelationTargets(TileFeatureLayer const& source, FeatureLayerFilterSourceResult& result);

    /** Commit one evaluated source into every output that depends on it. */
    tl::expected<std::vector<ReadyOutput>, simfil::Error> commitSource(
        size_t sourceIndex,
        TileFeatureLayer const& source,
        uint64_t outputModelBytes,
        FeatureLayerFilterSourceResult result);

    /** Resolve complete target snapshots without holding request coordination state. */
    tl::expected<std::vector<ReadyOutput>, simfil::Error>
    resolveRelationReadyOutputs(std::vector<RelationReadyOutput> ready);

    /** Move one fully unblocked output and its terminal target snapshots out of shared state. */
    tl::expected<RelationReadyOutput, simfil::Error>
    takeRelationReadyOutputLocked(size_t outputIndex);

    /** Record one loaded relation target as a dependency of a completed output. */
    tl::expected<void, simfil::Error> addRelationTargetContribution(
        ReadyOutput& output,
        MapTileKey const& targetKey,
        TileFeatureLayer const& targetLayer);

    /** Convert an unavailable target tile into per-channel output issues. */
    void markRelationTargetUnavailableInOutput(
        ReadyOutput& output,
        MapTileKey const& targetKey,
        std::string const& failureMessage);

    /** Partition complete outputs into finalizable and target-waiting sets. */
    tl::expected<PreparedRelationOutputs, simfil::Error>
    prepareRelationOutputs(std::vector<ReadyOutput> fixedReady);

    /** Schedule an ordinary coalescible request for one relation target tile. */
    void scheduleRelationTarget(MapTileKey const& targetKey);

    /** Complete outputs waiting on a relation target that could not load. */
    void completeUnavailableRelationTarget(MapTileKey const& targetKey, std::string message);

    /** Commit a loaded relation target to every dependent output. */
    void collectRelationTarget(MapTileKey const& targetKey, TileFeatureLayer::Ptr layer);

    /** Finish groups/relations and emit one immutable subset layer. */
    tl::expected<std::optional<size_t>, simfil::Error> finalizeOutput(ReadyOutput ready);

    /** Finalize source-complete outputs in stable output order. */
    bool emitCompletedOutputs(std::vector<ReadyOutput> ready);

    /** Evaluate and commit one source while maintaining memory gauges. */
    void evaluate(size_t sourceIndex, uint64_t sourceBytes, TileFeatureLayer::Ptr source);

    /** Process the terminal state of the ordinary source-tile child request. */
    void childFinished(RequestStatus status);
};

}  // namespace mapget::detail
