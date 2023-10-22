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

    // Create a DataSourceInfo object.
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

    DataSourceInfo otherInfo(DataSourceInfo{
        otherNodeId,
        otherMapId,
        layers,
        5,
        nlohmann::json::object(),
        TileLayerStream::CurrentProtocolVersion});

    SECTION("Insert, retrieve, and update feature layers") {
        // Open or create cache, clear any existing data.
        auto cache = std::make_shared<mapget::RocksDBCache>(
            1024, "mapget-cache", true);
        assert(cache->getStatistics()["loaded-field-dicts"] == 0);

        // putTileFeatureLayer triggers both putTileLayer and putFields.
        cache->putTileFeatureLayer(tile);
        auto returnedTile = cache->getTileFeatureLayer(tile->id(), info);
        assert(cache->getStatistics()["loaded-field-dicts"] == 1);
    }

    SECTION("Reopen cache, insert a feature layer") {
        // Open existing cache.
        auto cache = std::make_shared<mapget::RocksDBCache>();
        assert(cache->getStatistics()["loaded-field-dicts"] == 1);

        cache->putTileFeatureLayer(otherTile);

        auto returnedTile = cache->getTileFeatureLayer(otherTile->id(), otherInfo);
        assert(returnedTile->nodeId() == otherTile->nodeId());

        // Loaded field dicts, get updated for  the DS info with getTileFeatureLayer.
        assert(cache->getStatistics()["loaded-field-dicts"] == 2);
    }

    SECTION("Reopen cache, check field dicts loading") {
        // Open existing cache.
        auto cache = std::make_shared<mapget::RocksDBCache>();
        assert(cache->getStatistics()["loaded-field-dicts"] == 2);
    }

    SECTION("Clear cache") {
        // TODO
    }

    SECTION("Limit cache size") {

    }

    SECTION("Create cache under a custom path") {
        // TODO
    }

}
