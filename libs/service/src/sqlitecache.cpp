#ifdef MAPGET_WITH_SQLITE
#include <sqlite3.h>
#endif
#include <filesystem>
#include <iostream>
#include <chrono>

#include "mapget/log.h"
#include "sqlitecache.h"

namespace mapget
{

#ifdef MAPGET_WITH_SQLITE

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
        raise(fmt::format("Error opening SQLite database at {}: {}",
            dbPath_, sqlite3_errmsg(db_)));
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
    if (rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* nodeId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (nodeId) {
                // Trigger cache lookup to update offsets
                Cache::getStringPool(nodeId);
            }
        }
        sqlite3_finalize(stmt);
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
    sqlite3_reset(stmts_.getTile);
    sqlite3_bind_text(stmts_.getTile, 1, k.toString().c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmts_.getTile);
    if (rc == SQLITE_ROW) {
        const void* data = sqlite3_column_blob(stmts_.getTile, 0);
        int size = sqlite3_column_bytes(stmts_.getTile, 0);
        std::string result(static_cast<const char*>(data), size);
        
        log().trace(fmt::format("Key: {} | Layer size: {}", k.toString(), size));
        log().debug("Cache hits: {}, cache misses: {}", cacheHits_, cacheMisses_);
        return result;
    }
    else if (rc == SQLITE_DONE) {
        log().debug("Cache hits: {}, cache misses: {}", cacheHits_, cacheMisses_);
        return {};
    }
    else {
        raise(fmt::format("Error reading from database: {}", sqlite3_errmsg(db_)));
    }
}

void SQLiteCache::putTileLayerBlob(MapTileKey const& k, std::string const& v)
{
    auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();

    sqlite3_reset(stmts_.putTile);
    sqlite3_bind_text(stmts_.putTile, 1, k.toString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmts_.putTile, 2, v.data(), v.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmts_.putTile, 3, timestamp);

    int rc = sqlite3_step(stmts_.putTile);
    if (rc != SQLITE_DONE) {
        raise(fmt::format("Error writing to database: {}", sqlite3_errmsg(db_)));
    }

    log().debug("Cache hits: {}, cache misses: {}", cacheHits_, cacheMisses_);

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

std::optional<std::string> SQLiteCache::getStringPoolBlob(std::string_view const& sourceNodeId)
{
    sqlite3_reset(stmts_.getStringPool);
    sqlite3_bind_text(stmts_.getStringPool, 1, sourceNodeId.data(), sourceNodeId.size(), SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmts_.getStringPool);
    if (rc == SQLITE_ROW) {
        const void* data = sqlite3_column_blob(stmts_.getStringPool, 0);
        int size = sqlite3_column_bytes(stmts_.getStringPool, 0);
        std::string result(static_cast<const char*>(data), size);
        
        log().trace(fmt::format("Node: {} | String pool size: {}", sourceNodeId, size));
        return result;
    }
    else if (rc == SQLITE_DONE) {
        return {};
    }
    else {
        raise(fmt::format("Error reading from database: {}", sqlite3_errmsg(db_)));
    }
}

void SQLiteCache::putStringPoolBlob(std::string_view const& sourceNodeId, std::string const& v)
{
    sqlite3_reset(stmts_.putStringPool);
    sqlite3_bind_text(stmts_.putStringPool, 1, sourceNodeId.data(), sourceNodeId.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmts_.putStringPool, 2, v.data(), v.size(), SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmts_.putStringPool);
    if (rc != SQLITE_DONE) {
        raise(fmt::format("Error writing to database: {}", sqlite3_errmsg(db_)));
    }
}

#endif  // MAPGET_WITH_SQLITE

}  // namespace mapget