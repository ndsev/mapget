#include <rocksdb/db.h>
#include <iostream>
#include <filesystem>

#include "rocksdbcache.h"
#include "mapget/log.h"

namespace mapget
{

RocksDBCache::RocksDBCache()
{
    auto& db_path = "cache-rocksdb";
    auto current_path = std::filesystem::current_path();
    auto absolute_db_path = current_path / db_path;

    options_.create_if_missing = true;
    options_.create_missing_column_families = true;
    // TODO configurable TTL.
    options_.WAL_ttl_seconds = 3600;

    // Create separate column families for tile layers and fields.
    // RocksDB requires a default column family, setting this to tile layers.
    std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
    column_families.push_back(
        rocksdb::ColumnFamilyDescriptor(
            ROCKSDB_NAMESPACE::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions()));
    column_families.push_back(
        rocksdb::ColumnFamilyDescriptor(
            "FieldDicts", rocksdb::ColumnFamilyOptions()));

    // Open the database.
    rocksdb::Status status = rocksdb::DB::Open(
        options_, absolute_db_path, column_families, &column_family_handles_, &db_);

    if (!status.ok()) {
        logRuntimeError(stx::format(
            "Error opening database at {}: {}", absolute_db_path.string(), status.ToString()));
    }
}

RocksDBCache::~RocksDBCache() {
    for (auto handle : column_family_handles_) {
        db_->DestroyColumnFamilyHandle(handle);
    }
    delete db_;
}

std::optional<std::string> RocksDBCache::getTileLayer(MapTileKey const& k) {
    std::string read_value;
    auto status = db_->Get(
        read_options_, column_family_handles_[0], k.toString(), &read_value);

    if (status.ok()) {
        log().trace(stx::format("Key: {} | Layer size: {}", k.toString(), read_value.size()));
        return read_value;
    }

    logRuntimeError(stx::format("Error reading from database: {}", status.ToString()));
    return {};
}

void RocksDBCache::putTileLayer(MapTileKey const& k, std::string const& v) {
    auto status = db_->Put(write_options_, column_family_handles_[0], k.toString(), v);

    if (!status.ok()) {
        logRuntimeError(stx::format("Error writing to database: {}", status.ToString()));
    }
}

std::optional<std::string> RocksDBCache::getFields(std::string_view const& sourceNodeId) {
    std::string read_value;
    auto status = db_->Get(
        read_options_, column_family_handles_[1], sourceNodeId, &read_value);

    if (status.ok()) {
        log().trace(stx::format("Node: {} | Field dict size: {}", sourceNodeId, read_value.size()));
        return read_value;
    }

    logRuntimeError(stx::format("Error reading from database: {}", status.ToString()));
    return {};
}

void RocksDBCache::putFields(std::string_view const& sourceNodeId, std::string const& v) {
    auto status = db_->Put(write_options_, column_family_handles_[1], sourceNodeId, v);

    if (!status.ok()) {
        logRuntimeError(stx::format("Error writing to database: {}", status.ToString()));
    }
}


}