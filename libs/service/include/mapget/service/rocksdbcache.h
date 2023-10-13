#pragma once

#include "cache.h"

namespace mapget
{

class RocksDBCache : public Cache
{
public:
    RocksDBCache();

    std::optional<std::string> getTileLayer(MapTileKey const& k);
    void putTileLayer(MapTileKey const& k, std::string const& v);
    std::optional<std::string> getFields(std::string_view const& sourceNodeId);
    void putFields(std::string_view const& sourceNodeId, std::string const& v);
};

}