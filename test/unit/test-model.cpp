#include <catch2/catch_test_macros.hpp>

#include "mapget/model/featurelayer.h"
#include "mapget/model/stream.h"
#include "nlohmann/json.hpp"
#include "mapget/log.h"

#include <iostream>

using namespace mapget;

TEST_CASE("FeatureLayer", "[test.featurelayer]")
{
    mapget::setLogLevel("trace", log());

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
                    ],
                    [
                        {
                            "partId": "areaId",
                            "description": "String which identifies the map area.",
                            "datatype": "STR"
                        },
                        {
                            "partId": "wayIdU32",
                            "description": "A 32b uinteger.",
                            "datatype": "U32"
                        },
                        {
                            "partId": "wayIdU64",
                            "description": "A 64b uinteger.",
                            "datatype": "U64"
                        },
                        {
                            "partId": "wayIdUUID128",
                            "description": "A UUID128, must have 16 bytes.",
                            "datatype": "UUID128"
                        }
                    ],
                    [
                        {
                            "partId": "areaId",
                            "description": "String which identifies the map area.",
                            "datatype": "STR"
                        },
                        {
                            "partId": "wayIdI32",
                            "description": "A 32b integer.",
                            "datatype": "I32"
                        },
                        {
                            "partId": "wayIdI64",
                            "description": "A 64b integer.",
                            "datatype": "I64"
                        },
                        {
                            "partId": "wayIdUUID128",
                            "description": "A UUID128, must have 16 bytes.",
                            "datatype": "UUID128"
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

    // Use high-level geometry API
    feature1->addPoint({41.5, 10.5, 0});
    feature1->addPoints({{41.5, 10.5, 0}, {41.6, 10.7}});
    feature1->addLine({{41.5, 10.5, 0}, {41.6, 10.7}});
    feature1->addMesh({{41.5, 10.5, 0}, {41.6, 10.7}, {41.5, 10.3}});
    feature1->addPoly({{41.5, 10.5, 0}, {41.6, 10.7}, {41.5, 10.3}, {41.8, 10.9}});

    // Add a fixed attribute
    feature1->attributes()->addField("main_ingredient", "Pepper");

    // Add an attribute layer
    auto attrLayer = feature1->attributeLayers()->newLayer("cheese");
    auto attr = attrLayer->newAttribute("mozzarella");
    attr->setDirection(Attribute::Direction::Positive);
    attr->addField("smell", "neutral");

    // Add other features using different ID compositions
    auto featureForId1 = tile->newFeature(
        "Way",
        {{"wayIdU32", 42}, {"wayIdU64", 84}, {"wayIdUUID128", "0123456789abcdef"}});
    auto featureForId2 = tile->newFeature(
        "Way",
        {{"wayIdI32", -42}, {"wayIdI64", -84}, {"wayIdUUID128", "0123456789abcdef"}});

    SECTION("toGeoJSON")
    {
        constexpr auto expected = R"({"areaId":"TheBestArea","geometry":{"geometries":[{"coordinates":[[41.0,10.0,0.0],[43.0,11.0,0.0]],"type":"LineString"},{"coordinates":[[41.5,10.5,0.0]],"type":"MultiPoint"},{"coordinates":[[41.5,10.5,0.0],[41.600000001490116,10.700000002980232,0.0]],"type":"MultiPoint"},{"coordinates":[[41.5,10.5,0.0],[41.600000001490116,10.700000002980232,0.0]],"type":"LineString"},{"coordinates":[[41.5,10.5,0.0],[41.600000001490116,10.700000002980232,0.0],[41.5,10.299999997019768,0.0]],"type":"MultiPolygon"},{"coordinates":[[41.5,10.5,0.0],[41.600000001490116,10.700000002980232,0.0],[41.5,10.299999997019768,0.0],[41.80000001192093,10.900000005960464,0.0]],"type":"Polygon"}],"type":"GeometryCollection"},"id":"Way.TheBestArea.42","properties":{"layer":{"cheese":{"mozzarella":{"direction":1,"smell":"neutral"}}},"main_ingredient":"Pepper"},"type":"Feature","typeId":"Way","wayId":42})";
        std::stringstream featureGeoJson;
        featureGeoJson << feature1->toGeoJson();
        log().trace(featureGeoJson.str());
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
            feature1->evaluate("any(geo() within bbox(40., 9., 45., 12.))")
                .toString() == "true");
    }

    SECTION("Range-based for loop")
    {
        for (auto feature : *tile) {
            REQUIRE(feature->id()->toString().substr(0, 16) == "Way.TheBestArea.");
        }
    }

    SECTION("Create feature ID with negative value in uint") {
        CHECK_THROWS(tile->newFeature(
            "Way",
            {{"wayIdU32", -4},
             {"wayIdU64", -2},
             {"wayIdUUID128", "0123456789abcdef"}}));
    }

    SECTION("Create feature ID with non-16-byte UUID128") {
        CHECK_THROWS(tile->newFeature(
            "Way",
            {{"wayIdU32", 4},
             {"wayIdU64", 2},
             {"wayIdUUID128", "not what you would expect"}}));
    }

    SECTION("Create feature ID with no matching composition") {
        CHECK_THROWS(tile->newFeature(
            "Way",
            {{"wayIdI32", -4},
             {"wayIdU64", 2},
             {"wayIdUUID128", "0123456789abcdef"}}));
    }

    SECTION("Serialization")
    {
        std::stringstream tileBytes;
        tile->write(tileBytes);

        auto deserializedTile = std::make_shared<TileFeatureLayer>(
            tileBytes,
            [&](auto&& mapName, auto&& layerName){
                REQUIRE(mapName == "Tropico");
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
            REQUIRE(feature->id()->toString().substr(0, 16) == "Way.TheBestArea.");
        }
    }

    SECTION("Stream")
    {
        // We will write the same tile into the stream twice,
        // but expect the Fields object to be sent only once.
        // Then we add another feature with a yet unseen field, send it,
        // and expect an update for the fields dictionary to be sent along.

        auto messageCount = 0;
        std::stringstream byteStream;
        TileLayerStream::FieldOffsetMap fieldOffsets;
        TileLayerStream::Writer layerWriter{[&](auto&& msg, auto&& type){
            ++messageCount;
            byteStream << msg;
        }, fieldOffsets};

        layerWriter.write(tile);
        REQUIRE(messageCount == 2);
        layerWriter.write(tile);
        REQUIRE(messageCount == 3);

        // Create another Feature
        auto feature2 = tile->newFeature("Way", {{"wayId", 43}});
        feature1->attributes()->addField("new_shiny_attr_name", "Salsa");

        layerWriter.write(tile);
        REQUIRE(messageCount == 5);

        // Now, read the stream in small chunks to verify that the Reader
        // always waits until the full message is received before trying
        // to parse an object.

        std::vector<TileFeatureLayer::Ptr> readTiles;
        TileLayerStream::Reader reader{
            [&](auto&& mapId, auto&& layerId) { return layerInfo; },
            [&](auto&& layerPtr) { readTiles.push_back(layerPtr); },
        };

        // Reading an empty buffer should not result in any tiles.
        reader.read("");
        REQUIRE(readTiles.empty());

        std::string byteStreamData = byteStream.str();
        for (auto i = 0; i < byteStreamData.size(); i += 2) {
            // Read two-byte chunks, except if only one byte is left
            reader.read(byteStreamData.substr(i, (i < byteStreamData.size() - 1) ? 2 : 1));
        }

        REQUIRE(reader.eos());
        REQUIRE(readTiles.size() == 3);
        REQUIRE(readTiles[0]->fieldNames() == readTiles[1]->fieldNames());
        REQUIRE(readTiles[1]->fieldNames() == readTiles[2]->fieldNames());
        REQUIRE(readTiles[0]->numRoots() == 4);
        REQUIRE(readTiles[1]->numRoots() == 4);
        REQUIRE(readTiles[2]->numRoots() == 5);
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