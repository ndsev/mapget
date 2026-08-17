#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <glm/vec3.hpp>

#include "featurelayer.h"
#include "subsetlayer.h"

namespace mapget
{

/**
 * Candidate-expansion scope requested by one filter channel.
 *
 * `Auto` is a request-only value. A returned TileSubsetChannel always stores
 * one concrete terminal `Scope`.
 */
enum class FeatureLayerFilterScope : uint8_t {
    Feature,
    Attribute,
    Relation,
    Auto,
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    FeatureLayerFilterScope,
    {
        {FeatureLayerFilterScope::Feature, "feature"},
        {FeatureLayerFilterScope::Attribute, "attribute"},
        {FeatureLayerFilterScope::Relation, "relation"},
        {FeatureLayerFilterScope::Auto, "auto"},
    })

/** Scalar request binding available as both a SIMFIL constant and overlay field. */
using FeatureLayerFilterBinding = std::variant<std::monostate, bool, int64_t, double, std::string>;

/**
 * Portable selector for resolving a locate candidate inside one complete
 * feature tile.
 *
 * Exactly one form is used:
 * - `canonicalFeatureId_` performs an exact primary-id lookup.
 * - `typeId_ + featureFilter_` evaluates a schema-compiled SIMFIL expression
 *   against every feature of that type. Bindings and the request-local
 *   `$features` array are available to the expression.
 * - `typeId_ + featureIdExpression_` evaluates one schema-compiled SIMFIL
 *   expression for the tile and resolves its returned canonical feature IDs
 *   through the tile's primary-id index. This form avoids re-evaluating a
 *   tile-wide identity expression once for every candidate feature.
 */
struct FeatureLayerSelector
{
    std::optional<std::string> canonicalFeatureId_;
    std::string typeId_;
    std::optional<std::string> featureFilter_;
    std::optional<std::string> featureIdExpression_;
    std::map<std::string, FeatureLayerFilterBinding> bindings_;

    /** Return whether this selector uses canonical identity instead of SIMFIL. */
    [[nodiscard]] bool isExact() const { return canonicalFeatureId_.has_value(); }
};

/** Initial feature-only point-grid grouping operator. */
struct FeatureLayerPointGridGroup
{
    glm::dvec3 origin_{0.0, 0.0, 0.0};
    glm::dvec3 cellSize_{1.0, 1.0, 1.0};

    bool operator==(FeatureLayerPointGridGroup const&) const = default;
};

/** Narrow options for traversing stored relations. */
struct FeatureLayerStoredRelationOptions
{
    /** Optional regular expression matched against the stored relation name. */
    std::optional<std::string> relationNamePattern_;
    bool recursive_ = false;
    bool mergeTwoway_ = false;

    bool operator==(FeatureLayerStoredRelationOptions const&) const = default;
};

/** Definition of one ordered terminal result channel. */
struct FeatureLayerFilterChannel
{
    static constexpr uint32_t AllGeometryTypes = 0xffffffffu;

    std::string channelId_;
    std::optional<std::string> featureFilter_;
    std::optional<std::string> entryFilter_;
    FeatureLayerFilterScope scope_ = FeatureLayerFilterScope::Feature;

    /**
     * Apply LayerSchema::normalizeSearchQuery() to entryFilter.
     *
     * Ordinary schema-aware SIMFIL compilation is always enabled for both
     * filters and both field lists, independent of this flag.
     */
    bool rewrite_ = false;

    std::vector<std::string> featureTypes_;
    std::vector<std::string> featureFields_;
    std::vector<std::string> entryFields_;
    uint32_t geometryTypes_ = AllGeometryTypes;

    /** Nullopt selects all geometry names; a value selects that exact name. */
    std::optional<std::string> geometryName_;

    /** Present only for the feature-only point-grid grouping channel. */
    std::optional<FeatureLayerPointGridGroup> group_;

    /** Required exactly when scope is Relation. */
    std::optional<FeatureLayerStoredRelationOptions> relation_;
};

/** One exact relation-traversal root, retained in stable request order. */
struct FeatureLayerFilterRoot
{
    TileId tileId_;
    std::string typeId_;
    KeyValuePairs featureId_;
    size_t requestOrdinal_ = 0;
    /** Canonical FeatureId::toString() form accepted from frontend picks. */
    std::string canonicalFeatureId_;
};

/** Result of source-local filter evaluation. */
struct FeatureLayerFilterResult
{
    TileSubsetLayer::Ptr layer_;
};

/** Stable integer cell identity for a point-grid group. */
struct FeatureLayerPointGroupKey
{
    int64_t x_ = 0;
    int64_t y_ = 0;
    int64_t z_ = 0;

