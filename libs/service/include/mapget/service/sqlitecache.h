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
 * in SQLite. Oldest tiles are removed automatically in FIFO order when cacheMaxTiles is
 * reached.
 */
class SQLiteCache : public Cache
{
public:
    explicit SQLiteCache(
        uint32_t cacheMaxTiles = 1024,
        std::string cachePath = "mapget-cache.db",
        bool clearCache = false);
    ~SQLiteCache() override;

    std::optional<std::string> getTileLayerBlob(MapTileKey const& k) override;
    void putTileLayerBlob(MapTileKey const& k, std::string const& v) override;
    void forEachTileLayerBlob(const TileBlobVisitor& cb) const override;
    std::optional<std::string> getStringPoolBlob(std::string_view const& sourceNodeId) override;
    void putStringPoolBlob(std::string_view const& sourceNodeId, std::string const& v) override;

private:
    void initDatabase();
    void executeSQL(const std::string& sql);
    void prepareStatements();
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
