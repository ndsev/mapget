#include <catch2/catch_test_macros.hpp>

#include "mapget/model/featurelayer.h"
#include "nlohmann/json.hpp"
#include <iostream>

using namespace mapget;

TEST_CASE("FeatureLayer", "[test.featurelayer]")
{
    auto layerInfo = std::make_shared<LayerInfo>();
    layerInfo->featureTypes.emplace_back(FeatureTypeInfo{
        "Way",
        {{
            UniqueIdPart{"mapId", "String which identifies the map.", IdPartDataType::STR, false, false},
            UniqueIdPart{"wayId", "Globally Unique 32b integer.",
              IdPartDataType::U32, false, false}
        }}});
    auto fieldNames = std::make_shared<Fields>();

    // Create a basic TileFeatureLayer
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "TastyTomatoSalad",
        "GarlicChicken",
        layerInfo,
        fieldNames);
    tile->setPrefix({{"mapId", "TheBestMap"}});

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

    // Print the feature in GeoJSON format
    std::cout << to_string(feature1->toGeoJson()) << std::endl;

    // Evaluate some simfil filters on the feature
    REQUIRE(feature1->typeId() == "Way");
    REQUIRE(feature1->id()->toString() == "Way.TheBestMap.42");
    REQUIRE(feature1->evaluate("**.mozzarella.smell").toString() == "neutral");
    REQUIRE(feature1->evaluate("properties.main_ingredient").toString() == "Pepper");
    REQUIRE(feature1->evaluate("geo(geometry.*.*) within bbox(40., 9., 45., 12.)").toString() == "true");
}

TEST_CASE("TileId fromWgs84", "[TileId]") {
    using namespace mapget;

    SECTION("zoom level 0") {
        TileId tile = TileId::fromWgs84(0, 0, 0);
        REQUIRE(tile.x() == 1);
        REQUIRE(tile.y() == 0);
        REQUIRE(tile.z() == 0);
    }

    SECTION("positive longitude, positive latitude") {
        TileId tile = TileId::fromWgs84(90, 45, 1);
        REQUIRE(tile.x() == 3);
        REQUIRE(tile.y() == 0);
        REQUIRE(tile.z() == 1);
    }

    SECTION("negative longitude, positive latitude") {
        TileId tile = TileId::fromWgs84(-90, 45, 1);
        REQUIRE(tile.x() == 1);
        REQUIRE(tile.y() == 0);
        REQUIRE(tile.z() == 1);
    }

    SECTION("positive longitude, negative latitude") {
        TileId tile = TileId::fromWgs84(90, -45, 1);
        REQUIRE(tile.x() == 3);
        REQUIRE(tile.y() == 1);
        REQUIRE(tile.z() == 1);
    }

    SECTION("negative longitude, negative latitude") {
        TileId tile = TileId::fromWgs84(-90, -45, 1);
        REQUIRE(tile.x() == 1);
        REQUIRE(tile.y() == 1);
        REQUIRE(tile.z() == 1);
    }
}