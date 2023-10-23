#include <rocksdb/db.h>
#include <filesystem>
#include <iostream>

#include "mapget/log.h"
#include "rocksdbcache.h"

namespace mapget
{

static uint8_t COL_TIMESTAMP = 0;
static uint8_t COL_TILES = 1;
static uint8_t COL_FIELD_DICTS = 2;

RocksDBCache::RocksDBCache(uint32_t cacheMaxTiles, std::string cachePath, bool clearCache)
    : max_key_count_(cacheMaxTiles)
{
    // Set standard RocksDB options.
    options_.create_if_missing = true;
    options_.create_missing_column_families = true;

    // Create separate column families for tile layers and fields.
    // RocksDB requires a default column family, we use that for
    // the timestamp->tile column.
    std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
    column_families.push_back(rocksdb::ColumnFamilyDescriptor(
        ROCKSDB_NAMESPACE::kDefaultColumnFamilyName,
        rocksdb::ColumnFamilyOptions()));
    column_families.push_back(rocksdb::ColumnFamilyDescriptor(
        "Tiles",
        rocksdb::ColumnFamilyOptions()));
    column_families.push_back(rocksdb::ColumnFamilyDescriptor(
        "FieldDicts",
        rocksdb::ColumnFamilyOptions()));

    namespace fs = std::filesystem;

    fs::path absoluteCachePath = cachePath;
    if (fs::path(cachePath).is_relative()) {
        absoluteCachePath = fs::current_path() / cachePath;
    }

    if (!fs::exists(absoluteCachePath.parent_path())) {
        logRuntimeError(stx::format(
            "Error initializing rocksDB cache: parent directory {} does not exist!",
            absoluteCachePath.parent_path().string()));
    }

    // Open the database.
    rocksdb::Status status = rocksdb::DB::
        Open(options_, absoluteCachePath, column_families, &column_family_handles_, &db_);

    if (!status.ok()) {
        logRuntimeError(stx::format(
            "Error opening database at {}: {}",
            absoluteCachePath.string(),
            status.ToString()));
    }

    for (rocksdb::ColumnFamilyHandle* handle : column_family_handles_) {
        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions(), handle));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            if (clearCache) {
                // Remove all existing data, as requested.
                status = db_->Delete(rocksdb::WriteOptions(), handle, it->key());
                if (!status.ok()) {
                    logRuntimeError(stx::format("Error clearing RocksDB cache!"));
                }
            }
            else if (handle->GetName() == ROCKSDB_NAMESPACE::kDefaultColumnFamilyName) {
                // Count existing tiles, since we're not clearing the cache.
                key_count_++;
            }
            else if (handle->GetName() == "FieldDicts") {
                // Update fieldCacheOffsets_ (superclass member)
                // for each node ID by triggering cache lookup.
                (*this)(it->key().ToString());
            }
        }
    }

    // TODO: delete a range if the cache is initialized with lower maxTiles
    // than is in the opened one...

    log().debug(stx::format(
        "Initialized RocksDB cache with {} existing tile entries.",
        key_count_));
}

RocksDBCache::~RocksDBCache()
{
    for (auto handle : column_family_handles_) {
        db_->DestroyColumnFamilyHandle(handle);
    }
    delete db_;
}

std::optional<std::string> RocksDBCache::getTileLayer(MapTileKey const& k)
{
    std::string read_value;
    auto status = db_->Get(read_options_, column_family_handles_[COL_TILES], k.toString(), &read_value);

    if (status.ok()) {
        log().trace(stx::format("Key: {} | Layer size: {}", k.toString(), read_value.size()));
        return read_value;
    }
    else if (status.IsNotFound()) {
        return {};
    }

    logRuntimeError(stx::format("Error reading from database: {}", status.ToString()));
    return {}; // For the sake of control flow checker.
}

void RocksDBCache::putTileLayer(MapTileKey const& k, std::string const& v)
{
    // Delete the oldest entry if we are exceeding the cache limit.
    // TODO improve performance by using RocksDB's DeleteRange function.
    if (max_key_count_ && key_count_ == max_key_count_) {
        rocksdb::WriteBatch batch;

        // Iterator of the timestamp column is in tile insertion order.
        std::unique_ptr<rocksdb::Iterator> it(
            db_->NewIterator(read_options_,
                             column_family_handles_[COL_TIMESTAMP]));
        batch.Delete(column_family_handles_[COL_TIMESTAMP], it->key());
        batch.Delete(column_family_handles_[COL_TILES], it->value());
        rocksdb::Status status = db_->Write(write_options_, &batch);

        if (!status.ok()) {
            logRuntimeError(stx::format("Could not delete oldest cache entry: {}", status.ToString()));
        }

        --key_count_;
    }

    auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();

    rocksdb::WriteBatch batch;
    batch.Put(column_family_handles_[COL_TIMESTAMP], std::to_string(timestamp), k.toString());
    batch.Put(column_family_handles_[COL_TILES], k.toString(), v);

    auto status = db_->Write(write_options_, &batch);

    if (!status.ok()) {
        logRuntimeError(stx::format("Error writing to database: {}", status.ToString()));
    }

    ++key_count_;
}

std::optional<std::string> RocksDBCache::getFields(std::string_view const& sourceNodeId)
{
    std::string read_value;
    auto status = db_->Get(read_options_, column_family_handles_[COL_FIELD_DICTS], sourceNodeId, &read_value);

    if (status.ok()) {
        log().trace(stx::format("Node: {} | Field dict size: {}", sourceNodeId, read_value.size()));
        return read_value;
    }

    logRuntimeError(stx::format("Error reading from database: {}", status.ToString()));
    return {};
}

void RocksDBCache::putFields(std::string_view const& sourceNodeId, std::string const& v)
{
    auto status = db_->Put(write_options_, column_family_handles_[COL_FIELD_DICTS], sourceNodeId, v);

    if (!status.ok()) {
        logRuntimeError(stx::format("Error writing to database: {}", status.ToString()));
    }
}

}  // namespace mapget