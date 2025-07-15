#include <cstdint>
#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>

#include "mapget/http-service/cli.h"
#include "mapget/log.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/info.h"
#ifdef MAPGET_WITH_ROCKSDB
#include "mapget/service/rocksdbcache.h"
#endif
#include "mapget/service/sqlitecache.h"

using namespace mapget;

#ifdef MAPGET_WITH_ROCKSDB
TEST_CASE("RocksDBCache", "[Cache]")
{
    mapget::setLogLevel("trace", log());

    // TODO Make layer creation in test-model reusable.
    // Currently, TileFeatureLayer deserialization test in test-model.cpp
    // fails if layerInfo and strings access is replaced with
    // TileFeatureLayer functions.

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

    // Create one LayerInfo to be re-used by all DataSourceInfos.
    std::unordered_map<std::string, std::shared_ptr<LayerInfo>> layers;
    layers[layerInfo->layerId_] = std::make_shared<LayerInfo>(LayerInfo{
        layerInfo->layerId_,
        layerInfo->type_,
        std::vector<FeatureTypeInfo>(),
        std::vector<int>{0, 1, 2},
        std::vector<Coverage>{{1, 2, {}}, {3, 3, {}}},
        true,
        false,
        Version{0, 0, 0}});

    // Create a basic TileFeatureLayer.
    auto tileId = TileId::fromWgs84(42., 11., 13);
    auto nodeId = "CacheTestingNode";
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
    DataSourceInfo info(DataSourceInfo{
        nodeId,
        mapId,
        layers,
        5,
        false,
        nlohmann::json::object(),
        TileLayerStream::CurrentProtocolVersion});

    // Create another basic TileFeatureLayer for a different node.
    auto otherTileId = TileId::fromWgs84(42., 12., 13);
    auto otherNodeId = "OtherCacheTestingNode";
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
    DataSourceInfo otherInfo(DataSourceInfo{
        otherNodeId,
        otherMapId,
        layers,
        5,
        false,
        nlohmann::json::object(),
        TileLayerStream::CurrentProtocolVersion});


    auto testStringPoolNodeId = "StringPoolTestingNode";
    auto testStringPool = StringPool(testStringPoolNodeId);

    // Write a string pool directly into the cache, including the header
    // added in TileLayerStream::sendMessage.
    std::stringstream serializedStrings;
    testStringPool.write(serializedStrings, 0);

    std::stringstream serializedMessage;
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(serializedMessage);
    s.object(TileLayerStream::CurrentProtocolVersion);
    s.value1b(TileLayerStream::MessageType::StringPool);
    s.value4b((uint32_t)serializedStrings.str().size());
    serializedMessage << serializedStrings.str();

    auto getFeatureLayer = [](auto& cache, auto tileId, auto info) {
        auto layer = cache->getTileLayer(tileId, info);
        REQUIRE(!!layer);

        auto layerInfo = layer->layerInfo();
        REQUIRE(!!layerInfo);
        REQUIRE(layerInfo->type_ == mapget::LayerType::Features);
        return std::static_pointer_cast<TileFeatureLayer>(layer);
    };

    SECTION("Insert, retrieve, and update feature layer") {
        // Open or create cache, clear any existing data.
        auto cache = std::make_shared<mapget::RocksDBCache>(
            1024, "mapget-cache", true);
        auto stringPoolCount = cache->getStatistics()["loaded-string-pools"].get<int>();
        REQUIRE(stringPoolCount == 0);

        // putTileLayer triggers both putTileLayerBlob and putFieldsBlob.
        cache->putTileLayer(tile);
        auto returnedTile = getFeatureLayer(cache, tile->id(), info);
        stringPoolCount = cache->getStatistics()["loaded-string-pools"].get<int>();
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
        auto cache = std::make_shared<mapget::RocksDBCache>(
            1, "mapget-cache", false);
        auto stringPoolCount = cache->getStatistics()["loaded-string-pools"].get<int>();
        REQUIRE(stringPoolCount == 1);

        // Add a tile to trigger cache cleaning.
        cache->putTileLayer(otherTile);

        auto returnedTile = getFeatureLayer(cache, otherTile->id(), otherInfo);
        REQUIRE(returnedTile->nodeId() == otherTile->nodeId());

        // String pools are updated with getTileLayer.
        stringPoolCount = cache->getStatistics()["loaded-string-pools"].get<int>();
        REQUIRE(stringPoolCount == 2);

        // Query the first inserted layer - it should not be retrievable.
        auto missingTile = cache->getTileLayer(tile->id(), info);
        REQUIRE(cache->getStatistics()["cache-misses"] == 1);
        REQUIRE(!missingTile);
    }

    SECTION("Store another tile at unlimited cache size") {
        auto cache = std::make_shared<mapget::RocksDBCache>(
            0, "mapget-cache", false);

        // Insert another tile for the next test.
        cache->putTileLayer(tile);

        // Make sure the previous tile is still there, since cache is unlimited.
        auto olderTile = cache->getTileLayer(otherTile->id(), otherInfo);
        REQUIRE(cache->getStatistics()["cache-misses"] == 0);
        REQUIRE(cache->getStatistics()["cache-hits"] == 1);
    }

    SECTION("Reopen cache with maxTileCount=1, check older tile was deleted") {
        auto cache = std::make_shared<mapget::RocksDBCache>(
            1, "mapget-cache", false);
        REQUIRE(cache->getStatistics()["cache-misses"] == 0);

        // Query the first inserted layer - it should not be retrievable.
        auto missingTile = cache->getTileLayer(otherTile->id(), otherInfo);
        REQUIRE(cache->getStatistics()["cache-misses"] == 1);
    }

    SECTION("Reopen cache, check loading of string pools") {
        // Open existing cache.
        auto cache = std::make_shared<mapget::RocksDBCache>();
        REQUIRE(cache->getStatistics()["loaded-string-pools"] == 2);

        cache->putStringPoolBlob(testStringPoolNodeId, serializedMessage.str());
        auto returnedEntry = cache->getStringPoolBlob(testStringPoolNodeId);

        // Make sure the string pool was properly stored.
        REQUIRE(returnedEntry.value() == serializedMessage.str());

        // TODO this kind of access bypasses the creation of a stringPoolOffsets_
        //  entry in the cache -> decouple the cache storage logic clearly from
        //  tile layer writing logic.
        REQUIRE(cache->getStatistics()["loaded-string-pools"] == 2);
    }

    SECTION("Reopen cache again, check loading of string pools again") {
        // Open existing cache.
        auto cache = std::make_shared<mapget::RocksDBCache>();
        REQUIRE(cache->getStatistics()["loaded-string-pools"] == 3);

        // Check that the same value can still be retrieved from string pooln.
        auto returnedEntry = cache->getStringPoolBlob(testStringPoolNodeId);
        REQUIRE(returnedEntry.value() == serializedMessage.str());
    }

    SECTION("Create cache under a custom path") {
        auto now = std::chrono::system_clock::now();
        auto epoch_time = std::chrono::system_clock::to_time_t(now);

        std::filesystem::path test_cache = std::filesystem::temp_directory_path() /
            ("rocksdb-unit-test-" + std::to_string(epoch_time));
        log().info(fmt::format("Test creating cache: {}", test_cache.string()));

        // Delete cache if it already exists, e.g. from a broken test case.
        if (std::filesystem::exists(test_cache)) {
            std::filesystem::remove_all(test_cache);
        }
        REQUIRE(!std::filesystem::exists(test_cache));

        // Create cache and make sure it worked.
        auto cache = std::make_shared<mapget::RocksDBCache>(
            1024, test_cache.string(), true);
        REQUIRE(std::filesystem::exists(test_cache));
    }
}
#endif // MAPGET_WITH_ROCKSDB

