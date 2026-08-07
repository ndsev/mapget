#include "memcache.h"
#include "mapget/log.h"

#include <algorithm>

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

void MemCache::eraseTileLayerBlob(MapTileKey const& k)
{
    std::unique_lock cacheLock(cacheMutex_);
    auto const key = k.toString();
    cachedTiles_.erase(key);
    std::erase(fifo_, key);
}

void MemCache::forEachTileLayerBlob(const TileBlobVisitor& cb) const
{
    std::shared_lock cacheLock(cacheMutex_);
    for (const auto& [key, value] : cachedTiles_) {
        cb(MapTileKey(key), value);
    }
}

nlohmann::json MemCache::getStatistics() const
{
    auto result = Cache::getStatistics();
    std::shared_lock cacheLock(cacheMutex_);
    auto memory = memoryUsageLocked();
    auto const allocated = memory.total().allocatedBytes;
    auto peak = peakAllocatedBytes_.load(std::memory_order_relaxed);
    while (peak < allocated &&
           !peakAllocatedBytes_.compare_exchange_weak(
               peak,
               allocated,
               std::memory_order_relaxed)) {
    }
    auto memoryJson = memory.toJson();
    memoryJson["sampled-peak-allocated-bytes"] =
        peakAllocatedBytes_.load(std::memory_order_relaxed);
    result["memcache-map-size"] = static_cast<int64_t>(cachedTiles_.size());
    result["memcache-fifo-size"] = static_cast<int64_t>(fifo_.size());
    result["memory"]["tile-blobs"] = std::move(memoryJson);
    return result;
}

MemoryUsageBreakdown MemCache::memoryUsageLocked() const
{
    MemoryUsageBreakdown result;
    result.add("hash-index", {
        cachedTiles_.size() * sizeof(decltype(cachedTiles_)::value_type),
        cachedTiles_.bucket_count() * sizeof(void*) +
            cachedTiles_.size() *
                (sizeof(decltype(cachedTiles_)::value_type) + 2 * sizeof(void*)),
    });
    for (auto const& [key, value] : cachedTiles_) {
        result.add("hash-keys", stringMemoryUsage(key));
        result.add("serialized-tile-blobs", stringMemoryUsage(value));
    }
    // std::deque does not expose block capacity. Occupied element storage is a
    // conservative lower bound; spare implementation blocks are omitted.
    result.add("fifo-elements", {
        fifo_.size() * sizeof(std::string),
        fifo_.size() * sizeof(std::string),
    });
    for (auto const& key : fifo_) {
        result.add("fifo-keys", stringMemoryUsage(key));
    }
    return result;
}

}
