#include <sqlite3.h>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <chrono>
#include <mutex>

#include "mapget/log.h"
#include "sqlitecache.h"

namespace mapget
{

SQLiteCache::SQLiteCache(uint32_t cacheMaxTiles, std::string cachePath, bool clearCache)
    : maxTileCount_(cacheMaxTiles), dbPath_(cachePath), clearCache_(clearCache)
{
    namespace fs = std::filesystem;

    fs::path absoluteCachePath = cachePath;
    if (fs::path(cachePath).is_relative()) {
        absoluteCachePath = fs::current_path() / cachePath;
    }
    dbPath_ = absoluteCachePath.string();
    
    log().debug(fmt::format("Initializing SQLite cache at: {}", dbPath_));

    if (!fs::exists(absoluteCachePath.parent_path())) {
        raiseFmt("Error initializing SQLite cache: parent directory {} does not exist!",
            absoluteCachePath.parent_path().string());
    }

    if (clearCache && fs::exists(absoluteCachePath)) {
        fs::remove(absoluteCachePath);
    }

    // Open the database
    int rc = sqlite3_open(dbPath_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string error = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        raise(fmt::format("Error opening SQLite database at {}: {}",
            dbPath_, error));
    }

    // Enable WAL mode for better concurrency
    executeSQL("PRAGMA journal_mode=WAL");
    executeSQL("PRAGMA synchronous=NORMAL");

    initDatabase();
    prepareStatements();

    // Count existing tiles
    sqlite3_stmt* stmt;
    rc = sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM tiles", -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt, 0);
            log().debug(fmt::format("Initialized SQLite cache with {} existing tile entries.", count));
            
            // Handle special case: if the cache has more tiles than the limit
            if (!clearCache && maxTileCount_ > 0 && count > maxTileCount_) {
                int deleteCount = count - maxTileCount_;
                std::string sql = fmt::format(
                    "DELETE FROM tiles WHERE key IN (SELECT key FROM tiles ORDER BY timestamp ASC LIMIT {})",
                    deleteCount);
                executeSQL(sql);
            }
        }
        sqlite3_finalize(stmt);
    }

    // Update stringPoolOffsets_ for each existing string pool
    rc = sqlite3_prepare_v2(db_, "SELECT node_id FROM string_pools", -1, &stmt, nullptr);
    bool incompatibleCache = false;
    std::string incompatibleCacheReason;
    if (rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* stringPoolId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (stringPoolId) {
                try {
                    // Trigger cache lookup to update offsets.
                    Cache::getStringPool(stringPoolId);
                }
                catch (std::exception const& exception) {
                    incompatibleCache = true;
                    incompatibleCacheReason = exception.what();
                    break;
                }
            }
        }
        sqlite3_finalize(stmt);
    }
    if (incompatibleCache) {
        log().warn(
            "Clearing incompatible persistent cache '{}': {}",
            dbPath_,
            incompatibleCacheReason);
        executeSQL("DELETE FROM tiles");
        executeSQL("DELETE FROM string_pools");
        std::unique_lock stringPoolCacheLock(
            stringPoolCacheMutex_,
            std::defer_lock);
        std::unique_lock stringPoolOffsetLock(
            stringPoolOffsetMutex_,
            std::defer_lock);
        std::lock(
            stringPoolCacheLock,
            stringPoolOffsetLock);
        stringPoolPerStringPoolId_.clear();
        stringPoolOffsets_.clear();
    }
}

SQLiteCache::~SQLiteCache()
{
    // Clean up prepared statements
    if (stmts_.getTile) sqlite3_finalize(stmts_.getTile);
    if (stmts_.putTile) sqlite3_finalize(stmts_.putTile);
    if (stmts_.updateTileTimestamp) sqlite3_finalize(stmts_.updateTileTimestamp);
    if (stmts_.deleteTile) sqlite3_finalize(stmts_.deleteTile);
    if (stmts_.getStringPool) sqlite3_finalize(stmts_.getStringPool);
    if (stmts_.putStringPool) sqlite3_finalize(stmts_.putStringPool);
    if (stmts_.getOldestTile) sqlite3_finalize(stmts_.getOldestTile);
    if (stmts_.getTileCount) sqlite3_finalize(stmts_.getTileCount);

    if (db_) {
        sqlite3_close(db_);
    }
}

