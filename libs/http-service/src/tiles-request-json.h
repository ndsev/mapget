#pragma once

#include "mapget/model/featurelayer-search.h"
#include "mapget/model/layer.h"

#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace mapget::detail
{

struct ParsedLayerTilesRequest
{
    std::string mapId;
    std::string layerId;
    std::vector<std::vector<TileId>> tileIdsByNextStage;
    std::vector<TileId> priorityTileIds;
    std::optional<FeatureLayerSearchRequest> searchRequest;
    bool usesStageBuckets = false;
};

void inheritSearchFields(nlohmann::json& requestJson, const nlohmann::json& envelopeJson);

ParsedLayerTilesRequest parseLayerTilesRequestJson(const nlohmann::json& requestJson);

std::vector<MapTileKey> expandLayerTilesRequestKeys(
    const ParsedLayerTilesRequest& request,
    LayerType layerType,
    uint32_t stageCount);

}  // namespace mapget::detail
