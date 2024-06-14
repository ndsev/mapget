#pragma once

#include "cache.h"

#include <rocksdb/db.h>

namespace mapget
{

/**
 * A persistent cache implementation that stores layers and field dictionaries
 * in RocksDB. Oldest tiles are removed automatically in FIFO order when cacheMaxTiles is
 * reached.
 */
class RocksDBCache : public Cache
{
public:
    explicit RocksDBCache(
        uint32_t cacheMaxTiles = 1024,
        std::string cachePath = "mapget-cache",
        bool clearCache = false);
    ~RocksDBCache() override;

    std::optional<std::string> getTileLayerBlob(MapTileKey const& k) override;
    void putTileLayerBlob(MapTileKey const& k, std::string const& v) override;
    std::optional<std::string> getFieldsBlob(std::string_view const& sourceNodeId) override;
    void putFieldsBlob(std::string_view const& sourceNodeId, std::string const& v) override;

private:
    rocksdb::DB* db_{};
    rocksdb::Options options_;
    rocksdb::WriteOptions write_options_;
    rocksdb::ReadOptions read_options_;
    std::vector<rocksdb::ColumnFamilyHandle*> column_family_handles_;
    uint32_t key_count_ = 0;
    uint32_t max_key_count_ = 0;
};

}  // namespace mapget