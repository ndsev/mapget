#pragma once

#include "mapget/model/featurelayer-search.h"
#include "mapget/model/layer.h"
#include "mapget/model/stream.h"

#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace mapget::detail
{

/**
 * Normalized representation of one layer tile request parsed from HTTP or WS JSON.
 *
 * `tileIdsByNextStage` stores either one unstaged bucket or staged buckets where
 * each bucket index is the next stage the client still needs for those tiles.
 */
struct ParsedLayerTilesRequest
{
    std::string mapId;
    std::string layerId;
    std::vector<std::vector<TileId>> tileIdsByNextStage;
    std::vector<TileId> priorityTileIds;
    std::optional<FeatureLayerSearchRequest> searchRequest;
    bool usesStageBuckets = false;
};

/** Copy top-level search fields into one layer request when clients use envelope-level search syntax. */
void inheritSearchFields(nlohmann::json& requestJson, const nlohmann::json& envelopeJson);

/** Parse one layer tile request from the shared HTTP/WS request JSON shape. */
ParsedLayerTilesRequest parseLayerTilesRequestJson(const nlohmann::json& requestJson);

/** Parse the optional string-pool offset map advertised by a reconnecting client. */
TileLayerStream::StringPoolOffsetMap parseStringPoolOffsetsJson(const nlohmann::json& offsetsJson);

/** Collapse staged search buckets into one deduplicated tile-id list. */
std::vector<TileId> collectSearchTileIds(const ParsedLayerTilesRequest& request);

/** Expand a parsed request into concrete service queue keys, preserving priority-tile ordering. */
std::vector<MapTileKey> expandLayerTilesRequestKeys(
    const ParsedLayerTilesRequest& request,
    LayerType layerType,
    uint32_t stageCount);

}  // namespace mapget::detail