void SQLiteCache::initDatabase()
{
    // Create tiles table with timestamp for FIFO eviction
    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS tiles (
            key TEXT PRIMARY KEY,
            data BLOB NOT NULL,
            timestamp INTEGER NOT NULL
        )
    )");

    // Create index on timestamp for efficient FIFO eviction
    executeSQL("CREATE INDEX IF NOT EXISTS idx_tiles_timestamp ON tiles(timestamp ASC)");

    // Create string pools table
    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS string_pools (
            node_id TEXT PRIMARY KEY,
            data BLOB NOT NULL
        )
    )");
}

void SQLiteCache::executeSQL(const std::string& sql)
{
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string error = errMsg ? errMsg : "Unknown error";
        sqlite3_free(errMsg);
        raise(fmt::format("SQLite error executing '{}': {}", sql, error));
    }
}

void SQLiteCache::prepareStatements()
{
    int rc;

    // Prepare statement for getting tiles
    rc = sqlite3_prepare_v2(db_,
        "SELECT data FROM tiles WHERE key = ?",
        -1, &stmts_.getTile, nullptr);
    if (rc != SQLITE_OK) {
        raise(fmt::format("Failed to prepare getTile statement: {}", sqlite3_errmsg(db_)));
    }

    // Prepare statement for inserting/updating tiles
    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO tiles (key, data, timestamp) VALUES (?, ?, ?)",
        -1, &stmts_.putTile, nullptr);
    if (rc != SQLITE_OK) {
        raise(fmt::format("Failed to prepare putTile statement: {}", sqlite3_errmsg(db_)));
    }

    // Prepare statement for updating tile timestamp
    rc = sqlite3_prepare_v2(db_,
        "UPDATE tiles SET timestamp = ? WHERE key = ?",
        -1, &stmts_.updateTileTimestamp, nullptr);
    if (rc != SQLITE_OK) {
        raise(fmt::format("Failed to prepare updateTileTimestamp statement: {}", sqlite3_errmsg(db_)));
    }

    // Prepare statement for deleting tiles
    rc = sqlite3_prepare_v2(db_,
        "DELETE FROM tiles WHERE key = ?",
        -1, &stmts_.deleteTile, nullptr);
    if (rc != SQLITE_OK) {
        raise(fmt::format("Failed to prepare deleteTile statement: {}", sqlite3_errmsg(db_)));
    }

    // Prepare statement for getting string pools
    rc = sqlite3_prepare_v2(db_,
        "SELECT data FROM string_pools WHERE node_id = ?",
        -1, &stmts_.getStringPool, nullptr);
    if (rc != SQLITE_OK) {
        raise(fmt::format("Failed to prepare getStringPool statement: {}", sqlite3_errmsg(db_)));
    }

    // Prepare statement for inserting/updating string pools
    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO string_pools (node_id, data) VALUES (?, ?)",
        -1, &stmts_.putStringPool, nullptr);
    if (rc != SQLITE_OK) {
        raise(fmt::format("Failed to prepare putStringPool statement: {}", sqlite3_errmsg(db_)));
    }

    // Prepare statement for getting oldest tile
    rc = sqlite3_prepare_v2(db_,
        "SELECT key FROM tiles ORDER BY timestamp ASC LIMIT 1",
        -1, &stmts_.getOldestTile, nullptr);
    if (rc != SQLITE_OK) {
        raise(fmt::format("Failed to prepare getOldestTile statement: {}", sqlite3_errmsg(db_)));
    }

    // Prepare statement for counting tiles
    rc = sqlite3_prepare_v2(db_,
        "SELECT COUNT(*) FROM tiles",
        -1, &stmts_.getTileCount, nullptr);
    if (rc != SQLITE_OK) {
        raise(fmt::format("Failed to prepare getTileCount statement: {}", sqlite3_errmsg(db_)));
    }
}

