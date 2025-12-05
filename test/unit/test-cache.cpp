#include <cstdint>
#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>

#include "mapget/http-service/cli.h"
#include "mapget/log.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/info.h"
#include "mapget/service/sqlitecache.h"
#include "mapget/service/nullcache.h"

using namespace mapget;

// Cache traits to handle differences between cache implementations
template<typename CacheType>
struct CacheTraits;


template<>
struct CacheTraits<SQLiteCache> {
    static constexpr const char* nodeIdPrefix = "SQLiteCacheTestingNode";
    static constexpr const char* otherNodeIdPrefix = "OtherSQLiteCacheTestingNode";
    static constexpr const char* stringPoolNodeIdPrefix = "SQLiteStringPoolTestingNode";
    static constexpr const char* defaultCacheName = "mapget-cache.db";
    static constexpr const char* testDirPrefix = "sqlite-unit-test-";
    static constexpr bool needsDbExtension = true;
};

template<>
struct CacheTraits<NullCache> {
    static constexpr const char* nodeIdPrefix = "NullCacheTestingNode";
    static constexpr const char* otherNodeIdPrefix = "OtherNullCacheTestingNode";
    static constexpr const char* stringPoolNodeIdPrefix = "NullStringPoolTestingNode";
    static constexpr const char* defaultCacheName = "";
    static constexpr const char* testDirPrefix = "null-unit-test-";
    static constexpr bool needsDbExtension = false;
};

// Common test data setup functions
namespace {
    std::shared_ptr<LayerInfo> createTestLayerInfo() {
        auto layerInfo = LayerInfo::fromJson(R"({
            "layerId": "WayLayer",
            "type": "Features",
            "featureTypes": [
                {
                    "name": "Way",
                    "uniqueIdCompositions": [
                        [
                            {
                                "partId": "areaId",
                                "description": "String identifying the map area.",
                                "datatype": "STR"
                            },
                            {
                                "partId": "wayId",
                                "description": "Globally Unique 32b integer.",
                                "datatype": "U32"
                            }
                        ]
                    ]
                }
            ]
        })"_json);

        return std::make_shared<LayerInfo>(LayerInfo{
            layerInfo->layerId_,
            layerInfo->type_,
            layerInfo->featureTypes_,
            std::vector<int>{0, 1, 2},
            std::vector<Coverage>{{1, 2, {}}, {3, 3, {}}},
            true,
            false,
            Version{0, 0, 0}});
    }

    DataSourceInfo createTestDataSourceInfo(const std::string& nodeId, const std::string& mapId, std::shared_ptr<LayerInfo> layerInfo) {
        std::unordered_map<std::string, std::shared_ptr<LayerInfo>> layers;
        layers[layerInfo->layerId_] = layerInfo;
        
        return DataSourceInfo{
            nodeId,
            mapId,
            layers,
            5,
            false,
            nlohmann::json::object(),
            TileLayerStream::CurrentProtocolVersion};
    }

    std::string createSerializedStringPoolMessage(const std::string& testStringPoolNodeId) {
        auto testStringPool = StringPool(testStringPoolNodeId);
        
        std::stringstream serializedStrings;
        testStringPool.write(serializedStrings, 0);

        std::stringstream serializedMessage;
        bitsery::Serializer<bitsery::OutputStreamAdapter> s(serializedMessage);
        s.object(TileLayerStream::CurrentProtocolVersion);
        s.value1b(TileLayerStream::MessageType::StringPool);
        s.value4b((uint32_t)serializedStrings.str().size());
        serializedMessage << serializedStrings.str();
        
        return serializedMessage.str();
    }

    // Helper for creating temporary cache paths
    std::filesystem::path createTempCachePath(const std::string& prefix, bool needsDbExtension = true) {
        auto now = std::chrono::system_clock::now();
        auto epoch_time = std::chrono::system_clock::to_time_t(now);
        
        std::filesystem::path test_cache = std::filesystem::temp_directory_path() /
            (prefix + std::to_string(epoch_time) + (needsDbExtension ? ".db" : ""));
        
        // Delete cache if it already exists
        if (std::filesystem::exists(test_cache)) {
            std::filesystem::remove_all(test_cache);
        }
        
        return test_cache;
    }

    // Helper struct for concurrent test error tracking
    struct ConcurrentTestMetrics {
        std::atomic<int> readErrors{0};
        std::atomic<int> writeErrors{0};
        std::atomic<int> successfulReads{0};
        std::atomic<int> successfulWrites{0};
    };

    // Helper for joining threads
    void joinThreads(std::vector<std::thread>& threads) {
        for (auto& t : threads) {
            t.join();
        }
    }

    // Helper for creating test tiles with features
    std::shared_ptr<TileFeatureLayer> createTestTile(
        const TileId& tileId,
        const std::string& nodeId,
        const std::string& mapId,
        std::shared_ptr<LayerInfo> layerInfo,
        std::shared_ptr<StringPool> strings,
        int featureCount = 0) {
        
        auto tile = std::make_shared<TileFeatureLayer>(
            tileId, nodeId, mapId, layerInfo, strings);
        
        for (int i = 0; i < featureCount; ++i) {
            tile->newFeature("Way", {
                {"areaId", "Area" + std::to_string(i)},
                {"wayId", i}
            });
        }
        
        return tile;
    }
}