TEST_CASE("SQLiteCache", "[Cache]")
{
    mapget::setLogLevel("trace", log());

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

    // Create one LayerInfo to be re-used by all DataSourceInfos.
    std::unordered_map<std::string, std::shared_ptr<LayerInfo>> layers;
    layers[layerInfo->layerId_] = std::make_shared<LayerInfo>(LayerInfo{
        layerInfo->layerId_,
        layerInfo->type_,
        std::vector<FeatureTypeInfo>(),
        std::vector<int>{0, 1, 2},
        std::vector<Coverage>{{1, 2, {}}, {3, 3, {}}},
        true,
        false,
        Version{0, 0, 0}});

    // Create a basic TileFeatureLayer.
    auto tileId = TileId::fromWgs84(42., 11., 13);
    auto nodeId = "SQLiteCacheTestingNode";
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
    DataSourceInfo info(DataSourceInfo{
        nodeId,
        mapId,
        layers,
        5,
        false,
        nlohmann::json::object(),
        TileLayerStream::CurrentProtocolVersion});

    // Create another basic TileFeatureLayer for a different node.
    auto otherTileId = TileId::fromWgs84(42., 12., 13);
    auto otherNodeId = "OtherSQLiteCacheTestingNode";
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
    DataSourceInfo otherInfo(DataSourceInfo{
        otherNodeId,
        otherMapId,
        layers,
        5,
        false,
        nlohmann::json::object(),
        TileLayerStream::CurrentProtocolVersion});


    auto testStringPoolNodeId = "SQLiteStringPoolTestingNode";
    auto testStringPool = StringPool(testStringPoolNodeId);

    // Write a string pool directly into the cache, including the header
    // added in TileLayerStream::sendMessage.
    std::stringstream serializedStrings;
    testStringPool.write(serializedStrings, 0);

    std::stringstream serializedMessage;
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(serializedMessage);
    s.object(TileLayerStream::CurrentProtocolVersion);
    s.value1b(TileLayerStream::MessageType::StringPool);
    s.value4b((uint32_t)serializedStrings.str().size());
    serializedMessage << serializedStrings.str();

    auto getFeatureLayer = [](auto& cache, auto tileId, auto info) {
        auto layer = cache->getTileLayer(tileId, info);
        REQUIRE(!!layer);

        auto layerInfo = layer->layerInfo();
        REQUIRE(!!layerInfo);
        REQUIRE(layerInfo->type_ == mapget::LayerType::Features);
        return std::static_pointer_cast<TileFeatureLayer>(layer);
    };

    SECTION("Insert, retrieve, and update feature layer") {
        // Open or create cache, clear any existing data.
        auto cache = std::make_shared<mapget::SQLiteCache>(
            1024, "mapget-cache.db", true);
        auto stringPoolCount = cache->getStatistics()["loaded-string-pools"].get<int>();
        REQUIRE(stringPoolCount == 0);

        // putTileLayer triggers both putTileLayerBlob and putFieldsBlob.
        cache->putTileLayer(tile);
        auto returnedTile = getFeatureLayer(cache, tile->id(), info);
        stringPoolCount = cache->getStatistics()["loaded-string-pools"].get<int>();
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
        auto cache = std::make_shared<mapget::SQLiteCache>(
            1, "mapget-cache.db", false);
        auto stringPoolCount = cache->getStatistics()["loaded-string-pools"].get<int>();
        REQUIRE(stringPoolCount == 1);

        // Add a tile to trigger cache cleaning.
        cache->putTileLayer(otherTile);

        auto returnedTile = getFeatureLayer(cache, otherTile->id(), otherInfo);
        REQUIRE(returnedTile->nodeId() == otherTile->nodeId());

        // String pools are updated with getTileLayer.
        stringPoolCount = cache->getStatistics()["loaded-string-pools"].get<int>();
        REQUIRE(stringPoolCount == 2);

        // Query the first inserted layer - it should not be retrievable.
        auto missingTile = cache->getTileLayer(tile->id(), info);
        REQUIRE(cache->getStatistics()["cache-misses"] == 1);
        REQUIRE(!missingTile);
    }

    SECTION("Store another tile at unlimited cache size") {
        auto cache = std::make_shared<mapget::SQLiteCache>(
            0, "mapget-cache.db", false);

        // Insert another tile for the next test.
        cache->putTileLayer(tile);

        // Make sure the previous tile is still there, since cache is unlimited.
        auto olderTile = cache->getTileLayer(otherTile->id(), otherInfo);
        REQUIRE(cache->getStatistics()["cache-misses"] == 0);
        REQUIRE(cache->getStatistics()["cache-hits"] == 1);
    }

    SECTION("Reopen cache with maxTileCount=1, check older tile was deleted") {
        auto cache = std::make_shared<mapget::SQLiteCache>(
            1, "mapget-cache.db", false);
        REQUIRE(cache->getStatistics()["cache-misses"] == 0);

        // Query the first inserted layer - it should not be retrievable.
        auto missingTile = cache->getTileLayer(otherTile->id(), otherInfo);
        REQUIRE(cache->getStatistics()["cache-misses"] == 1);
    }

    SECTION("Reopen cache, check loading of string pools") {
        // Open existing cache.
        auto cache = std::make_shared<mapget::SQLiteCache>();
        REQUIRE(cache->getStatistics()["loaded-string-pools"] == 2);

        cache->putStringPoolBlob(testStringPoolNodeId, serializedMessage.str());
        auto returnedEntry = cache->getStringPoolBlob(testStringPoolNodeId);

        // Make sure the string pool was properly stored.
        REQUIRE(returnedEntry.value() == serializedMessage.str());

        // TODO this kind of access bypasses the creation of a stringPoolOffsets_
        //  entry in the cache -> decouple the cache storage logic clearly from
        //  tile layer writing logic.
        REQUIRE(cache->getStatistics()["loaded-string-pools"] == 2);
    }

    SECTION("Reopen cache again, check loading of string pools again") {
        // Open existing cache.
        auto cache = std::make_shared<mapget::SQLiteCache>();
        REQUIRE(cache->getStatistics()["loaded-string-pools"] == 3);

        // Check that the same value can still be retrieved from string pooln.
        auto returnedEntry = cache->getStringPoolBlob(testStringPoolNodeId);
        REQUIRE(returnedEntry.value() == serializedMessage.str());
    }

    SECTION("Create cache under a custom path") {
        auto now = std::chrono::system_clock::now();
        auto epoch_time = std::chrono::system_clock::to_time_t(now);

        std::filesystem::path test_cache = std::filesystem::temp_directory_path() /
            ("sqlite-unit-test-" + std::to_string(epoch_time) + ".db");
        log().info(fmt::format("Test creating cache: {}", test_cache.string()));

        // Delete cache if it already exists, e.g. from a broken test case.
        if (std::filesystem::exists(test_cache)) {
            std::filesystem::remove_all(test_cache);
        }
        REQUIRE(!std::filesystem::exists(test_cache));

        // Create cache and make sure it worked.
        auto cache = std::make_shared<mapget::SQLiteCache>(
            1024, test_cache.string(), true);
        REQUIRE(std::filesystem::exists(test_cache));
    }
}
