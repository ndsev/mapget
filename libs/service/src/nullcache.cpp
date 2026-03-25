#include "mapget/service/nullcache.h"

namespace mapget
{

std::optional<std::string> NullCache::getTileLayerBlob(MapTileKey const& k)
{
    return std::nullopt;
}

void NullCache::putTileLayerBlob(MapTileKey const& k, std::string const& v)
{
    // Do nothing - no caching
}

void NullCache::forEachTileLayerBlob(const TileBlobVisitor& cb) const
{
    // No cached tiles.
}

std::optional<std::string> NullCache::getStringPoolBlob(std::string_view const& sourceNodeId)
{
    return std::nullopt;
}

void NullCache::putStringPoolBlob(std::string_view const& sourceNodeId, std::string const& v)
{
    // Do nothing - no caching
}

}
