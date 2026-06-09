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
    Auto,
};

/** Request parameters for turning a TileFeatureLayer into a TileSearchResultLayer. */
struct FeatureLayerSearchRequest
{
    /** Optional client-visible search identity used by interactive transports. */
    std::string searchId_;
    /** Transport-internal refresh/fingerprint key used to drop stale result frames. */
    std::string requestKey_;
    /** SIMFIL predicate evaluated on each candidate feature or attribute context. */
    std::string query_;
    /** Selects whether the predicate runs once per feature or once per attribute validity. */
    FeatureLayerSearchScope scope_ = FeatureLayerSearchScope::Feature;
    /** Run schema-backed query normalization before evaluating this search. */
    bool rewriteQuery_ = false;
    /** SIMFIL expressions materialized into each SearchResult::values() row. */
    std::vector<std::string> withFields_;
    /** Optional client refresh counter for ordering updates of the same search id. */
    std::optional<int64_t> refresh_;
    /** Source stage mask used to assemble the searched tile, if it came from staged payloads. */
    std::vector<uint32_t> sourceStageMask_;
    /** Result chunk index. V1 currently emits one result layer per searched tile. */
    uint32_t chunkIndex_ = 0;
};

/** Result of a server-side feature-layer search. */
struct FeatureLayerSearchResult
{
    TileSearchResultLayer::Ptr layer_;
};

/**
 * Evaluate a SIMFIL search query on one feature tile and return map-stylable result roots.
 *
 * The returned TileSearchResultLayer stores copied feature ids, copied display
 * geometries, a fixed values array for each expression in request.withFields_,
 * and the parsed SIMFIL diagnostics collected while evaluating this chunk.
 * SIMFIL trace() aggregates are stored as typed SearchTrace nodes.
 */
tl::expected<FeatureLayerSearchResult, simfil::Error> searchFeatureLayerAsResultLayer(
    TileFeatureLayer& sourceLayer,
    FeatureLayerSearchRequest const& request);

/**
 * Assemble a full feature tile from ordered staged payloads.
 *
 * The first stage is reused as the base layer and later stages are attached as
 * overlays. Repeated assembly with the same stage pointers is idempotent.
 */
tl::expected<TileFeatureLayer::Ptr, simfil::Error> assembleFeatureLayerStages(
    std::span<TileFeatureLayer::Ptr const> stages);

} // namespace mapget
