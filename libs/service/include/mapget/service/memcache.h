#pragma once

#include "cache.h"

#include <unordered_map>
#include <deque>
#include <shared_mutex>

namespace mapget
{

/**
 * Simple in-memory mapget cache implementation.
 */
class MemCache : public Cache
{
public:
    using Ptr = std::shared_ptr<Cache>;

    /**
     * Construct a cache, and indicate the max number of cached tiles.
     * If the limit is reached, tiles are evicted in FIFO order.
     */
    MemCache(uint32_t maxCachedTiles=1024);

    /** Retrieve a TileLayer blob for a MapTileKey. */
    std::optional<std::string> getTileLayer(MapTileKey const& k) override;

    /** Upsert a TileLayer blob. */
    void putTileLayer(MapTileKey const& k, std::string const& v) override;

    /** Retrieve a Fields-dict-blob for a sourceNodeId -> No-Op */
    std::optional<std::string> getFields(std::string const& sourceNodeId) override {return {};}

    /** Upsert a Fields-dict blob. -> No-Op */
    void putFields(std::string const& sourceNodeId, std::string const& v) override {}

private:
    // Cached tile blobs.
    std::shared_mutex cacheMutex_;
    std::unordered_map<std::string, std::string> cachedTiles_;
    std::deque<std::string> fifo_;
    uint32_t maxCachedTiles_ = 0;
};

}