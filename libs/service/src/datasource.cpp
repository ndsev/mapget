#include "datasource.h"

namespace mapget
{

TileFeatureLayer::Ptr DataSource::get(const MapTileKey& k, Cache& cache, DataSourceInfo const& info)
{
    auto result = cache.getTileFeatureLayer(k, info);
    if (result)
        return result;
    auto it = info.layers_.find(k.layerId_);
    if (it == info.layers_.end())
        throw std::runtime_error(stx::format("Could not find layer {}.", k.layerId_));
    result = std::make_shared<TileFeatureLayer>(
        k.tileId_,
        info.nodeId_,
        info.mapId_,
        it->second,
        cache(info.nodeId_));
    fill(result);
    return result;
}

}