#pragma once

#include "cache.h"

#include <unordered_map>
#include <shared_mutex>

namespace mapget
{

/**
 * Simple in-memory mapget cache implementation.
 *  TODO: Allow limiting number of cached tiles.
 */
class MemCache : public Cache
{
public:
    using Ptr = std::shared_ptr<Cache>;

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
};

}