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

    auto nodeId = "SpecialCacheTestingNode";
    auto mapId = "CacheMe";
    auto tileId = TileId::fromWgs84(42., 11., 13);

    DataSourceInfo info(DataSourceInfo{
        nodeId,
        mapId,
        layers,
        5,
        nlohmann::json::object(),
        TileLayerStream::CurrentProtocolVersion});

    // Create empty shared autofilled field-name dictionary
    auto fieldNames = std::make_shared<Fields>(nodeId);

    // Create a basic TileFeatureLayer.
    auto tile = std::make_shared<TileFeatureLayer>(
        tileId,
        nodeId,
        mapId,
        layerInfo,
        fieldNames);

    SECTION("Insert, retrieve, and update feature layers") {
        auto cache = std::make_shared<mapget::RocksDBCache>();

        // putTileFeatureLayer triggers both putTileLayer and putFields.
        cache->putTileFeatureLayer(tile);
        auto returnedTile = cache->getTileFeatureLayer(tile->id(), info);
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
