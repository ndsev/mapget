#pragma once

#include "cache.h"

#ifdef MAPGET_WITH_ROCKSDB
#include <rocksdb/db.h>
#endif

namespace mapget
{

#ifdef MAPGET_WITH_ROCKSDB

/**
 * A persistent cache implementation that stores layers and string pools
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
    std::optional<std::string> getStringPoolBlob(std::string_view const& sourceNodeId) override;
    void putStringPoolBlob(std::string_view const& sourceNodeId, std::string const& v) override;

private:
    rocksdb::DB* db_{};
    rocksdb::Options options_;
    rocksdb::WriteOptions write_options_;
    rocksdb::ReadOptions read_options_;
    std::vector<rocksdb::ColumnFamilyHandle*> column_family_handles_;
    uint32_t key_count_ = 0;
    uint32_t max_key_count_ = 0;
};

#else

/**
 * Stub implementation when RocksDB is disabled at compile time.
 * Throws an exception when instantiated.
 */
class RocksDBCache : public Cache
{
public:
    explicit RocksDBCache(
        uint32_t cacheMaxTiles = 1024,
        std::string cachePath = "mapget-cache",
        bool clearCache = false)
    {
        throw std::runtime_error("RocksDB support was disabled at compile time. Use -DMAPGET_WITH_ROCKSDB=ON to enable it.");
    }

    std::optional<std::string> getTileLayerBlob(MapTileKey const& k) override { return {}; }
    void putTileLayerBlob(MapTileKey const& k, std::string const& v) override {}
    std::optional<std::string> getStringPoolBlob(std::string_view const& sourceNodeId) override { return {}; }
    void putStringPoolBlob(std::string_view const& sourceNodeId, std::string const& v) override {}
};

#endif

}  // namespace mapget