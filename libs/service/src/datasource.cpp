#include "datasource.h"
#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <chrono>
#include <vector>
#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/info.h"

namespace mapget
{

bool addAuthHeaderRegexMatchOption(
    AuthHeaderRegexMap& alternatives,
    std::string header,
    std::regex re)
{
    std::ranges::transform(
        header,
        header.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return alternatives.emplace(
        std::move(header),
        std::move(re)).second;
}

bool authHeadersMatch(
    AuthHeaderRegexMap const& alternatives,
    AuthHeaders const& clientHeaders)
{
    if (alternatives.empty()) {
        return true;
    }

    for (auto const& [name, value] : clientHeaders) {
        auto normalizedName = name;
        std::ranges::transform(
            normalizedName,
            normalizedName.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
        auto pattern = alternatives.find(normalizedName);
        if (pattern != alternatives.end() &&
            std::regex_match(value, pattern->second))
        {
            return true;
        }
    }
    return false;
}

TileLayer::Ptr DataSource::get(
    const MapTileKey& k,
    Cache::Ptr& cache,
    DataSourceInfo const& info,
    TileLayer::LoadStateCallback loadStateCallback)
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
            info.stringPoolId_,
            info.mapId_,
            info.getLayer(k.layerId_),
            cache->getStringPool(info.stringPoolId_));
        if (loadStateCallback) {
            tileFeatureLayer->setLoadStateCallback(loadStateCallback);
        }
        fill(tileFeatureLayer);
        result = tileFeatureLayer;
        break;
    }
    case mapget::LayerType::SourceData: {
        auto tileSourceDataLayer = std::make_shared<TileSourceDataLayer>(
            k.tileId_,
            info.stringPoolId_,
            info.mapId_,
            info.getLayer(k.layerId_),
            cache->getStringPool(info.stringPoolId_));
        if (loadStateCallback) {
            tileSourceDataLayer->setLoadStateCallback(loadStateCallback);
        }
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
        result->setInfo("Load+Convert/Total#ms", std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
    }
    return result;
}

void DataSource::requireAuthHeaderRegexMatchOption(std::string header, std::regex re)
{
    addAuthHeaderRegexMatchOption(
        authHeaderAlternatives_,
        std::move(header),
        std::move(re));
}

bool DataSource::isDataSourceAuthorized(
    AuthHeaders const& clientHeaders) const
{
    return authHeadersMatch(
        authHeaderAlternatives_,
        clientHeaders);
}

StringId DataSource::cachedStringPoolOffset(const std::string& stringPoolId, Cache::Ptr const& cache)
{
    return cache->cachedStringPoolOffset(stringPoolId);
}

void DataSource::setTtl(std::optional<std::chrono::milliseconds> ttl)
{
    ttl_ = ttl;
}

std::optional<std::chrono::milliseconds> DataSource::ttl() const
{
    return ttl_;
}

std::vector<LocateCandidate> DataSource::locate(
    LocateRequest const&)
{
    return {};
}

std::optional<AttachmentResponse> DataSource::attachment(
    AttachmentRequest const&)
{
    return {};
}

std::optional<uint64_t> DataSource::estimatedRetainedMemoryBytes() const
{
    return std::nullopt;
}

}
