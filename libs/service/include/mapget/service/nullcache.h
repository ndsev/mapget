#pragma once

#include "cache.h"

namespace mapget
{

/**
 * A no-op cache implementation that doesn't store anything.
 * All requests result in cache misses.
 */
class NullCache : public Cache
{
public:
    using Ptr = std::shared_ptr<Cache>;

    /** Retrieve a TileLayer blob for a MapTileKey - always returns empty. */
    std::optional<std::string> getTileLayerBlob(MapTileKey const& k) override;

    /** Upsert a TileLayer blob - does nothing. */
    void putTileLayerBlob(MapTileKey const& k, std::string const& v) override;

    /** Iterate cached tile blobs - no-op. */
    void forEachTileLayerBlob(const TileBlobVisitor& cb) const override;

    /** Retrieve a string-pool blob for a sourceNodeId - always returns empty. */
    std::optional<std::string> getStringPoolBlob(std::string_view const& sourceNodeId) override;

    /** Upsert a string-pool blob - does nothing. */
    void putStringPoolBlob(std::string_view const& sourceNodeId, std::string const& v) override;
};

}
