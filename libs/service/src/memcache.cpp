#include "memcache.h"
#include "mapget/log.h"

#include <algorithm>

namespace mapget
{

MemCache::MemCache(uint32_t maxCachedTiles)
    : MemCache(maxCachedTiles, defaultMaxCachedBytes(maxCachedTiles))
{}

MemCache::MemCache(uint32_t maxCachedTiles, uint64_t maxCachedBytes)
    : maxCachedTiles_(maxCachedTiles),
      maxCachedBytes_(maxCachedBytes)
{}

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
    if (auto existing = cachedTiles_.find(ks); existing != cachedTiles_.end()) {
        cachedTileBytes_ -= existing->second.size();
        cachedTiles_.erase(existing);
    }

    // An entry that cannot fit by itself would otherwise evict useful cache
    // contents before being evicted in turn.
    if (maxCachedBytes_ > 0 && v.size() > maxCachedBytes_) {
        log().debug(
            "Not caching oversized tile {} ({} bytes exceeds {} byte limit)",
            ks,
            v.size(),
            maxCachedBytes_);
        return;
    }

    fifo_.push_front(ks);
    auto [inserted, _] = cachedTiles_.emplace(ks, v);
    cachedTileBytes_ += inserted->second.size();
    while ((maxCachedTiles_ > 0 && fifo_.size() > maxCachedTiles_) ||
           (maxCachedBytes_ > 0 && cachedTileBytes_ > maxCachedBytes_))
    {
        auto oldestTileKey = fifo_.back();
        fifo_.pop_back();
        log().debug("Evicting tile from cache: {}", oldestTileKey);
        if (auto oldest = cachedTiles_.find(oldestTileKey);
            oldest != cachedTiles_.end())
        {
            cachedTileBytes_ -= oldest->second.size();
            cachedTiles_.erase(oldest);
        }
    }
}

void MemCache::eraseTileLayerBlob(MapTileKey const& k)
{
    std::unique_lock cacheLock(cacheMutex_);
    auto const key = k.toString();
    if (auto existing = cachedTiles_.find(key); existing != cachedTiles_.end()) {
        cachedTileBytes_ -= existing->second.size();
        cachedTiles_.erase(existing);
    }
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
    result["memcache-tile-bytes"] = cachedTileBytes_;
    result["memcache-max-tiles"] = maxCachedTiles_;
    result["memcache-max-bytes"] = maxCachedBytes_;
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
