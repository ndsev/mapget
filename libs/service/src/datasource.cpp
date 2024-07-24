#include "datasource.h"
#include <memory>
#include <stdexcept>
#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/info.h"

namespace mapget
{

TileLayer::Ptr DataSource::get(const MapTileKey& k, Cache::Ptr& cache, DataSourceInfo const& info)
{
    auto layerInfo = info.getLayer(k.layerId_);
    if (!layerInfo)
        throw std::runtime_error("Layer info is null");

    auto result = TileLayer::Ptr{};
    switch (layerInfo->type_) {
    case mapget::LayerType::Features: {
        auto result = std::make_shared<TileFeatureLayer>(
            k.tileId_,
            info.nodeId_,
            info.mapId_,
            info.getLayer(k.layerId_),
            cache->getFieldDict(info.nodeId_));
        fill(result);
        return result;
    }
    case mapget::LayerType::SourceData: {
        auto result = std::make_shared<TileSourceDataLayer>(
            k.tileId_,
            info.nodeId_,
            info.mapId_,
            info.getLayer(k.layerId_),
            cache->getFieldDict(info.nodeId_));
        fill(result);
        return result;
    }
    default:
        return nullptr;
    }
}

simfil::StringId DataSource::cachedFieldsOffset(const std::string& nodeId, Cache::Ptr const& cache)
{
    return cache->cachedFieldsOffset(nodeId);
}

std::vector<LocateResponse> DataSource::locate(const LocateRequest& req)
{
    return {};
}

}
