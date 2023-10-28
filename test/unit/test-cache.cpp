#include <catch2/catch_test_macros.hpp>
#include "mapget/http-service/cli.h"
#include "mapget/log.h"
#include "mapget/service/rocksdbcache.h"
#include <chrono>

using namespace mapget;

TEST_CASE("Cache settings via CLI", "[CLI-Cache]")
{
    mapget::runFromCommandLine(
        {"--config", "../../examples/config/sample-service.toml", "serve"}
    );
}

TEST_CASE("RocksDBCache", "[Cache]")
{
    mapget::setLogLevel("trace", log());

    // TODO Make layer creation in test-model reusable.
    // Currently, TileFeatureLayer deserialization test in test-model.cpp
    // fails if layerInfo and fieldNames access is replaced with
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
    std::map<std::string, std::shared_ptr<LayerInfo>> layers;
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
    // Create empty shared autofilled field-name dictionary.
    auto fieldNames = std::make_shared<Fields>(nodeId);
    auto tile = std::make_shared<TileFeatureLayer>(
        tileId,
        nodeId,
        mapId,
        layerInfo,
        fieldNames);
    // Create a DataSourceInfo object.
    DataSourceInfo info(DataSourceInfo{
        nodeId,
        mapId,
        layers,
        5,
        nlohmann::json::object(),
        TileLayerStream::CurrentProtocolVersion});

    // Create another basic TileFeatureLayer for a different node.
    auto otherTileId = TileId::fromWgs84(42., 12., 13);
    auto otherNodeId = "OtherCacheTestingNode";
    auto otherMapId = "CacheMeToo";
    // Create empty shared autofilled field-name dictionary.
    auto otherFieldNames = std::make_shared<Fields>(otherNodeId);
    auto otherTile = std::make_shared<TileFeatureLayer>(
        otherTileId,
        otherNodeId,
        otherMapId,
        layerInfo,
        otherFieldNames);
    // Create another DataSourceInfo object, but reuse the layer info.
    DataSourceInfo otherInfo(DataSourceInfo{
        otherNodeId,
        otherMapId,
        layers,
        5,
        nlohmann::json::object(),
        TileLayerStream::CurrentProtocolVersion});

    SECTION("Insert, retrieve, and update feature layer") {
        // Open or create cache, clear any existing data.
        auto cache = std::make_shared<mapget::RocksDBCache>(
            1024, "mapget-cache", true);
        auto fieldDictCount = cache->getStatistics()["loaded-field-dicts"].get<int>();
        REQUIRE(fieldDictCount == 0);

        // putTileFeatureLayer triggers both putTileLayer and putFields.
        cache->putTileFeatureLayer(tile);
        auto returnedTile = cache->getTileFeatureLayer(tile->id(), info);
        fieldDictCount = cache->getStatistics()["loaded-field-dicts"].get<int>();
        REQUIRE(fieldDictCount == 1);

        // Update a tile, check that cache returns the updated version.
        REQUIRE(returnedTile->size() == 0);
        auto feature = tile->newFeature("Way", {{"areaId", "MediocreArea"}, {"wayId", 24}});
        cache->putTileFeatureLayer(tile);
        auto updatedTile = cache->getTileFeatureLayer(tile->id(), info);
        REQUIRE(updatedTile->size() == 1);

        // Check that cache hits and misses are properly recorded.
        auto missingTile = cache->getTileFeatureLayer(otherTile->id(), otherInfo);
        REQUIRE(cache->getStatistics()["cache-hits"] == 2);
        REQUIRE(cache->getStatistics()["cache-misses"] == 1);
    }

    SECTION("Reopen cache with maxTileCount=1, insert another layer") {
        // Open existing cache.
        auto cache = std::make_shared<mapget::RocksDBCache>(
            1, "mapget-cache", false);
        auto fieldDictCount = cache->getStatistics()["loaded-field-dicts"].get<int>();
        REQUIRE(fieldDictCount == 1);

        // Add a tile to trigger cache cleaning.
        cache->putTileFeatureLayer(otherTile);

        auto returnedTile = cache->getTileFeatureLayer(otherTile->id(), otherInfo);
        REQUIRE(returnedTile->nodeId() == otherTile->nodeId());

        // Field dicts are updated with getTileFeatureLayer.
        fieldDictCount = cache->getStatistics()["loaded-field-dicts"].get<int>();
        REQUIRE(fieldDictCount == 2);

        // Query the first inserted layer - it should not be retrievable.
        auto missingTile = cache->getTileFeatureLayer(tile->id(), info);
        REQUIRE(cache->getStatistics()["cache-misses"] == 1);
        REQUIRE(!missingTile);
    }

    SECTION("Store another tile at unlimited cache size") {
        auto cache = std::make_shared<mapget::RocksDBCache>(
            0, "mapget-cache", false);

        // Insert another tile for the next test.
        cache->putTileFeatureLayer(tile);

        // Make sure the previous tile is still there, since cache is unlimited.
        auto olderTile = cache->getTileFeatureLayer(otherTile->id(), otherInfo);
        REQUIRE(cache->getStatistics()["cache-misses"] == 0);
        REQUIRE(cache->getStatistics()["cache-hits"] == 1);
    }

    SECTION("Reopen cache with maxTileCount=1, check older tile was deleted") {
        auto cache = std::make_shared<mapget::RocksDBCache>(
            1, "mapget-cache", false);
        REQUIRE(cache->getStatistics()["cache-misses"] == 0);

        // Query the first inserted layer - it should not be retrievable.
        auto missingTile = cache->getTileFeatureLayer(otherTile->id(), otherInfo);
        REQUIRE(cache->getStatistics()["cache-misses"] == 1);
    }

    SECTION("Reopen cache, check loading of field dicts") {
        // Open existing cache.
        auto cache = std::make_shared<mapget::RocksDBCache>();
        REQUIRE(cache->getStatistics()["loaded-field-dicts"] == 2);

        // Replace field dict value with test value.
        auto val = cache->getFields(nodeId);
        std::string fieldDictEntry = "test";
        cache->putFields(nodeId, fieldDictEntry);
        auto returnedEntry = cache->getFields(nodeId);

        // Make sure field dict was properly stored, replacing previous value.
        REQUIRE(returnedEntry == fieldDictEntry);
        REQUIRE(cache->getStatistics()["loaded-field-dicts"] == 2);
    }

    SECTION("Create cache under a custom path") {
        auto now = std::chrono::system_clock::now();
        auto epoch_time = std::chrono::system_clock::to_time_t(now);

        std::filesystem::path test_cache = std::filesystem::temp_directory_path() /
            ("rocksdb-unit-test-" + std::to_string(epoch_time));
        log().info(stx::format("Test creating cache: {}", test_cache.string()));

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
