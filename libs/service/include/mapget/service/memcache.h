#pragma once

#include "cache.h"

#include <unordered_map>
#include <deque>
#include <shared_mutex>
#include <atomic>
#include <cstdint>

namespace mapget
{

/**
 * Simple in-memory mapget cache implementation.
 */
class MemCache : public Cache
{
public:
    using Ptr = std::shared_ptr<Cache>;
    static constexpr uint64_t DefaultBytesPerTile = 512ULL * 1024ULL;

    /**
     * Construct a cache with a tile limit and the default 512 KiB byte budget
     * per permitted tile. Zero tiles disables both derived limits.
     */
    explicit MemCache(uint32_t maxCachedTiles=1024);

    /** Construct a cache with independent tile-count and serialized-byte limits. */
    MemCache(uint32_t maxCachedTiles, uint64_t maxCachedBytes);

    /** Derive the default byte budget for a configured tile-count limit. */
    [[nodiscard]] static constexpr uint64_t defaultMaxCachedBytes(
        uint32_t maxCachedTiles)
    {
        return static_cast<uint64_t>(maxCachedTiles) * DefaultBytesPerTile;
    }

    /** Retrieve a TileLayer blob for a MapTileKey. */
    std::optional<std::string> getTileLayerBlob(MapTileKey const& k) override;

    /** Upsert a TileLayer blob. */
    void putTileLayerBlob(MapTileKey const& k, std::string const& v) override;

    /** Remove a TileLayer blob. */
    void eraseTileLayerBlob(MapTileKey const& k) override;

    /** Iterate over cached tile layer blobs. */
    void forEachTileLayerBlob(const TileBlobVisitor& cb) const override;

    /** Retrieve a string-pool blob for a sourceStringPoolId -> No-Op */
    std::optional<std::string> getStringPoolBlob(std::string_view const& sourceStringPoolId) override {return {};}

    /** Upsert a string-pool blob. -> No-Op */
    void putStringPoolBlob(std::string_view const& sourceStringPoolId, std::string const& v) override {}

    /** Enriches the statistics with info about the number of cached tiles. */
    nlohmann::json getStatistics() const override;

private:
    /** Measure all in-memory blob/index ownership while cacheMutex_ is held. */
    [[nodiscard]] MemoryUsageBreakdown memoryUsageLocked() const;

    // Cached tile blobs.
    mutable std::shared_mutex cacheMutex_;
    std::unordered_map<std::string, std::string> cachedTiles_;
    std::deque<std::string> fifo_;
    uint32_t maxCachedTiles_ = 0;
    uint64_t maxCachedBytes_ = 0;
    uint64_t cachedTileBytes_ = 0;
    /** High-water mark across explicit statistics samples. */
    mutable std::atomic_size_t peakAllocatedBytes_{0};
};

}
