#include <rocksdb/db.h>
#include <iostream>

#include "rocksdbcache.h"
#include "mapget/log.h"

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
        logRuntimeError(stx::format("Error opening database: {}", status.ToString()));
    }
}

RocksDBCache::~RocksDBCache() {
    delete db_;
}

std::optional<std::string> RocksDBCache::getTileLayer(MapTileKey const& k) {
    std::string read_value;
    auto status = db_->Get(read_options_, k.toString(), &read_value);
    if (status.ok()) {
        log().trace(stx::format("Key: {} | Value: {}", k.toString(), read_value));
        return read_value;
    }

    logRuntimeError(stx::format("Error reading from database: {}", status.ToString()));
    return {};
}

void RocksDBCache::putTileLayer(MapTileKey const& k, std::string const& v) {
    auto status = db_->Put(write_options_, k.toString(), v);
    if (!status.ok()) {
        logRuntimeError(stx::format("Error writing to database: {}", status.ToString()));
    }
}

std::optional<std::string> RocksDBCache::getFields(std::string_view const& sourceNodeId) {

}

void RocksDBCache::putFields(std::string_view const& sourceNodeId, std::string const& v) {

}


}