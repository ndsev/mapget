#pragma once

#include "mapget/model/featurelayer-filter.h"
#include "mapget/model/layer.h"
#include "mapget/model/stream.h"

#include <optional>
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace mapget::detail
{

/** Normalized representation of one HTTP or interactive layer request. */
struct ParsedLayerTilesRequest
{
    std::string mapId;
    std::string layerId;
    std::optional<std::string> sourceId;
    std::vector<TileId> tileIds;
    std::vector<TileId> priorityTileIds;
    /** Explicit reconnect/resume overrides; omitted tiles use or retain deliveryEpoch_. */
    std::map<TileId, uint64_t> deliveryEpochs;
    std::map<TileId, std::vector<std::string>> featureIdsByTile;
    std::vector<FeatureLayerFilterRoot> exactRoots;
    std::optional<FeatureLayerFilterRequest> filterRequest;
};

/** Copy top-level filter fields into one request item when omitted locally. */
void inheritFilterFields(
    nlohmann::json& requestJson,
    nlohmann::json const& envelopeJson);

/** Return true if an object contains any field belonging to `/filter`. */
bool containsFilterFields(nlohmann::json const& requestJson);

/** Parse one plain or interactive layer-tile request. */
ParsedLayerTilesRequest parseLayerTilesRequestJson(
    nlohmann::json const& requestJson);

/** Parse the shared REST `/filter` envelope definition. */
FeatureLayerFilterRequest parseRestFilterEnvelopeJson(
    nlohmann::json const& envelopeJson);

/** Parse one REST `/filter` source request and attach the shared bundle. */
ParsedLayerTilesRequest parseRestFilterLayerRequestJson(
    nlohmann::json const& requestJson,
    FeatureLayerFilterRequest const& filterTemplate);

/** Parse reconnecting-client string-pool offsets. */
TileLayerStream::StringPoolOffsetMap parseStringPoolOffsetsJson(
    nlohmann::json const& offsetsJson);

/** Deduplicate filter tile IDs while preserving first-seen order. */
std::vector<TileId> collectFilterTileIds(
    ParsedLayerTilesRequest const& request);

/** Expand a plain tile request into concrete scheduler keys. */
std::vector<MapTileKey> expandLayerTilesRequestKeys(
    ParsedLayerTilesRequest const& request,
    LayerType layerType);

/** Serialize one filter definition into the canonical public JSON shape. */
nlohmann::json filterRequestToJson(
    FeatureLayerFilterRequest const& request,
    bool includeIdentity = true);

} // namespace mapget::detail
