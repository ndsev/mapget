#include "rocksdbcache.h"
#include <rocksdb/db.h>
#include <iostream>

namespace mapget
{

RocksDBCache::RocksDBCache()
{
    auto db_path = "cache-rocksdb";
    options_.create_if_missing = true;
    // Set TTL to 1 hour (adjust as needed).
    options_.WAL_ttl_seconds = 3600;

    // Open the database.
    rocksdb::Status status = rocksdb::DB::Open(options_, db_path, &db_);
    if (!status.ok()) {
        std::cerr << "Error opening database: " << status.ToString() << std::endl;
    }
}

RocksDBCache::~RocksDBCache() {
    delete db_;
}

std::optional<std::string> RocksDBCache::getTileLayer(MapTileKey const& k) {
    std::string read_value;
    auto status = db_->Get(read_options_, k.toString(), &read_value);
    if (status.ok()) {
        std::cout << "Key: " << k.toString() << " | Value: " << read_value << std::endl;
        return read_value;
    } else {
        std::cerr << "Error reading from database: " << status.ToString() << std::endl;
        return {};
    }
}

void RocksDBCache::putTileLayer(MapTileKey const& k, std::string const& v) {
    auto status = db_->Put(write_options_, k.toString(), v);
    if (!status.ok()) {
        std::cerr << "Error writing to database: " << status.ToString() << std::endl;
    }
}

std::optional<std::string> RocksDBCache::getFields(std::string_view const& sourceNodeId) {

}

void RocksDBCache::putFields(std::string_view const& sourceNodeId, std::string const& v) {

}


}