// Common test function for both cache implementations
template<typename CacheType>
void testCacheImplementation() {
    using Traits = CacheTraits<CacheType>;
    
    mapget::setLogLevel("trace", log());

    // Use common test data setup
    auto layerInfo = createTestLayerInfo();

    // Create a basic TileFeatureLayer.
    auto tileId = TileId::fromWgs84(42., 11., 13);
    auto nodeId = Traits::nodeIdPrefix;
    auto mapId = "CacheMe";
    // Create empty shared autofilled string dictionary.
    auto strings = std::make_shared<StringPool>(nodeId);
    auto tile = std::make_shared<TileFeatureLayer>(
        tileId,
        nodeId,
        mapId,
        layerInfo,
        strings);
    // Create a DataSourceInfo object.
    DataSourceInfo info = createTestDataSourceInfo(nodeId, mapId, layerInfo);

    // Create another basic TileFeatureLayer for a different node.
    auto otherTileId = TileId::fromWgs84(42., 12., 13);
    auto otherNodeId = Traits::otherNodeIdPrefix;
    auto otherMapId = "CacheMeToo";
    // Create empty shared autofilled string-pool.
    auto otherStringPool = std::make_shared<StringPool>(otherNodeId);
    auto otherTile = std::make_shared<TileFeatureLayer>(
        otherTileId,
        otherNodeId,
        otherMapId,
        layerInfo,
        otherStringPool);
    // Create another DataSourceInfo object, but reuse the layer info.
    DataSourceInfo otherInfo = createTestDataSourceInfo(otherNodeId, otherMapId, layerInfo);


    auto testStringPoolNodeId = Traits::stringPoolNodeIdPrefix;
    auto serializedMessage = createSerializedStringPoolMessage(testStringPoolNodeId);

    auto getFeatureLayer = [](auto& cache, auto tileId, auto info) {
        auto layer = cache->getTileLayer(tileId, info);
        REQUIRE(!!layer.tile);

        auto layerInfo = layer.tile->layerInfo();
        REQUIRE(!!layerInfo);
        REQUIRE(layerInfo->type_ == mapget::LayerType::Features);
        return std::static_pointer_cast<TileFeatureLayer>(layer.tile);
    };

    SECTION("Insert, retrieve, and update feature layer") {
        // Open or create cache, clear any existing data.
        auto cache = std::make_shared<CacheType>(
            1024, Traits::defaultCacheName, true);
        auto stringPoolCount = cache->getStatistics()["loaded-string-pools"].template get<int>();
        REQUIRE(stringPoolCount == 0);

        // putTileLayer triggers both putTileLayerBlob and putFieldsBlob.
        cache->putTileLayer(tile);
        auto returnedTile = getFeatureLayer(cache, tile->id(), info);
        stringPoolCount = cache->getStatistics()["loaded-string-pools"].template get<int>();
        REQUIRE(stringPoolCount == 1);

        // Update a tile, check that cache returns the updated version.
        REQUIRE(returnedTile->size() == 0);
        auto feature = tile->newFeature("Way", {{"areaId", "MediocreArea"}, {"wayId", 24}});
        cache->putTileLayer(tile);
        auto updatedTile = getFeatureLayer(cache, tile->id(), info);
        REQUIRE(updatedTile->size() == 1);

        // Check that cache hits and misses are properly recorded.
        auto missingTile = cache->getTileLayer(otherTile->id(), otherInfo);
        REQUIRE(cache->getStatistics()["cache-hits"] == 2);
        REQUIRE(cache->getStatistics()["cache-misses"] == 1);
    }

    SECTION("Reopen cache with maxTileCount=1, insert another layer") {
        // Open existing cache.
        auto cache = std::make_shared<CacheType>(
            1, Traits::defaultCacheName, false);
        auto stringPoolCount = cache->getStatistics()["loaded-string-pools"].template get<int>();
        REQUIRE(stringPoolCount == 1);

        // Add a tile to trigger cache cleaning.
        cache->putTileLayer(otherTile);

        auto returnedTile = getFeatureLayer(cache, otherTile->id(), otherInfo);
        REQUIRE(returnedTile->nodeId() == otherTile->nodeId());

        // String pools are updated with getTileLayer.
        stringPoolCount = cache->getStatistics()["loaded-string-pools"].template get<int>();
        REQUIRE(stringPoolCount == 2);

        // Query the first inserted layer - it should not be retrievable.
        auto missingTile = cache->getTileLayer(tile->id(), info);
        REQUIRE(cache->getStatistics()["cache-misses"] == 1);
        REQUIRE(!missingTile.tile);
    }

    SECTION("Store another tile at unlimited cache size") {
        auto cache = std::make_shared<CacheType>(
            0, Traits::defaultCacheName, false);

        // Insert another tile for the next test.
        cache->putTileLayer(tile);

        // Make sure the previous tile is still there, since cache is unlimited.
        auto olderTile = cache->getTileLayer(otherTile->id(), otherInfo);
        REQUIRE(cache->getStatistics()["cache-misses"] == 0);
        REQUIRE(cache->getStatistics()["cache-hits"] == 1);
    }

    SECTION("Reopen cache with maxTileCount=1, check older tile was deleted") {
        auto cache = std::make_shared<CacheType>(
            1, Traits::defaultCacheName, false);
        REQUIRE(cache->getStatistics()["cache-misses"] == 0);

        // Query the first inserted layer - it should not be retrievable.
        auto missingTile = cache->getTileLayer(otherTile->id(), otherInfo);
        REQUIRE(cache->getStatistics()["cache-misses"] == 1);
    }

    SECTION("Reopen cache, check loading of string pools") {
        // Open existing cache.
        auto cache = std::make_shared<CacheType>();
        REQUIRE(cache->getStatistics()["loaded-string-pools"] == 2);

        cache->putStringPoolBlob(testStringPoolNodeId, serializedMessage);
        auto returnedEntry = cache->getStringPoolBlob(testStringPoolNodeId);

        // Make sure the string pool was properly stored.
        REQUIRE(returnedEntry.value() == serializedMessage);

        // TODO this kind of access bypasses the creation of a stringPoolOffsets_
        //  entry in the cache -> decouple the cache storage logic clearly from
        //  tile layer writing logic.
        REQUIRE(cache->getStatistics()["loaded-string-pools"] == 2);
    }

    SECTION("Reopen cache again, check loading of string pools again") {
        // Open existing cache.
        auto cache = std::make_shared<CacheType>();
        REQUIRE(cache->getStatistics()["loaded-string-pools"] == 3);

        // Check that the same value can still be retrieved from string pooln.
        auto returnedEntry = cache->getStringPoolBlob(testStringPoolNodeId);
        REQUIRE(returnedEntry.value() == serializedMessage);
    }

    SECTION("Create cache under a custom path") {
        auto test_cache = createTempCachePath(Traits::testDirPrefix, Traits::needsDbExtension);
        log().info(fmt::format("Test creating cache: {}", test_cache.string()));
        
        REQUIRE(!std::filesystem::exists(test_cache));

        // Create cache and make sure it worked.
        auto cache = std::make_shared<CacheType>(
            1024, test_cache.string(), true);
        REQUIRE(std::filesystem::exists(test_cache));
    }
}


