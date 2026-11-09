#pragma once

#include "cache.h"
#include <sqlite3.h>
#include <memory>
#include <mutex>
#include <string>

namespace mapget
{

/**
 * A persistent cache implementation that stores layers and string pools
 * in SQLite. Writes refresh a tile's insertion timestamp; reads do not. The
 * oldest timestamp is evicted when a positive tile-count limit is exceeded.
 */
class SQLiteCache : public Cache
{
public:
    /**
     * Open/create a database relative to the current directory. Zero tiles
     * means unbounded storage; clearCache removes an existing file first.
     * Incompatible persisted dictionaries cause both tables to be cleared.
     */
    explicit SQLiteCache(
        uint32_t cacheMaxTiles = 1024,
        std::string cachePath = "mapget-cache.db",
        bool clearCache = false);

    /** Release prepared statements and close the database after callers have stopped. */
    ~SQLiteCache() override;

    std::optional<std::string> getTileLayerBlob(MapPartitionKey const& k) override;
    void putTileLayerBlob(MapPartitionKey const& k, std::string const& v) override;
    void eraseTileLayerBlob(MapPartitionKey const& k) override;
    void forEachTileLayerBlob(const TileBlobVisitor& cb) const override;
    std::optional<std::string> getStringPoolBlob(std::string_view const& sourceStringPoolId) override;
    void putStringPoolBlob(std::string_view const& sourceStringPoolId, std::string const& v) override;

    /** Add SQLite page-cache/schema/statement memory counters to generic cache statistics. */
    [[nodiscard]] nlohmann::json getStatistics() const override;

private:
    /** Create the persistent tile/dictionary tables and the FIFO timestamp index. */
    void initDatabase();

    /** Execute constructor-time SQL, throwing a mapget error on SQLite failure. */
    void executeSQL(const std::string& sql);

    /** Allocate reusable statements before this cache is exposed to worker threads. */
    void prepareStatements();

    /** Evict one oldest tile; the caller must hold dbMutex_. */
    void cleanupOldestTiles();

    sqlite3* db_{nullptr};
    std::string dbPath_;
    uint32_t maxTileCount_;
    bool clearCache_;
    mutable std::mutex dbMutex_;

    // Prepared statements for performance
    struct Statements {
        sqlite3_stmt* getTile{nullptr};
        sqlite3_stmt* putTile{nullptr};
        sqlite3_stmt* updateTileTimestamp{nullptr};
        sqlite3_stmt* deleteTile{nullptr};
        sqlite3_stmt* getStringPool{nullptr};
        sqlite3_stmt* putStringPool{nullptr};
        sqlite3_stmt* getOldestTile{nullptr};
        sqlite3_stmt* getTileCount{nullptr};
    } stmts_;
};

}  // namespace mapget
