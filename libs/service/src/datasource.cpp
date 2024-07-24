#include "datasource.h"

namespace mapget
{

TileFeatureLayer::Ptr DataSource::get(const MapTileKey& k, Cache::Ptr& cache, DataSourceInfo const& info)
{
    auto result = std::make_shared<TileFeatureLayer>(
        k.tileId_,
        info.nodeId_,
        info.mapId_,
        info.getLayer(k.layerId_),
        cache->getFieldDict(info.nodeId_));
    fill(result);
    return result;
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