// Specialized test function for NullCache
void testNullCacheImplementation() {
    using Traits = CacheTraits<NullCache>;
    
    mapget::setLogLevel("trace", log());

    // Use common test data setup
    auto layerInfo = createTestLayerInfo();

    // Create a basic TileFeatureLayer.
    auto tileId = TileId::fromWgs84(42., 11., 13);
    auto nodeId = Traits::nodeIdPrefix;
    auto mapId = "CacheMe";
    // Create empty shared autofilled string dictionary.
    auto strings = std::make_shared<StringPool>(nodeId);
    auto tile = std::make_shared<TileFeatureLayer>(
        tileId,
        nodeId,
        mapId,
        layerInfo,
        strings);
    // Create a DataSourceInfo object.
    DataSourceInfo info = createTestDataSourceInfo(nodeId, mapId, layerInfo);

    auto testStringPoolNodeId = Traits::stringPoolNodeIdPrefix;
    auto serializedMessage = createSerializedStringPoolMessage(testStringPoolNodeId);

    SECTION("NullCache always returns empty/cache misses") {
        // Create NullCache instance
        auto cache = std::make_shared<NullCache>();
        
        // Verify initial statistics
        auto stringPoolCount = cache->getStatistics()["loaded-string-pools"].get<int>();
        REQUIRE(stringPoolCount == 0);

        // Try to store a tile - should succeed but not actually store anything
        cache->putTileLayer(tile);
        
        // Try to retrieve the tile - should always return nullptr
        auto returnedTile = cache->getTileLayer(tile->id(), info);
        REQUIRE(!returnedTile.tile);
        
        // Check cache statistics - should show cache miss
        REQUIRE(cache->getStatistics()["cache-hits"] == 0);
        REQUIRE(cache->getStatistics()["cache-misses"] == 1);
        
        // Try string pool operations
        cache->putStringPoolBlob(testStringPoolNodeId, serializedMessage);
        auto returnedEntry = cache->getStringPoolBlob(testStringPoolNodeId);
        
        // String pool retrieval should also return empty
        REQUIRE(!returnedEntry.has_value());
        
        // Multiple retrieval attempts should all result in cache misses
        for (int i = 0; i < 5; ++i) {
            auto missingTile = cache->getTileLayer(tile->id(), info);
            REQUIRE(!missingTile.tile);
        }
        
        // Verify all attempts resulted in cache misses
        REQUIRE(cache->getStatistics()["cache-hits"] == 0);
        REQUIRE(cache->getStatistics()["cache-misses"] == 6); // 1 initial + 5 in loop
    }
}

