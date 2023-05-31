#include <catch2/catch_test_macros.hpp>

#include "mapget/model/featurelayer.h"
#include "nlohmann/json.hpp"

#include <sstream>

using namespace mapget;

TEST_CASE("FeatureLayer", "[test.featurelayer]")
{
    // Create layer info which has a single feature type with
    // a single allowed feature id composition.
    auto layerInfo = std::make_shared<LayerInfo>();
    layerInfo->featureTypes_.emplace_back(FeatureTypeInfo{
        "Way",
        {{
            UniqueIdPart{"areaId", "String which identifies the map area.", IdPartDataType::STR, false, false},
            UniqueIdPart{"wayId", "Globally Unique 32b integer.",
              IdPartDataType::U32, false, false}
        }}});
    layerInfo->layerId_ = "WayLayer";

    // Create empty shared autofilled field-name dictionary
    auto fieldNames = std::make_shared<Fields>();

    // Create a basic TileFeatureLayer
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "TastyTomatoSaladNode",
        "GarlicChickenMap",
        layerInfo,
        fieldNames);
    tile->setPrefix({{"areaId", "TheBestArea"}});

    // Create a feature with line geometry
    auto feature1 = tile->newFeature("Way", {{"wayId", 42}});
    auto line = feature1->geom()->newGeometry(Geometry::GeomType::Line, 2);
    line->append({41., 10.});
    line->append({43., 11.});

    // Add a fixed attribute
    feature1->attributes()->addField("main_ingredient", "Pepper");

    // Add an attribute layer
    auto attrLayer = feature1->attributeLayers()->newLayer("cheese");
    auto attr = attrLayer->newAttribute("mozzarella");
    attr->setDirection(Attribute::Direction::Positive);
    attr->addField("smell", "neutral");

    SECTION("toGeoJSON")
    {
        constexpr auto expected = R"({"areaId":"TheBestArea","geometry":{"geometries":[{"coordinates":[[41.0,10.0,0.0],[43.0,11.0,0.0]],"type":"LineString"}],"type":"GeometryCollection"},"id":"Way.TheBestArea.42","properties":{"layer":{"cheese":{"mozzarella":{"direction":1,"smell":"neutral"}}},"main_ingredient":"Pepper"},"type":"Feature","typeId":"Way","wayId":42})";
        std::stringstream featureGeoJson;
        featureGeoJson << feature1->toGeoJson();
        REQUIRE(featureGeoJson.str() == expected);
    }

    SECTION("Basic field access")
    {
        REQUIRE(feature1->typeId() == "Way");
        REQUIRE(feature1->id()->toString() == "Way.TheBestArea.42");
    }

    SECTION("Evaluate simfil filter")
    {
        REQUIRE(feature1->evaluate("**.mozzarella.smell").toString() == "neutral");
        REQUIRE(feature1->evaluate("properties.main_ingredient").toString() == "Pepper");
        REQUIRE(
            feature1->evaluate("geo(geometry.geometries.*) within bbox(40., 9., 45., 12.)")
                .toString() == "true");
    }

    SECTION("Range-based for loop")
    {
        for (auto feature : *tile) {
            REQUIRE(feature->id()->toString() == feature1->id()->toString());
        }
    }

    SECTION("Serialization")
    {
        std::stringstream tileBytes;
        tile->write(tileBytes);

        auto deserializedTile = std::make_shared<TileFeatureLayer>(
            tileBytes,
            [&](auto&& mapName, auto&& layerName){
                REQUIRE(mapName == "GarlicChickenMap");
                REQUIRE(layerName == "WayLayer");
                return layerInfo;
            },
            [&](auto&& nodeId){
                REQUIRE(nodeId == "TastyTomatoSaladNode");
                return fieldNames;
            }
        );

        REQUIRE(deserializedTile->tileId() == tile->tileId());
        REQUIRE(deserializedTile->nodeId() == tile->nodeId());
        REQUIRE(deserializedTile->mapId() == tile->mapId());
        REQUIRE(deserializedTile->layerInfo() == tile->layerInfo());
        REQUIRE(deserializedTile->error() == tile->error());
        REQUIRE(deserializedTile->timestamp().time_since_epoch() == tile->timestamp().time_since_epoch());
        REQUIRE(deserializedTile->ttl() == tile->ttl());
        REQUIRE(deserializedTile->mapVersion() == tile->mapVersion());
        REQUIRE(deserializedTile->info() == tile->info());

        REQUIRE(deserializedTile->fieldNames() == tile->fieldNames());
        for (auto feature : *deserializedTile) {
            REQUIRE(feature->id()->toString() == feature1->id()->toString());
        }
    }
}

// Helper function to compare two points with some tolerance
void REQUIRE_EQUAL(const Point& p1, const Point& p2, double eps = 1e-6) {
    REQUIRE(std::abs(p1.x - p2.x) < eps);
    REQUIRE(std::abs(p1.y - p2.y) < eps);
}

TEST_CASE("TileId", "[TileId]") {
    using namespace mapget;

    SECTION("fromWgs84: zoom level 0") {
        TileId tile = TileId::fromWgs84(0, 0, 0);
        REQUIRE(tile.x() == 1);
        REQUIRE(tile.y() == 0);
        REQUIRE(tile.z() == 0);
    }

    SECTION("fromWgs84: positive longitude, positive latitude") {
        TileId tile = TileId::fromWgs84(90, 45, 1);
        REQUIRE(tile.x() == 3);
        REQUIRE(tile.y() == 0);
        REQUIRE(tile.z() == 1);
    }

    SECTION("fromWgs84: negative longitude, positive latitude") {
        TileId tile = TileId::fromWgs84(-90, 45, 1);
        REQUIRE(tile.x() == 1);
        REQUIRE(tile.y() == 0);
        REQUIRE(tile.z() == 1);
    }

    SECTION("fromWgs84: positive longitude, negative latitude") {
        TileId tile = TileId::fromWgs84(90, -45, 1);
        REQUIRE(tile.x() == 3);
        REQUIRE(tile.y() == 1);
        REQUIRE(tile.z() == 1);
    }

    SECTION("fromWgs84: negative longitude, negative latitude") {
        TileId tile = TileId::fromWgs84(-90, -45, 1);
        REQUIRE(tile.x() == 1);
        REQUIRE(tile.y() == 1);
        REQUIRE(tile.z() == 1);
    }

    SECTION("Tile center/SW/NE/size calculation") {
        TileId tile(0, 0, 0);
        REQUIRE_EQUAL(tile.center(), {-90, 0});
        REQUIRE_EQUAL(tile.sw(), {-180, -90});
        REQUIRE_EQUAL(tile.ne(), {0, 90});
        REQUIRE_EQUAL(tile.size(), {180, 180});
    }
}