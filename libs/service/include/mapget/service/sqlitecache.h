#pragma once

#include "cache.h"

#ifdef MAPGET_WITH_SQLITE
#include <sqlite3.h>
#include <memory>
#include <string>
#endif

namespace mapget
{

#ifdef MAPGET_WITH_SQLITE

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

#else

/**
 * Stub implementation when SQLite is disabled at compile time.
 * Throws an exception when instantiated.
 */
class SQLiteCache : public Cache
{
public:
    explicit SQLiteCache(
        uint32_t cacheMaxTiles = 1024,
        std::string cachePath = "mapget-cache.db",
        bool clearCache = false)
    {
        throw std::runtime_error("SQLite support was disabled at compile time. Use -DMAPGET_WITH_SQLITE=ON to enable it.");
    }

    std::optional<std::string> getTileLayerBlob(MapTileKey const& k) override { return {}; }
    void putTileLayerBlob(MapTileKey const& k, std::string const& v) override {}
    std::optional<std::string> getStringPoolBlob(std::string_view const& sourceNodeId) override { return {}; }
    void putStringPoolBlob(std::string_view const& sourceNodeId, std::string const& v) override {}
};

#endif

}  // namespace mapget