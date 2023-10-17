#pragma once

#include "cache.h"

#include <rocksdb/db.h>

namespace mapget
{

class RocksDBCache : public Cache
{
public:
    RocksDBCache();
    ~RocksDBCache();

    std::optional<std::string> getTileLayer(MapTileKey const& k);
    void putTileLayer(MapTileKey const& k, std::string const& v);
    std::optional<std::string> getFields(std::string_view const& sourceNodeId);
    void putFields(std::string_view const& sourceNodeId, std::string const& v);

private:
    rocksdb::DB* db_;
    rocksdb::Options options_;
    rocksdb::WriteOptions write_options_;
    rocksdb::ReadOptions read_options_;
    std::vector<rocksdb::ColumnFamilyHandle*> column_family_handles_;
};

}