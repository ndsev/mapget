#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "featurelayer.h"
#include "searchresultlayer.h"

namespace mapget
{

/** Search scope used by searchFeatureLayerAsResultLayer(). */
enum class FeatureLayerSearchScope
{
    Feature,
    Attribute,
};

/** Request parameters for turning a TileFeatureLayer into a TileSearchResultLayer. */
struct FeatureLayerSearchRequest
{
    /** Client-visible search identity. Reusing this id updates the logical search. */
    std::string searchId_;
    /** Transport-internal refresh/fingerprint key used to drop stale result frames. */
    std::string requestKey_;
    /** SIMFIL predicate evaluated on each candidate feature or attribute context. */
    std::string query_;
    /** Selects whether the predicate runs once per feature or once per attribute validity. */
    FeatureLayerSearchScope scope_ = FeatureLayerSearchScope::Feature;
    /** SIMFIL expressions materialized into each SearchResult::values() row. */
    std::vector<std::string> withFields_;
    /** Optional client refresh counter for ordering updates of the same search id. */
    std::optional<int64_t> refresh_;
    /** Optional override for the result layer's synthetic string-pool node id. */
    std::optional<std::string> resultNodeId_;
    /** Source stage mask used to assemble the searched tile, if it came from staged payloads. */
    std::vector<uint32_t> sourceStageMask_;
    /** Result chunk index. V1 currently emits one result layer per searched tile. */
    uint32_t chunkIndex_ = 0;
};

/** Result of a server-side feature-layer search. */
struct FeatureLayerSearchResult
{
    TileSearchResultLayer::Ptr layer_;
    simfil::Diagnostics diagnostics_;
};

/**
 * Evaluate a SIMFIL search query on one feature tile and return map-stylable result roots.
 *
 * The returned TileSearchResultLayer stores copied feature ids, copied primary feature
 * geometries, and a fixed values array for each expression in request.withFields_.
 * SIMFIL trace() aggregates are attached to layer->info()["traces"].
 */
tl::expected<FeatureLayerSearchResult, simfil::Error> searchFeatureLayerAsResultLayer(
    TileFeatureLayer& sourceLayer,
    FeatureLayerSearchRequest const& request);

/**
 * Assemble a transient full feature tile from ordered staged payloads.
 *
 * The returned layer owns a caller-provided synthetic string pool and clones
 * every stage into a single feature model. Source tiles and datasource-owned
 * string pools are left untouched.
 */
tl::expected<TileFeatureLayer::Ptr, simfil::Error> assembleFeatureLayerStages(
    std::span<TileFeatureLayer::Ptr const> stages,
    std::string_view evaluationNodeId);

} // namespace mapget
