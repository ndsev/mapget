#pragma once

#include "mapget/model/layer.h"

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
    bool usesStageBuckets = false;
};

ParsedLayerTilesRequest parseLayerTilesRequestJson(const nlohmann::json& requestJson);

std::vector<MapTileKey> expandLayerTilesRequestKeys(
    const ParsedLayerTilesRequest& request,
    LayerType layerType,
    uint32_t stageCount);

}  // namespace mapget::detail
