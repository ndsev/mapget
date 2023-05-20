#include <catch2/catch_test_macros.hpp>

#include "mapget/model/featurelayer.h"

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
    auto featureLayer = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "TastyTomatoSalad",
        "GarlicChicken",
        layerInfo,
        fieldNames);

    featureLayer->setPrefix({{"mapId", "TheBestMap"}});

    auto feature1 = featureLayer->newFeature("Way", {{"wayId", 42}});
    REQUIRE(feature1->typeId() == "Way");
    REQUIRE(feature1->id()->toString() == "Way.TheBestMap.42");
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