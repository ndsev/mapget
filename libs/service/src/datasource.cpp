#include "datasource.h"
#include <memory>
#include <stdexcept>
#include <chrono>
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

    auto start = std::chrono::steady_clock::now();
    switch (layerInfo->type_) {
    case mapget::LayerType::Features: {
        auto tileFeatureLayer = std::make_shared<TileFeatureLayer>(
            k.tileId_,
            info.nodeId_,
            info.mapId_,
            info.getLayer(k.layerId_),
            cache->getStringPool(info.nodeId_));
        fill(tileFeatureLayer);
        result = tileFeatureLayer;
        break;
    }
    case mapget::LayerType::SourceData: {
        auto tileSourceDataLayer = std::make_shared<TileSourceDataLayer>(
            k.tileId_,
            info.nodeId_,
            info.mapId_,
            info.getLayer(k.layerId_),
            cache->getStringPool(info.nodeId_));
        fill(tileSourceDataLayer);
        result = tileSourceDataLayer;
        break;
    }
    default:
        break;
    }

    // Notify the tile how long it took to fill.
    if (result) {
        auto duration = std::chrono::steady_clock::now() - start;
        result->setInfo("fill-time-ms", std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
    }
    return result;
}

void DataSource::requireAuthHeaderRegexMatchOption(std::string header, std::regex re)
{
    authHeaderAlternatives_.insert({std::move(header), std::move(re)});
}

bool DataSource::isDataSourceAuthorized(
    AuthHeaders const& clientHeaders) const
{
    if (authHeaderAlternatives_.empty())
        return true;

    for (auto const& [k, v] : clientHeaders) {
        auto authHeaderPatternIt = authHeaderAlternatives_.find(k);
        if (authHeaderPatternIt != authHeaderAlternatives_.end()) {
            if (std::regex_match(v, authHeaderPatternIt->second)) {
                return true;
            }
        }
    }

    return false;
}

StringId DataSource::cachedStringPoolOffset(const std::string& nodeId, Cache::Ptr const& cache)
{
    return cache->cachedStringPoolOffset(nodeId);
}

std::vector<LocateResponse> DataSource::locate(const LocateRequest& req)
{
    return {};
}

}
