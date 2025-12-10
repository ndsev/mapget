#include "memcache.h"
#include "mapget/log.h"

namespace mapget
{

MemCache::MemCache(uint32_t maxCachedTiles) : maxCachedTiles_(maxCachedTiles) {}

std::optional<std::string> MemCache::getTileLayerBlob(const MapTileKey& k)
{
    std::shared_lock cacheLock(cacheMutex_);
    auto cacheIt = cachedTiles_.find(k.toString());
    if (cacheIt != cachedTiles_.end())
        return cacheIt->second;
    return {};
}

void MemCache::putTileLayerBlob(const MapTileKey& k, const std::string& v)
{
    std::unique_lock cacheLock(cacheMutex_);
    auto ks = k.toString();
    // Remove any existing entry for this key from the FIFO to avoid duplicates.
    fifo_.erase(std::remove(fifo_.begin(), fifo_.end(), ks), fifo_.end());
    fifo_.push_front(ks);
    cachedTiles_[ks] = v;
    while (fifo_.size() > maxCachedTiles_) {
        auto oldestTileKey = fifo_.back();
        fifo_.pop_back();
        log().debug("Evicting tile from cache: {}", oldestTileKey);
        cachedTiles_.erase(oldestTileKey);
    }
}

nlohmann::json MemCache::getStatistics() const {
    auto result = Cache::getStatistics();
    result["memcache-map-size"] = (int64_t)cachedTiles_.size();
    result["memcache-fifo-size"] = (int64_t)fifo_.size();
    return result;
}

}