std::optional<std::string> SQLiteCache::getTileLayerBlob(MapTileKey const& k)
{
    std::lock_guard<std::mutex> lock(dbMutex_);
    
    sqlite3_reset(stmts_.getTile);
    sqlite3_bind_text(stmts_.getTile, 1, k.toString().c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmts_.getTile);
    if (rc == SQLITE_ROW) {
        const void* data = sqlite3_column_blob(stmts_.getTile, 0);
        int size = sqlite3_column_bytes(stmts_.getTile, 0);
        std::string result(static_cast<const char*>(data), size);
        
        log().trace(fmt::format("Key: {} | Layer size: {}", k.toString(), size));
        log().debug("Cache hits: {}, cache misses: {}", cacheHits_.load(), cacheMisses_.load());
        return result;
    }
    else if (rc == SQLITE_DONE) {
        log().debug("Cache hits: {}, cache misses: {}", cacheHits_.load(), cacheMisses_.load());
        return {};
    }
    else {
        raise(fmt::format("Error reading from database: {}", sqlite3_errmsg(db_)));
    }
}

void SQLiteCache::putTileLayerBlob(MapTileKey const& k, std::string const& v)
{
    std::lock_guard<std::mutex> lock(dbMutex_);
    
    auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();

    sqlite3_reset(stmts_.putTile);
    sqlite3_bind_text(stmts_.putTile, 1, k.toString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmts_.putTile, 2, v.data(), v.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmts_.putTile, 3, timestamp);

    int rc = sqlite3_step(stmts_.putTile);
    if (rc != SQLITE_DONE) {
        raise(fmt::format("Error writing to database: {}", sqlite3_errmsg(db_)));
    }

    log().debug("Cache hits: {}, cache misses: {}", cacheHits_.load(), cacheMisses_.load());

    // Check if we need to evict old tiles
    if (maxTileCount_ > 0) {
        sqlite3_reset(stmts_.getTileCount);
        if (sqlite3_step(stmts_.getTileCount) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmts_.getTileCount, 0);
            if (count > maxTileCount_) {
                cleanupOldestTiles();
            }
        }
    }
}

void SQLiteCache::eraseTileLayerBlob(MapTileKey const& k)
{
    std::lock_guard<std::mutex> lock(dbMutex_);
    sqlite3_reset(stmts_.deleteTile);
    auto const key = k.toString();
    sqlite3_bind_text(
        stmts_.deleteTile,
        1,
        key.c_str(),
        -1,
        SQLITE_TRANSIENT);
    int const rc = sqlite3_step(stmts_.deleteTile);
    if (rc != SQLITE_DONE) {
        raise(fmt::format(
            "Could not delete cache entry '{}': {}",
            key,
            sqlite3_errmsg(db_)));
    }
}

void SQLiteCache::forEachTileLayerBlob(const TileBlobVisitor& cb) const
{
    std::lock_guard<std::mutex> lock(dbMutex_);

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, "SELECT key, data FROM tiles", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        raise(fmt::format("Failed to prepare tile iteration statement: {}", sqlite3_errmsg(db_)));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const void* data = sqlite3_column_blob(stmt, 1);
        int size = sqlite3_column_bytes(stmt, 1);
        if (key && data && size >= 0) {
            cb(MapTileKey(key), std::string(static_cast<const char*>(data), size));
        }
    }
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        raise(fmt::format("Error iterating cached tiles: {}", sqlite3_errmsg(db_)));
    }

    sqlite3_finalize(stmt);
}

