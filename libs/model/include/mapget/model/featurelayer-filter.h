#pragma once

#include <cstddef>
#include <cstdint>
#include <compare>
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
using FeatureLayerFilterBinding =
    std::variant<std::monostate, bool, int64_t, double, std::string>;

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

/** Shared definition evaluated over one source tile or coordinated tile set. */
struct FeatureLayerFilterRequest
{
    std::string filterId_;
    uint64_t generation_ = 0;
    std::vector<FeatureLayerFilterChannel> channels_;
    std::map<std::string, FeatureLayerFilterBinding> bindings_;
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

    auto operator<=>(FeatureLayerPointGroupKey const&) const =
        default;
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
    size_t rootOrdinal_ = 0;
    bool exactRoot_ = false;
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
using FeatureLayerFilterCancellationCheck =
    std::function<bool()>;

/**
 * Scan one source tile in source-major order.
 *
 * Set `materializeOutput` exactly for requested output tiles. Halo-only scans
 * evaluate only coordinated operators and do not allocate a subset layer.
 */
tl::expected<FeatureLayerFilterSourceResult, simfil::Error>
filterFeatureLayerSource(
    TileFeatureLayer const& sourceLayer,
    FeatureLayerFilterRequest const& request,
    bool materializeOutput,
    std::span<FeatureLayerFilterRoot const> exactRoots = {},
    FeatureLayerFilterCancellationCheck const&
        cancellationCheck = {});

/**
 * Deterministically merge and materialize completed point-grid groups into an
 * output layer whose channel roots were created by filterFeatureLayerSource().
 */
tl::expected<FeatureLayerPointGroupCompletion, simfil::Error>
completeFeatureLayerPointGroups(
    TileSubsetLayer& outputLayer,
    FeatureLayerFilterRequest const& request,
    std::span<FeatureLayerPointGroupMember const> members,
    FeatureLayerFilterCancellationCheck const&
        cancellationCheck = {});

/**
 * Pair, filter, and materialize fully resolved stored-relation descriptors.
 *
 * `requestedOutputKeys` is used only for permanent generic twoway ownership.
 * Exact-root traversal retains origin-output ownership; when both endpoints
 * are explicit roots, the first root in request order owns a merged pair.
 */
tl::expected<FeatureLayerRelationCompletion, simfil::Error>
completeFeatureLayerRelations(
    TileSubsetLayer& outputLayer,
    FeatureLayerFilterRequest const& request,
    std::span<FeatureLayerRelationDescriptor const> descriptors,
    std::span<MapTileKey const> requestedOutputKeys,
    std::span<FeatureLayerFilterRoot const> exactRoots = {},
    FeatureLayerFilterCancellationCheck const&
        cancellationCheck = {});

/**
 * Evaluate all source-local feature and attribute channels in one source pass.
 *
 * Point grouping and stored relations use the same request types but require
 * request-wide service coordination and are rejected by this source-local
 * entry point.
 */
tl::expected<FeatureLayerFilterResult, simfil::Error> filterFeatureLayer(
    TileFeatureLayer const& sourceLayer,
    FeatureLayerFilterRequest const& request);

} // namespace mapget
