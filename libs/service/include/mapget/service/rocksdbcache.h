#pragma once

#include "cache.h"

#include <rocksdb/db.h>

namespace mapget
{

class RocksDBCache : public Cache
{
public:
    explicit RocksDBCache(
        int64_t cacheMaxTiles = 1024,
        std::string cachePath = "mapget-cache",
        bool clearCache = false);
    ~RocksDBCache() override;

    std::optional<std::string> getTileLayer(MapTileKey const& k) override;
    void putTileLayer(MapTileKey const& k, std::string const& v) override;
    std::optional<std::string> getFields(std::string_view const& sourceNodeId) override;
    void putFields(std::string_view const& sourceNodeId, std::string const& v) override;

private:
    rocksdb::DB* db_{};
    rocksdb::Options options_;
    rocksdb::WriteOptions write_options_;
    rocksdb::ReadOptions read_options_;
    std::vector<rocksdb::ColumnFamilyHandle*> column_family_handles_;
    uint32_t key_count_ = 0;
};

}  // namespace mapget