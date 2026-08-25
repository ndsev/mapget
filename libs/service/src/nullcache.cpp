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

void NullCache::eraseTileLayerBlob(MapTileKey const& k)
{
    // No cached tiles.
}

void NullCache::forEachTileLayerBlob(const TileBlobVisitor& cb) const
{
    // No cached tiles.
}

std::optional<std::string> NullCache::getStringPoolBlob(std::string_view const& sourceStringPoolId)
{
    return std::nullopt;
}

void NullCache::putStringPoolBlob(std::string_view const& sourceStringPoolId, std::string const& v)
{
    // Do nothing - no caching
}

}