void SQLiteCache::cleanupOldestTiles()
{
    // Delete the oldest tile
    sqlite3_reset(stmts_.getOldestTile);
    if (sqlite3_step(stmts_.getOldestTile) == SQLITE_ROW) {
        const char* oldestKey = reinterpret_cast<const char*>(sqlite3_column_text(stmts_.getOldestTile, 0));
        if (oldestKey) {
            sqlite3_reset(stmts_.deleteTile);
            sqlite3_bind_text(stmts_.deleteTile, 1, oldestKey, -1, SQLITE_TRANSIENT);
            
            int rc = sqlite3_step(stmts_.deleteTile);
            if (rc != SQLITE_DONE) {
                raise(fmt::format("Could not delete oldest cache entry: {}", sqlite3_errmsg(db_)));
            }
        }
    }
}

std::optional<std::string> SQLiteCache::getStringPoolBlob(std::string_view const& sourceStringPoolId)
{
    std::lock_guard<std::mutex> lock(dbMutex_);
    
    sqlite3_reset(stmts_.getStringPool);
    sqlite3_bind_text(stmts_.getStringPool, 1, sourceStringPoolId.data(), sourceStringPoolId.size(), SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmts_.getStringPool);
    if (rc == SQLITE_ROW) {
        const void* data = sqlite3_column_blob(stmts_.getStringPool, 0);
        int size = sqlite3_column_bytes(stmts_.getStringPool, 0);
        std::string result(static_cast<const char*>(data), size);
        
        log().trace(fmt::format("Node: {} | String pool size: {}", sourceStringPoolId, size));
        return result;
    }
    else if (rc == SQLITE_DONE) {
        return {};
    }
    else {
        raise(fmt::format("Error reading from database: {}", sqlite3_errmsg(db_)));
    }
}

void SQLiteCache::putStringPoolBlob(std::string_view const& sourceStringPoolId, std::string const& v)
{
    std::lock_guard<std::mutex> lock(dbMutex_);
    
    sqlite3_reset(stmts_.putStringPool);
    sqlite3_bind_text(stmts_.putStringPool, 1, sourceStringPoolId.data(), sourceStringPoolId.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmts_.putStringPool, 2, v.data(), v.size(), SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmts_.putStringPool);
    if (rc != SQLITE_DONE) {
        raise(fmt::format("Error writing to database: {}", sqlite3_errmsg(db_)));
    }
}

nlohmann::json SQLiteCache::getStatistics() const
{
    auto result = Cache::getStatistics();
    std::lock_guard lock(dbMutex_);
    auto readByteStatus = [this](int operation) {
        int current = 0;
        int peak = 0;
        if (sqlite3_db_status(db_, operation, &current, &peak, 0) != SQLITE_OK) {
            return nlohmann::json::object();
        }
        return nlohmann::json{
            {"current-bytes", current},
            {"peak-bytes", peak},
        };
    };
    auto const pageCache = readByteStatus(SQLITE_DBSTATUS_CACHE_USED);
    auto const schema = readByteStatus(SQLITE_DBSTATUS_SCHEMA_USED);
    auto const statements = readByteStatus(SQLITE_DBSTATUS_STMT_USED);
    int lookasideCurrent = 0;
    int lookasidePeak = 0;
    sqlite3_db_status(
        db_,
        SQLITE_DBSTATUS_LOOKASIDE_USED,
        &lookasideCurrent,
        &lookasidePeak,
        0);

    result["sqlite"] = {
        {"page-cache", pageCache},
        {"schema", schema},
        {"prepared-statements", statements},
        {"lookaside", {
            {"current-slots", lookasideCurrent},
            {"peak-slots", lookasidePeak},
        }},
        {"database-path", dbPath_},
    };
    auto const sqliteBytes =
        pageCache.value("current-bytes", uint64_t{0}) +
        schema.value("current-bytes", uint64_t{0}) +
        statements.value("current-bytes", uint64_t{0});
    result["memory"]["sqlite-owned-bytes"] = sqliteBytes;
    result["memory"]["sqlite-measurement"] = "sqlite-db-status";
    return result;
}

}  // namespace mapget