TEST_CASE("SQLiteCache", "[Cache]")
{
    testCacheImplementation<SQLiteCache>();
}

TEST_CASE("SQLiteCache Concurrent Access", "[Cache][Concurrent]")
{
    mapget::setLogLevel("trace", log());
    
    // Use common test data setup
    auto layerInfo = createTestLayerInfo();
    
    SECTION("Concurrent read-write access") {
        auto test_cache = createTempCachePath("sqlite-concurrent-rw-test-");
        auto cache = std::make_shared<SQLiteCache>(0, test_cache.string(), true);
        
        // Create test data
        auto nodeId = "ConcurrentTestNode";
        auto mapId = "ConcurrentMap";
        auto strings = std::make_shared<StringPool>(nodeId);
        DataSourceInfo info = createTestDataSourceInfo(nodeId, mapId, layerInfo);
        
        // Create multiple tiles for testing
        std::vector<std::shared_ptr<TileFeatureLayer>> tiles;
        for (int i = 0; i < 10; ++i) {
            auto tileId = TileId::fromWgs84(42.0 + i * 0.1, 11.0, 13);
            tiles.push_back(createTestTile(tileId, nodeId, mapId, layerInfo, strings, 1));
        }
        
        // Write initial tiles
        for (const auto& tile : tiles) {
            cache->putTileLayer(tile);
        }
        
        ConcurrentTestMetrics metrics;
        std::atomic<int> updatedFeaturesFound{0};
        
        // Track the maximum feature count seen for each tile
        std::vector<std::atomic<size_t>> maxFeatureCounts(tiles.size());
        for (size_t i = 0; i < tiles.size(); ++i) {
            maxFeatureCounts[i] = 1; // Initial count
        }
        
        // Start multiple reader threads
        std::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i) {
            readers.emplace_back([&cache, &tiles, &info, &metrics, 
                                  &updatedFeaturesFound, &maxFeatureCounts]() {
                for (int j = 0; j < 100; ++j) {
                    try {
                        // Read random tiles
                        int tileIndex = j % tiles.size();
                        auto tile = cache->getTileLayer(tiles[tileIndex]->id(), info);
                        if (tile.tile) {
                            metrics.successfulReads++;
                            auto featureLayer = std::static_pointer_cast<TileFeatureLayer>(tile.tile);
                            
                            // Verify the tile data is correct
                            REQUIRE(featureLayer->size() > 0);
                            
                            // Track if we see increasing feature counts
                            size_t currentCount = featureLayer->size();
                            size_t previousMax = maxFeatureCounts[tileIndex].load();
                            while (currentCount > previousMax && 
                                   !maxFeatureCounts[tileIndex].compare_exchange_weak(previousMax, currentCount)) {
                                // Update previousMax and retry
                            }
                            
                            // Check if we see features added by writers (wayId >= 1000)
                            // For simplicity, just count features beyond the initial one per tile
                            // Since writers add features with wayId >= 1000, any feature beyond
                            // the first one in a tile must be from a writer
                            if (currentCount > 1) {
                                // Additional features beyond initial must be from writers
                                updatedFeaturesFound += (currentCount - 1);
                            }
                        }
                    } catch (...) {
                        metrics.readErrors++;
                    }
                }
            });
        }
        
        // Start writer threads that update tiles
        std::vector<std::thread> writers;
        for (int i = 0; i < 2; ++i) {
            writers.emplace_back([&cache, &tiles, &metrics, i]() {
                for (int j = 0; j < 50; ++j) {
                    try {
                        // Update tiles by adding more features
                        int tileIndex = j % tiles.size();
                        auto& tile = tiles[tileIndex];
                        // Store the area ID to ensure it outlives the string_view
                        auto areaId = "UpdatedArea" + std::to_string(i) + "_" + std::to_string(j);
                        tile->newFeature("Way", {
                            {"areaId", areaId},
                            {"wayId", 1000 + i * 100 + j}
                        });
                        cache->putTileLayer(tile);
                        metrics.successfulWrites++;
                    } catch (...) {
                        metrics.writeErrors++;
                    }
                }
            });
        }
        
        // Wait for all threads to complete
        joinThreads(readers);
        joinThreads(writers);
        
        // Verify no errors occurred
        REQUIRE(metrics.readErrors == 0);
        REQUIRE(metrics.writeErrors == 0);
        REQUIRE(metrics.successfulReads > 0);
        REQUIRE(metrics.successfulWrites == 100); // 2 writers * 50 writes each
        
        // Verify that readers saw updates from writers
        REQUIRE(updatedFeaturesFound > 0);
        
        // Check that at least some tiles have more than the initial 1 feature
        size_t tilesWithUpdates = 0;
        for (size_t i = 0; i < tiles.size(); ++i) {
            if (maxFeatureCounts[i].load() > 1) {
                tilesWithUpdates++;
            }
        }
        REQUIRE(tilesWithUpdates > 0);
        
        // Verify the final state - all tiles should have multiple features
        for (size_t i = 0; i < tiles.size(); ++i) {
            auto [tile, expiredAt] = cache->getTileLayer(tiles[i]->id(), info);
            REQUIRE(tile != nullptr);
            auto featureLayer = std::static_pointer_cast<TileFeatureLayer>(tile);
            // Each tile should have more than the initial 1 feature
            // Writers add features, so final count should be > 1
            REQUIRE(featureLayer->size() > 1);
        }
        
        // Clean up - reset cache to close DB connection before removing file
        cache.reset();
        std::filesystem::remove(test_cache);
    }
    
    SECTION("Concurrent write-write access") {
        auto test_cache = createTempCachePath("sqlite-concurrent-ww-test-");
        auto cache = std::make_shared<SQLiteCache>(0, test_cache.string(), true);
        
        // Create test data for multiple nodes
        std::vector<std::string> nodeIds = {"Node1", "Node2", "Node3", "Node4"};
        std::vector<std::string> mapIds = {"Map1", "Map2", "Map3", "Map4"};
        
        ConcurrentTestMetrics metrics;
        
        // Start multiple writer threads, each writing different tiles
        std::vector<std::thread> writers;
        for (size_t i = 0; i < nodeIds.size(); ++i) {
            writers.emplace_back([&cache, &layerInfo, &metrics, i, &nodeIds, &mapIds]() {
                auto nodeId = nodeIds[i];
                auto mapId = mapIds[i];
                auto strings = std::make_shared<StringPool>(nodeId);
                DataSourceInfo info = createTestDataSourceInfo(nodeId, mapId, layerInfo);
                
                for (int j = 0; j < 25; ++j) {
                    try {
                        // Create unique tiles for each thread
                        auto tileId = TileId::fromWgs84(40.0 + i, 10.0 + j * 0.1, 13);
                        auto tile = std::make_shared<TileFeatureLayer>(
                            tileId, nodeId, mapId, layerInfo, strings);
                        
                        // Add multiple features
                        // Store area ID to ensure it outlives the string_view
                        auto areaId = "Area" + std::to_string(i * 100 + j);
                        
                        for (int k = 0; k < 5; ++k) {
                            // Build feature ID parts explicitly to work around lambda type deduction
                            KeyValueViewPairs idParts;
                            idParts.emplace_back("areaId", areaId);
                            idParts.emplace_back("wayId", static_cast<int64_t>(i * 1000 + j * 10 + k));
                            
                            tile->newFeature("Way", idParts);
                        }
                        
                        cache->putTileLayer(tile);
                        metrics.successfulWrites++;
                        
                        // Also test string pool writes
                        if (j % 5 == 0) {
                            std::string poolData = "TestPool_" + nodeId + "_" + std::to_string(j);
                            cache->putStringPoolBlob(nodeId + "_" + std::to_string(j), poolData);
                        }
                    } catch (...) {
                        metrics.writeErrors++;
                    }
                }
            });
        }
        
        // Wait for all writers to complete
        joinThreads(writers);
        
        // Verify no errors occurred
        REQUIRE(metrics.writeErrors == 0);
        REQUIRE(metrics.successfulWrites == 100); // 4 writers * 25 writes each
        
        // Verify all data was written correctly by reading it back
        for (size_t i = 0; i < nodeIds.size(); ++i) {
            auto nodeId = nodeIds[i];
            auto mapId = mapIds[i];
            DataSourceInfo info = createTestDataSourceInfo(nodeId, mapId, layerInfo);
            
            for (int j = 0; j < 25; ++j) {
                auto tileId = TileId::fromWgs84(40.0 + i, 10.0 + j * 0.1, 13);
                // Need to create a temporary tile to get the proper MapTileKey
                auto tempTile = std::make_shared<TileFeatureLayer>(
                    tileId, nodeId, mapId, layerInfo, std::make_shared<StringPool>(nodeId));
                auto tile = cache->getTileLayer(tempTile->id(), info);
                REQUIRE(tile.tile != nullptr);
                auto featureLayer = std::static_pointer_cast<TileFeatureLayer>(tile.tile);
                REQUIRE(featureLayer->size() == 5); // Each tile should have 5 features
            }
        }
        
        // Clean up - reset cache to close DB connection before removing file
        cache.reset();
        std::filesystem::remove(test_cache);
    }
    
    SECTION("Multiple concurrent readers") {
        auto test_cache = createTempCachePath("sqlite-concurrent-readers-test-");
        auto cache = std::make_shared<SQLiteCache>(0, test_cache.string(), true);
        
        // Create test data
        auto nodeId = "ReaderTestNode";
        auto mapId = "ReaderTestMap";
        auto strings = std::make_shared<StringPool>(nodeId);
        DataSourceInfo info = createTestDataSourceInfo(nodeId, mapId, layerInfo);
        
        // Write some initial data
        std::vector<TileId> tileIds;
        for (int i = 0; i < 20; ++i) {
            auto tileId = TileId::fromWgs84(42.0 + i * 0.1, 11.0, 13);
            tileIds.push_back(tileId);
            auto tile = createTestTile(tileId, nodeId, mapId, layerInfo, strings, 1);
            cache->putTileLayer(tile);
        }
        
        ConcurrentTestMetrics metrics;
        
        // Start many reader threads
        std::vector<std::thread> readers;
        for (int i = 0; i < 10; ++i) {
            readers.emplace_back([&cache, &tileIds, &info, &metrics, &nodeId, &mapId, &layerInfo, &strings]() {
                for (int j = 0; j < 50; ++j) {
                    try {
                        // Read tiles in a pattern
                        for (const auto& tileId : tileIds) {
                            // Need to create a temporary tile to get the proper MapTileKey
                            auto tempTile = std::make_shared<TileFeatureLayer>(
                                tileId, nodeId, mapId, layerInfo, strings);
                            auto tile = cache->getTileLayer(tempTile->id(), info);
                            if (tile.tile) {
                                metrics.successfulReads++;
                                // Verify we can cast and access the data
                                auto featureLayer = std::static_pointer_cast<TileFeatureLayer>(tile.tile);
                                REQUIRE(featureLayer->size() == 1);
                            }
                        }
                    } catch (...) {
                        metrics.readErrors++;
                    }
                }
            });
        }
        
        // Wait for all readers to complete
        joinThreads(readers);
        
        // Verify no errors occurred and all reads were successful
        REQUIRE(metrics.readErrors == 0);
        REQUIRE(metrics.successfulReads == 10000); // 10 readers * 50 iterations * 20 tiles
        
        // Clean up - reset cache to close DB connection before removing file
        cache.reset();
        std::filesystem::remove(test_cache);
    }
}

TEST_CASE("NullCache", "[Cache]")
{
    testNullCacheImplementation();
}
