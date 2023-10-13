#include "rocksdbcache.h"
#include <rocksdb/db.h>
#include <iostream>

namespace mapget
{

RocksDBCache::RocksDBCache()
{
    // TODO return the database instance.
    // This is just a dummy test.

    auto db_path = "cache-rocksdb";

    rocksdb::Options options;
    options.create_if_missing = true;
    options.WAL_ttl_seconds = 3600;  // Set TTL to 1 hour (adjust as needed).

    // Open the database.
    rocksdb::DB* db;
    rocksdb::Status status = rocksdb::DB::Open(options, db_path, &db);
    if (!status.ok()) {
        std::cerr << "Error opening database: " << status.ToString() << std::endl;
    }

    // Define a key and value.
    std::string key = "dummy_key";
    std::string value = "dummy_value";

    // Put the key-value pair into RocksDB.
    rocksdb::WriteOptions write_options;
    status = db->Put(write_options, key, value);
    if (!status.ok()) {
        std::cerr << "Error writing to database: " << status.ToString() << std::endl;
    }

    // Read the value from RocksDB.
    rocksdb::ReadOptions read_options;
    std::string read_value;
    status = db->Get(read_options, key, &read_value);
    if (status.ok()) {
        std::cout << "Key: " << key << " | Value: " << read_value << std::endl;
    } else {
        std::cerr << "Error reading from database: " << status.ToString() << std::endl;
    }

    // TODO destructor.
    // Close the database.
    delete db;
}

std::optional<std::string> RocksDBCache::getTileLayer(MapTileKey const& k) {

}

void RocksDBCache::putTileLayer(MapTileKey const& k, std::string const& v) {

}

std::optional<std::string> RocksDBCache::getFields(std::string_view const& sourceNodeId) {

}

void RocksDBCache::putFields(std::string_view const& sourceNodeId, std::string const& v) {

}


}