    auto operator<=>(FeatureLayerPointGroupKey const&) const = default;
};

/**
 * One source feature's request-local participation in one point-grid cell.
 *
 * The owning model_ptr deliberately keeps the immutable source tile alive
 * until every output that uses this member has completed.
 */
struct FeatureLayerPointGroupMember
{
    size_t channelIndex_ = 0;
    FeatureLayerPointGroupKey key_;
    model_ptr<Feature> feature_;
    Point representativePoint_;
    uint32_t geometryOrdinal_ = 0;
    uint32_t pointOrdinal_ = 0;
    std::optional<std::string> geometryName_;
};

/** One portable candidate awaiting in-tile relation-target resolution. */
struct FeatureLayerRelationTargetCandidate
{
    MapTileKey tileKey_;
    FeatureLayerSelector selector_;
    bool resolved_ = false;
};

/**
 * One admitted stored relation whose target is local or awaits service-level
 * location and tile resolution.
 */
struct FeatureLayerRelationDescriptor
{
    size_t channelIndex_ = 0;
    model_ptr<Feature> source_;
    model_ptr<Relation> relation_;
    uint32_t relationOrdinal_ = 0;
    std::string targetTypeId_;
    KeyValuePairs targetFeatureId_;
    model_ptr<Feature> target_;
    std::optional<MapTileKey> targetTileKey_;
    std::vector<FeatureLayerRelationTargetCandidate> targetCandidates_;
    std::vector<model_ptr<Feature>> targetMatches_;
    size_t rootOrdinal_ = 0;
    bool exactRoot_ = false;
    /** True when a target-independent entry filter already accepted this relation. */
    bool entryFilterPreflighted_ = false;
};

/**
 * Result of scanning one complete source tile once for all bundled channels.
 *
 * `layer_` is present only when the source tile is also a requested output.
 * Group members remain request-local and are never serialized or cached.
 */
struct FeatureLayerFilterSourceResult
{
    TileSubsetLayer::Ptr layer_;
    std::vector<FeatureLayerPointGroupMember> pointGroupMembers_;
    std::vector<FeatureLayerRelationDescriptor> relationDescriptors_;
    std::vector<FilterIssue> issues_;
    std::map<std::string, simfil::Trace> traces_;
    simfil::Diagnostics diagnostics_;
    uint32_t sourceFeatureCount_ = 0;
};

/** Issues and traces produced while completing point groups for one output tile. */
struct FeatureLayerPointGroupCompletion
{
    std::vector<FilterIssue> issues_;
    std::map<std::string, simfil::Trace> traces_;
    size_t entriesAdded_ = 0;
};

/** Issues and traces from final relation pairing, filtering, and materialization. */
struct FeatureLayerRelationCompletion
{
    std::vector<FilterIssue> issues_;
    std::map<std::string, simfil::Trace> traces_;
    size_t entriesAdded_ = 0;
    size_t relationsSkippedOwnerOutsideCoverage_ = 0;
};

/** Optional cheap cancellation probe used by service-owned long traversals. */
using FeatureLayerFilterCancellationCheck = std::function<bool()>;

/**
 * Shared filter definition evaluated over one source tile or coordinated tile set.
 *
 * These methods operate only on already-loaded models. Cross-tile loading,
 * scheduling, and publication remain service responsibilities.
 */
struct FeatureLayerFilterRequest
{
    std::string filterId_;
    uint64_t generation_ = 0;
    /** Per-tile transport version; semantic filter identity remains generation_. */
    uint64_t deliveryEpoch_ = 0;
    std::vector<FeatureLayerFilterChannel> channels_;
    std::map<std::string, FeatureLayerFilterBinding> bindings_;

    /**
     * Scan one source tile in source-major order.
     *
     * Set `materializeOutput` for requested output tiles. Halo-only scans
     * evaluate coordinated operators without allocating a subset layer.
     */
    [[nodiscard]] tl::expected<FeatureLayerFilterSourceResult, simfil::Error> filterSource(
        TileFeatureLayer const& sourceLayer,
        bool materializeOutput,
        std::span<FeatureLayerFilterRoot const> exactRoots = {},
        FeatureLayerFilterCancellationCheck const& cancellationCheck = {}) const;

    /** Materialize completed point-grid groups into an output created by filterSource(). */
    [[nodiscard]] tl::expected<FeatureLayerPointGroupCompletion, simfil::Error> completePointGroups(
        TileSubsetLayer& outputLayer,
        std::span<FeatureLayerPointGroupMember const> members,
        FeatureLayerFilterCancellationCheck const& cancellationCheck = {}) const;

    /**
     * Pair, filter, and materialize fully resolved stored-relation descriptors.
     *
     * `requestedOutputKeys` controls permanent generic two-way ownership.
     * Exact-root traversal retains origin-output ownership; the first explicit
     * root owns a pair when both endpoints are roots.
     */
    [[nodiscard]] tl::expected<FeatureLayerRelationCompletion, simfil::Error> completeRelations(
        TileSubsetLayer& outputLayer,
        std::span<FeatureLayerRelationDescriptor const> descriptors,
        std::span<MapTileKey const> requestedOutputKeys,
        std::span<FeatureLayerFilterRoot const> exactRoots = {},
        FeatureLayerFilterCancellationCheck const& cancellationCheck = {}) const;

    /**
     * Evaluate source-local feature and attribute channels in one source pass.
     *
     * Point grouping and stored relations require service coordination and are
     * rejected by this convenience operation.
     */
    [[nodiscard]] tl::expected<FeatureLayerFilterResult, simfil::Error>
    filter(TileFeatureLayer const& sourceLayer) const;
};

}  // namespace mapget
