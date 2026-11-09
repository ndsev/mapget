#pragma once

#include "mcp-mapget-call.h"

namespace mapget::detail
{

/** A bounded, immutable scan checkpoint. Model pointers retain their actual owning tile. */
struct McpMapgetTools::Continuation
{
    Principal principal;
    nlohmann::json arguments;
    uint64_t catalogRevision = 0;
    std::chrono::steady_clock::time_point expires;
    size_t retainedBytes = 0;
    std::vector<PartitionId> partitions;
    size_t nextPartition = 0;
    std::vector<std::string> featureIds;
    std::map<PartitionId, std::vector<std::pair<std::string, LocateCandidate>>> locateCandidates;
    std::set<std::string> resolvedFeatureIds;
    PartitionFeatureLayer::Ptr tile;
    size_t retainedTileBytes = 0;
    std::vector<model_ptr<Feature>> selected;
    bool selectionReady = false;
    size_t feature = 0;
    uint32_t attributeLayer = 0, attributeMember = 0, attributeIndex = 0, validity = 0;
    size_t progress = 0;
    size_t offsetRemaining = 0;
};

/** One-partition-at-a-time extraction and continuation state owned by an invocation.
 * Child callbacks retain/lock the Call; cancellation only detaches its child request.
 */
class McpMapgetTools::Call::Extraction
{
public:
    /** Create a fresh scan without loading a partition or scheduling work. */
    explicit Extraction(Call& call) : call_(call) {}

    /** Prepare or restore the scan, then schedule its first outstanding partition. */
    void run();

    /** Schedule child-request abort without entering callback locks on the MCP loop. */
    void cancel();

    /** Report unresolved requested identities only after a complete scan. */
    void reportMissingFeatures();

    /** Publish a checkpoint only after scan progress, never an endlessly repeating partial row. */
    void continueExtraction(nlohmann::json& result);

private:
    Call& call_;
    std::shared_ptr<Continuation> scan_ = std::make_shared<Continuation>();
    size_t initialProgress_ = 0;
    std::mutex requestMutex_;
    LayerTilesRequest::Ptr request_;

    /** Restore an authorized immutable checkpoint into this invocation's private scan state. */
    bool resumeExtraction();

    /** Prepare explicit partitions and retain cheap primary/secondary-ID locate selectors. */
    void prepareExtraction();

    /** Schedule one partition and evaluate on the delivering worker before loading another. */
    void loadNext();

    /** Iterate feature/attribute contexts with bounded predicates and projected sequences. */
    void extractFeatures(PartitionFeatureLayer::Ptr const& tile);

    /** Materialize one matched row with optional effective geometry and native provenance. */
    bool extractRow(
        model_ptr<Feature> const& feature,
        simfil::ModelNode const& context,
        simfil::TypeId schema,
        nlohmann::json identity,
        model_ptr<Validity> const& validity = {});

    /** List actual immediate fields, retaining scalar values but never expanding compounds. */
    nlohmann::json fieldSummary(simfil::ModelNode const& node);

    /** Describe a concrete feature without serializing coordinates or nested attribute payloads. */
    nlohmann::json featureSummary(model_ptr<Feature> const& feature);

    /** Resolve native source addresses or query source model roots directly. */
    void extractSourceData(PartitionSourceDataLayer::Ptr const& tile);
};

}  // namespace mapget::detail
