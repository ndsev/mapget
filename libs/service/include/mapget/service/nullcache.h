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

    /** Remove a TileLayer blob - does nothing. */
    void eraseTileLayerBlob(MapTileKey const& k) override;

    /** Iterate cached tile blobs - no-op. */
    void forEachTileLayerBlob(const TileBlobVisitor& cb) const override;

    /** Retrieve a string-pool blob for a sourceStringPoolId - always returns empty. */
    std::optional<std::string> getStringPoolBlob(std::string_view const& sourceStringPoolId) override;

    /** Upsert a string-pool blob - does nothing. */
    void putStringPoolBlob(std::string_view const& sourceStringPoolId, std::string const& v) override;
};

}
