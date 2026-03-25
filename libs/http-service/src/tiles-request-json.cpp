#include "tiles-request-json.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace mapget::detail
{

ParsedLayerTilesRequest parseLayerTilesRequestJson(const nlohmann::json& requestJson)
{
    ParsedLayerTilesRequest result;
    result.mapId = requestJson.at("mapId").get<std::string>();
    result.layerId = requestJson.at("layerId").get<std::string>();

    if (auto stagedIt = requestJson.find("tileIdsByNextStage");
        stagedIt != requestJson.end())
    {
        result.usesStageBuckets = true;
        if (!stagedIt->is_array()) {
            throw std::runtime_error("tileIdsByNextStage must be an array");
        }
        result.tileIdsByNextStage.reserve(stagedIt->size());
        for (auto const& bucketJson : *stagedIt) {
            if (!bucketJson.is_array()) {
                throw std::runtime_error("tileIdsByNextStage entries must be arrays");
            }
            std::vector<TileId> bucket;
            bucket.reserve(bucketJson.size());
            for (auto const& tileIdJson : bucketJson) {
                bucket.emplace_back(tileIdJson.get<uint64_t>());
            }
            result.tileIdsByNextStage.push_back(std::move(bucket));
        }
        return result;
    }

    auto const& tileIdsJson = requestJson.at("tileIds");
    if (!tileIdsJson.is_array()) {
        throw std::runtime_error("tileIds must be an array");
    }

    std::vector<TileId> tileIds;
    tileIds.reserve(tileIdsJson.size());
    for (auto const& tileIdJson : tileIdsJson) {
        tileIds.emplace_back(tileIdJson.get<uint64_t>());
    }
    result.tileIdsByNextStage.push_back(std::move(tileIds));
    return result;
}

std::vector<MapTileKey> expandLayerTilesRequestKeys(
    const ParsedLayerTilesRequest& request,
    LayerType layerType,
    uint32_t stageCount)
{
    std::vector<MapTileKey> result;
    std::set<MapTileKey> seen;

    if (!request.usesStageBuckets) {
        if (!request.tileIdsByNextStage.empty()) {
            for (auto const& tileId : request.tileIdsByNextStage.front()) {
                MapTileKey key(
                    layerType,
                    request.mapId,
                    request.layerId,
                    tileId,
                    UnspecifiedStage);
                if (seen.insert(key).second) {
                    result.push_back(std::move(key));
                }
            }
        }
        return result;
    }

    auto const normalizedStageCount = std::max<uint32_t>(1U, stageCount);
    for (uint32_t stage = 0; stage < normalizedStageCount; ++stage) {
        for (size_t bucketIndex = 0; bucketIndex < request.tileIdsByNextStage.size(); ++bucketIndex) {
            auto const nextMissingStage = static_cast<uint32_t>(bucketIndex);
            if (nextMissingStage > stage || nextMissingStage >= normalizedStageCount) {
                continue;
            }
            for (auto const& tileId : request.tileIdsByNextStage[bucketIndex]) {
                MapTileKey key(layerType, request.mapId, request.layerId, tileId, stage);
                if (seen.insert(key).second) {
                    result.push_back(std::move(key));
                }
            }
        }
    }

    return result;
}

}  // namespace mapget::detail
