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
    std::optional<std::string> getTileLayerBlob(MapTileKey const& k) override;

    /** Upsert a TileLayer blob. */
    void putTileLayerBlob(MapTileKey const& k, std::string const& v) override;

    /** Retrieve a Fields-dict-blob for a sourceNodeId -> No-Op */
    std::optional<std::string> getFieldsBlob(std::string_view const& sourceNodeId) override {return {};}

    /** Upsert a Fields-dict blob. -> No-Op */
    void putFieldsBlob(std::string_view const& sourceNodeId, std::string const& v) override {}

    /** Enriches the statistics with info about the number of cached tiles. */
    nlohmann::json getStatistics() const override;

private:
    // Cached tile blobs.
    std::shared_mutex cacheMutex_;
    std::unordered_map<std::string, std::string> cachedTiles_;
    std::deque<std::string> fifo_;
    uint32_t maxCachedTiles_ = 0;
};

}