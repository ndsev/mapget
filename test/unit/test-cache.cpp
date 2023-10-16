#include <catch2/catch_test_macros.hpp>
#include "mapget/service/rocksdbcache.h"
#include "mapget/log.h"

using namespace mapget;

TEST_CASE("RocksDBCache", "[Cache]")
{
    auto cache = mapget::RocksDBCache();

    mapget::setLogLevel("trace", log());

    // TODO Make layer creation in test-model reusable.
    // Currently, TileFeatureLayer deserialization test fails if layerInfo and
    // fieldNames direct access is replaced with TileFeatureLayer functions.

    // Create layer info which has a single feature type with
    // a single allowed feature id composition.
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
                            "description": "String which identifies the map area.",
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

    // Create empty shared autofilled field-name dictionary
    auto fieldNames = std::make_shared<Fields>("TastyTomatoSaladNode");

    // Create a basic TileFeatureLayer
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "TastyTomatoSaladNode",
        "Tropico",
        layerInfo,
        fieldNames);

    // Test creating a feature while tile prefix is not set
    auto feature0 = tile->newFeature("Way", {{"areaId", "MediocreArea"}, {"wayId", 24}});

    // Set the tile feature id prefix
    tile->setPrefix({{"areaId", "TheBestArea"}});

    // Create a feature with line geometry
    auto feature1 = tile->newFeature("Way", {{"wayId", 42}});
    auto line = feature1->geom()->newGeometry(Geometry::GeomType::Line, 2);
    line->append({41., 10.});
    line->append({43., 11.});

    SECTION("Store layer") {
        cache.putTileFeatureLayer(tile);
    }

}
