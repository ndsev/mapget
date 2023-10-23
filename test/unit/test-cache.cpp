#include <catch2/catch_test_macros.hpp>
#include "mapget/http-service/cli.h"
#include "mapget/log.h"
#include "mapget/service/rocksdbcache.h"

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
        assert(fieldDictCount == 0);

        // putTileFeatureLayer triggers both putTileLayer and putFields.
        cache->putTileFeatureLayer(tile);
        auto returnedTile = cache->getTileFeatureLayer(tile->id(), info);
        fieldDictCount = cache->getStatistics()["loaded-field-dicts"].get<int>();
        assert(fieldDictCount == 1);

        // Update a tile, check that cache returns the updated version.
        assert(returnedTile->size() == 0);
        auto feature = tile->newFeature("Way", {{"areaId", "MediocreArea"}, {"wayId", 24}});
        cache->putTileFeatureLayer(tile);
        auto updatedTile = cache->getTileFeatureLayer(tile->id(), info);
        assert(updatedTile->size() == 1);

        // Check that cache hits and misses are properly recorded.
        // TODO discuss: cache hit/miss stats are not persistent, is that a problem?
        auto missingTile = cache->getTileFeatureLayer(otherTile->id(), otherInfo);
        assert(cache->getStatistics()["cache-hits"] == 2);
        assert(cache->getStatistics()["cache-misses"] == 1);
    }

    SECTION("Reopen cache, insert another feature layer") {
        // Open existing cache.
        auto cache = std::make_shared<mapget::RocksDBCache>(
            1024, "mapget-cache", false);
        auto fieldDictCount = cache->getStatistics()["loaded-field-dicts"].get<int>();
        assert(fieldDictCount == 1);

        cache->putTileFeatureLayer(otherTile);

        auto returnedTile = cache->getTileFeatureLayer(otherTile->id(), otherInfo);
        assert(returnedTile->nodeId() == otherTile->nodeId());

        // Field dicts are updated with getTileFeatureLayer.
        fieldDictCount = cache->getStatistics()["loaded-field-dicts"].get<int>();
        assert(fieldDictCount == 2);
    }

    SECTION("Reopen cache, check loading of field dicts") {
        // Open existing cache.
        auto cache = std::make_shared<mapget::RocksDBCache>();
        assert(cache->getStatistics()["loaded-field-dicts"] == 2);
    }

    SECTION("Check cache clearing") {
        // TODO open cache with clearCache=true.
    }

    SECTION("Limit cache size") {
        // TODO test cacheMaxTiles.
        // Set to 1 upon opening, store 2 layers, check that the first was purged.
    }

    SECTION("Create cache under a custom path") {
        std::filesystem::path test_cache = std::filesystem::current_path() / "rocksdb-unit-test";
        log().info(stx::format("Test creating cache: {}", test_cache.string()));

        // Delete cache if it already exists, e.g. from a broken test case.
        if (std::filesystem::exists(test_cache)) {
            std::filesystem::remove_all(test_cache);
        }
        assert(!std::filesystem::exists(test_cache));

        // Create cache and make sure it worked.
        auto cache = std::make_shared<mapget::RocksDBCache>(
            1024, test_cache, true);
        assert(std::filesystem::exists(test_cache));

        // Delete cache.
        std::filesystem::remove_all(test_cache);
    }
}
