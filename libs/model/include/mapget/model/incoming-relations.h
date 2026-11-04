#pragma once

#include "featurelayer.h"
#include "relation.h"

#include <map>

namespace mapget
{

/** One stored, source-owned relation associated with the selected target by the service. */
struct IncomingRelationReference
{
    MapPartitionKey source_;
    std::string featureId_;
    uint32_t relationIndex_ = 0;
    std::string name_;
    /** Canonical target selected by the service, including secondary-ID selector resolution. */
    std::string targetFeatureId_;
};

/** Typed manifest accompanying immutable target/source snapshots in an incoming response. */
struct IncomingRelationCollection
{
    static constexpr size_t MaxRelations = 4096;
    static constexpr size_t MaxSnapshots = 64;
    MapPartitionKey target_;
    std::string featureId_;
    std::vector<MapPartitionKey> snapshots_;
    std::vector<IncomingRelationReference> relations_;
    std::vector<std::string> issues_;
    bool complete_ = true;
    std::optional<int64_t> expiresAtMs_;

    /** Encode the versioned result contract independently of feature-layer dictionaries. */
    [[nodiscard]] nlohmann::json toJson() const;
    /** Decode a bounded manifest, rejecting unsupported and malformed result contracts. */
    [[nodiscard]] static IncomingRelationCollection fromJson(nlohmann::json const& json);
};

/** Match a reference using the target model's canonical ID compositions, without loading data. */
[[nodiscard]] bool relationTargetsFeature(Relation const& relation, Feature const& target);

/** Copy only selected features while preserving provenance, ordinals and owning layer identity. */
[[nodiscard]] PartitionFeatureLayer::Ptr cloneInspectionFeatures(
    PartitionFeatureLayer::Ptr const& source, std::span<std::string const> featureIds,
    size_t byteBudget = std::numeric_limits<size_t>::max(),
    std::function<bool()> const& cancelled = {});

/** Request-owned inspection context; membership is assigned by the backend and validated here. */
class FeatureInspectionView
{
public:
    /** Resolved source relation whose owning model is retained by the view. */
    struct Incoming
    {
        model_ptr<Feature> source_;
        model_ptr<Relation> relation_;
        MapPartitionKey sourceKey_;
        uint32_t ordinal_ = 0;
    };

    /** Validate the complete bundle before exposing any foreign model nodes to inspection. */
    FeatureInspectionView(IncomingRelationCollection collection,
                          std::vector<PartitionFeatureLayer::Ptr> snapshots);
    /** The selected target from the same response as the incoming relations. */
    [[nodiscard]] model_ptr<Feature> target() const { return target_; }
    /** Source-owned relation handles in stable service order. */
    [[nodiscard]] std::vector<Incoming> const& incoming() const { return incoming_; }
    /** Completeness, diagnostics and expiry from the backend. */
    [[nodiscard]] IncomingRelationCollection const& collection() const { return collection_; }

private:
    IncomingRelationCollection collection_;
    std::vector<PartitionFeatureLayer::Ptr> snapshots_;
    model_ptr<Feature> target_;
    std::vector<Incoming> incoming_;
};

} // namespace mapget
