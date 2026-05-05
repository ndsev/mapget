#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "mapget/model/featurelayer.h"
#include "mapget/model/sourcedatareference.h"
#include "mapget/model/stream.h"
#include "nlohmann/json.hpp"
#include "mapget/log.h"
#include "nlohmann/json_fwd.hpp"

using namespace mapget;

namespace
{

using LayerAttributeValues = std::vector<std::tuple<std::string, std::string, std::string>>;

std::string stringValue(simfil::ModelNode::Ptr const& value)
{
    auto scalar = value->value();
    if (auto const* s = std::get_if<std::string>(&scalar)) {
        return *s;
    }
    if (auto const* sv = std::get_if<std::string_view>(&scalar)) {
        return std::string(*sv);
    }
    return {};
}

LayerAttributeValues collectLayerAttributeValues(model_ptr<AttributeLayerList> const& layers)
{
    LayerAttributeValues result;
    REQUIRE(layers);
    REQUIRE(layers->forEachLayer(
        [&](std::string_view layerName, model_ptr<AttributeLayer> const& layer) {
            return layer->forEachAttribute([&](model_ptr<Attribute> const& attr) {
                std::string fieldValue;
                REQUIRE(attr->forEachField(
                    [&](std::string_view const& key, simfil::ModelNode::Ptr const& value) {
                        if (key == "value") {
                            fieldValue = stringValue(value);
                        }
                        return true;
                    }));
                result.emplace_back(std::string(layerName), std::string(attr->name()), fieldValue);
                return true;
            });
        }));
    return result;
}

}  // namespace

TEST_CASE("FeatureLayer", "[test.featurelayer]")
{
    mapget::setLogLevel("trace", log());

    // Create layer info which has a single feature type with
    // several allowed feature id compositions.
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
    auto strings = std::make_shared<StringPool>("TastyTomatoSaladNode");

    // Create a basic TileFeatureLayer
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "TastyTomatoSaladNode",
        "Tropico",
        layerInfo,
        strings);

    // Set the tile's feature id prefix.
    tile->setIdPrefix({{"areaId", "TheBestArea"}});

    // Test creating a feature while tile prefix is not set.
    auto feature0 = tile->newFeature("Way", {{"wayId", 24}});

    // Setting the tile feature id prefix after a feature was added
    // must lead to a runtime error.
    REQUIRE_THROWS(tile->setIdPrefix({{"areaId", "TheBestArea"}}));

    // Create a feature with line geometry
    auto feature1 = tile->newFeature("Way", {{"wayId", 42}});
    auto line = feature1->geom()->newGeometry(GeomType::Line, 2);
    line->append({41., 10.});
    line->append({43., 11.});

    // Use high-level geometry API
    feature1->addPoint({41.5, 10.5, 0});
    feature1->addPoints({{41.5, 10.5, 0}, {41.6, 10.7}});
    feature1->addLine({{41.5, 10.5, 0}, {41.6, 10.7}});

    feature1->addMesh({{41.5, 10.5, 0}, {41.6, 10.7}, {41.5, 10.3}});

    feature1->addPoly({{41.5, 10.5, 0}, {41.6, 10.7}, {41.5, 10.3}, {41.8, 10.9}});

    // Unclosed polygon in CW order
    feature1->addPoly({{0, 1}, {1, 1}, {1, 0}, {0, 0}});

    // Closed polygon in CCW order
    feature1->addPoly({{1, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 0}});

    // Closed polygon in CW order with elevation
    feature1->addPoly({{2, 1, 0}, {3, 1, 1}, {3, 0, 2}, {2, 0, 3}, {2, 1, 0}});

    // Mesh of multiple triangles
    feature1->addMesh({{3, 0, 0}, {4, 0, 0}, {4, 1, 0}, {4, 1, 0}, {3, 1, 0}, {3, 0, 0}});

    // Add a fixed attribute
    feature1->attributes()->addField("main_ingredient", "Pepper");

    // Add an attribute layer
    auto attrLayer = feature1->attributeLayers()->newLayer("cheese");
    auto attr = attrLayer->newAttribute("mozzarella");
    attr->validity()->newDirection(Validity::Direction::Positive);
    attr->addField("smell", "neutral");

    // Add feature ids using secondary ID compositions
    auto featureForId1 = tile->newFeatureId(
        "Way",
        {{"wayIdU32", 42}, {"wayIdU64", 84}, {"wayIdUUID128", "0123456789abcdef"}});
    auto featureForId2 = tile->newFeatureId(
        "Way",
        {{"wayIdI32", -42}, {"wayIdI64", -84}, {"wayIdUUID128", "0123456789abcdef"}});
    auto externalFeatureId = tile->newFeatureId(
        "Way",
        {{"wayIdU32", 7}, {"wayIdU64", 11}, {"wayIdUUID128", "fedcba9876543210"}},
        "ValidationMap");

    SECTION("firstGeometry")
    {
        auto firstGeom = feature1->firstGeometry();
        REQUIRE(firstGeom.geomType_ == GeomType::Line);
    }

    SECTION("toJSON")
    {
        constexpr auto expected =
            R"({"areaId":"TheBestArea","geometry":{"geometries":[)"
            R"({"coordinates":[[41.0,10.0,0.0],[43.0,11.0,0.0]],"type":"LineString"},)"
            R"({"coordinates":[[41.5,10.5,0.0]],"type":"MultiPoint"},)"
            R"({"coordinates":[[41.5,10.5,0.0],[41.599999994039536,10.699999988079071,0.0]],"type":"MultiPoint"},)"
            R"({"coordinates":[[41.5,10.5,0.0],[41.599999994039536,10.699999988079071,0.0]],"type":"LineString"},)"
            R"({"coordinates":[[[[41.5,10.5,0.0],[41.5,10.300000011920929,0.0],[41.599999994039536,10.699999988079071,0.0],[41.5,10.5,0.0]]]],"type":"MultiPolygon"},)"
            R"({"coordinates":[[[41.5,10.5,0.0],[41.599999994039536,10.699999988079071,0.0],[41.5,10.300000011920929,0.0],[41.79999999701977,10.899999998509884,0.0],[41.5,10.5,0.0]]],"type":"Polygon"},)"
            R"({"coordinates":[[[0,1,0],[0,0,0],[1,0,0],[1,1,0],[0,1,0]]],"type":"Polygon"},)"  // Unclosed, CW
            R"({"coordinates":[[[1,0,0],[2,0,0],[2,1,0],[1,1,0],[1,0,0]]],"type":"Polygon"},)"  // Closed, CCW
            R"({"coordinates":[[[2,1,0],[3,1,1],[3,0,2],[2,0,3],[2,1,0]]],"type":"Polygon"},)"  // Closed, CW, Z!=0
            R"({"coordinates":[[[[3,0,0],[4,0,0],[4,1,0],[3,0,0]]],[[[4,1,0],[3,0,0],[3,1,0],[4,1,0]]]],"type":"MultiPolygon"})"  // Mesh
            R"(],"type":"GeometryCollection"},"id":"Way.TheBestArea.42","properties":{"layer":{"cheese":{"mozzarella":{"smell":"neutral","validity":{"direction":"POSITIVE"}}}},"main_ingredient":"Pepper"},"type":"Feature","typeId":"Way","wayId":42,)"
            R"("layerId":"WayLayer","mapId":"Tropico"})";

        auto res = feature1->toJson();
        auto exp = nlohmann::json::parse(expected);

        INFO(nlohmann::json::diff( exp, res).dump());
        REQUIRE(res == exp);
    }

    SECTION("Basic field access")
    {
        REQUIRE(feature1->typeId() == "Way");
        REQUIRE(feature1->id()->toString() == "Way.TheBestArea.42");
    }

    SECTION("Secondary feature ID compositions keep all labeled parts")
    {
        auto const keyValuePairs = featureForId1->keyValuePairs();
        REQUIRE(featureForId1->toString() == "Way.42.84.0123456789abcdef");
        REQUIRE(keyValuePairs.size() == 3);
        REQUIRE(keyValuePairs[0].first == "wayIdU32");
        REQUIRE(std::get<int64_t>(keyValuePairs[0].second) == 42);
        REQUIRE(keyValuePairs[1].first == "wayIdU64");
        REQUIRE(std::get<int64_t>(keyValuePairs[1].second) == 84);
        REQUIRE(keyValuePairs[2].first == "wayIdUUID128");
        REQUIRE(std::get<std::string_view>(keyValuePairs[2].second) == "0123456789abcdef");
    }

    SECTION("Detached feature IDs preserve optional external map IDs")
    {
        REQUIRE(featureForId1->toJson() == nlohmann::json("Way.42.84.0123456789abcdef"));
        REQUIRE(externalFeatureId->toString() == "Way.7.11.fedcba9876543210");
        REQUIRE(externalFeatureId->mapId() == "ValidationMap");
        REQUIRE(externalFeatureId->externalMapId() == std::optional<std::string_view>{"ValidationMap"});
        REQUIRE(externalFeatureId->toJson() == nlohmann::json{
            {"id", "Way.7.11.fedcba9876543210"},
            {"mapId", "ValidationMap"},
        });
    }

    SECTION("Evaluate simfil filter")
    {
        REQUIRE(feature1->evaluate("**.mozzarella.smell").value().toString() == "neutral");
        REQUIRE(feature1->evaluate("properties.main_ingredient").value().toString() == "Pepper");
        REQUIRE(
            feature1->evaluate("any(geo() within bbox(40., 9., 45., 12.))").value().toString() ==
            "true");
    }

    SECTION("Range-based for loop")
    {
        for (auto feature : *tile) {
            REQUIRE(feature->id()->toString().substr(0, 16) == "Way.TheBestArea.");
        }
    }

    SECTION("Create feature ID with negative value in uint") {
        CHECK_THROWS(tile->newFeatureId(
            "Way",
            {{"wayIdU32", -4},
             {"wayIdU64", -2},
             {"wayIdUUID128", "0123456789abcdef"}}));
    }

    SECTION("Create feature ID with non-16-byte UUID128") {
        CHECK_THROWS(tile->newFeatureId(
            "Way",
            {{"wayIdU32", 4},
             {"wayIdU64", 2},
             {"wayIdUUID128", "not what you would expect"}}));
    }

    SECTION("Create feature ID with no matching composition") {
        CHECK_THROWS(tile->newFeatureId(
            "Way",
            {{"wayIdI32", -4},
             {"wayIdU64", 2},
             {"wayIdUUID128", "0123456789abcdef"}}));
    }

    SECTION("Serialization")
    {
        tile->setGlbAttachment("city.glb", {0x67, 0x6c, 0x54, 0x46});

        std::stringstream tileBytes;
        tile->write(tileBytes);
        auto serializedTile = tileBytes.str();
        std::vector<uint8_t> tileBuffer(serializedTile.begin(), serializedTile.end());

        auto deserializedTile = std::make_shared<TileFeatureLayer>(
            tileBuffer,
            [&](auto&& mapName, auto&& layerName){
                REQUIRE(mapName == "Tropico");
                REQUIRE(layerName == "WayLayer");
                return layerInfo;
            },
            [&](auto&& nodeId){
                REQUIRE(nodeId == "TastyTomatoSaladNode");
                return strings;
            }
        );

        REQUIRE(deserializedTile->tileId() == tile->tileId());
        REQUIRE(deserializedTile->nodeId() == tile->nodeId());
        REQUIRE(deserializedTile->mapId() == tile->mapId());
        REQUIRE(deserializedTile->layerInfo() == tile->layerInfo());
        REQUIRE(deserializedTile->error() == tile->error());
        REQUIRE(deserializedTile->errorCode() == tile->errorCode());
        REQUIRE(deserializedTile->timestamp().time_since_epoch() == tile->timestamp().time_since_epoch());
        REQUIRE(deserializedTile->ttl() == tile->ttl());
        REQUIRE(deserializedTile->mapVersion() == tile->mapVersion());
        REQUIRE(deserializedTile->info() == tile->info());
        REQUIRE(deserializedTile->glbAttachment() != nullptr);
        REQUIRE(deserializedTile->glbAttachment()->name_ == "city.glb");
        REQUIRE(deserializedTile->glbAttachment()->bytes_ == std::vector<uint8_t>({0x67, 0x6c, 0x54, 0x46}));

        REQUIRE(deserializedTile->strings() == tile->strings());
        for (auto feature : *deserializedTile) {
            REQUIRE(feature->id()->toString().substr(0, 16) == "Way.TheBestArea.");
        }

        auto deserializedFeatureId = deserializedTile->resolve<FeatureId>(
            simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::ExternalFeatureIds, 0});
        REQUIRE(deserializedFeatureId);
        auto const deserializedKeyValuePairs = deserializedFeatureId->keyValuePairs();
        REQUIRE(deserializedKeyValuePairs.size() == 3);
        REQUIRE(deserializedKeyValuePairs[0].first == "wayIdU32");
        REQUIRE(std::get<int64_t>(deserializedKeyValuePairs[0].second) == 42);
        REQUIRE(deserializedKeyValuePairs[1].first == "wayIdU64");
        REQUIRE(std::get<int64_t>(deserializedKeyValuePairs[1].second) == 84);
        REQUIRE(deserializedKeyValuePairs[2].first == "wayIdUUID128");
        REQUIRE(std::get<std::string_view>(deserializedKeyValuePairs[2].second) == "0123456789abcdef");

        auto deserializedExternalFeatureId = deserializedTile->resolve<FeatureId>(
            simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::ExternalFeatureIds, 2});
        REQUIRE(deserializedExternalFeatureId);
        REQUIRE(deserializedExternalFeatureId->toString() == "Way.7.11.fedcba9876543210");
        REQUIRE(deserializedExternalFeatureId->mapId() == "ValidationMap");
        REQUIRE(
            deserializedExternalFeatureId->externalMapId() ==
            std::optional<std::string_view>{"ValidationMap"});
    }

    SECTION("Stream")
    {
        // We will write the same tile into the stream twice,
        // but expect the Fields object to be sent only once.
        // Then we add another feature with a yet unseen field, send it,
        // and expect an update for the fields dictionary to be sent along.
        tile->setGlbAttachment("city.glb", {0x67, 0x6c, 0x54, 0x46});

        auto messageCount = 0;
        std::stringstream byteStream;
        TileLayerStream::StringPoolOffsetMap stringOffsets;
        TileLayerStream::Writer layerWriter{[&](auto&& msg, auto&& type){
            ++messageCount;
            byteStream << msg;
        }, stringOffsets};

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
            [&](auto&& layerPtr) {
                if (auto featureLayer = std::dynamic_pointer_cast<TileFeatureLayer>(layerPtr))
                    readTiles.push_back(featureLayer);
            },
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
        REQUIRE(readTiles[0]->strings() == readTiles[1]->strings());
        REQUIRE(readTiles[1]->strings() == readTiles[2]->strings());
        REQUIRE(readTiles[0]->numRoots() == 2);
        REQUIRE(readTiles[1]->numRoots() == 2);
        REQUIRE(readTiles[2]->numRoots() == 3);
        REQUIRE(readTiles[0]->glbAttachment() != nullptr);
        REQUIRE(readTiles[1]->glbAttachment() != nullptr);
        REQUIRE(readTiles[2]->glbAttachment() != nullptr);
        REQUIRE(readTiles[2]->glbAttachment()->name_ == "city.glb");
        REQUIRE(readTiles[2]->glbAttachment()->bytes_ == std::vector<uint8_t>({0x67, 0x6c, 0x54, 0x46}));
    }

    SECTION("Find")
    {
        auto foundFeature01 = tile->find("Way", KeyValueViewPairs{{"areaId", "TheBestArea"}, {"wayId", 24}});
        REQUIRE(foundFeature01);
        REQUIRE(foundFeature01->addr() == feature0->addr());

        auto foundFeature01_2 = tile->find("Way.TheBestArea.24");
        REQUIRE(foundFeature01_2);
        REQUIRE(foundFeature01_2->addr() == feature0->addr());

        auto foundFeature11 = tile->find("Way", KeyValueViewPairs{{"areaId", "TheBestArea"}, {"wayId", 42}});
        REQUIRE(foundFeature11);
        REQUIRE(foundFeature11->addr() == feature1->addr());

        auto foundFeature00 = tile->find("Way", KeyValueViewPairs{{"areaId", "MediocreArea"}, {"wayId", 24}});
        REQUIRE(!foundFeature00);

        auto foundFeature00_2 = tile->find("Way.MediocreArea.24");
        REQUIRE(!foundFeature00_2);

        auto foundFeature10 = tile->find("Way", KeyValueViewPairs{{"wayId", 42}});
        REQUIRE(!foundFeature10);
    }

    SECTION("toJson with enhanced metadata")
    {
        // Set TTL
        tile->setTtl(std::chrono::milliseconds(3600000));
        tile->setGlbAttachment("city.glb", {0x67, 0x6c, 0x54, 0x46});

        auto json = tile->toJson();

        // Verify required fields
        REQUIRE(json["type"] == "FeatureCollection");
        REQUIRE(json["mapgetTileId"].is_number_unsigned());
        REQUIRE(json["mapgetTileId"].get<uint64_t>() == tile->tileId().value_);
        REQUIRE(json["mapId"] == "Tropico");
        REQUIRE(json["mapgetLayerId"] == "WayLayer");

        // Verify timestamp is ISO 8601 format
        REQUIRE(json["timestamp"].is_string());
        std::string timestamp = json["timestamp"];
        REQUIRE(timestamp.find("T") != std::string::npos);
        REQUIRE(timestamp.back() == 'Z');

        // Verify TTL
        REQUIRE(json["ttl"] == 3600000);

        REQUIRE(json["glbAttachment"].is_object());
        REQUIRE(json["glbAttachment"]["name"] == "city.glb");
        REQUIRE(json["glbAttachment"]["mimeType"] == "model/gltf-binary");
        REQUIRE(json["glbAttachment"]["sizeBytes"] == 4);

        // Verify features array exists
        REQUIRE(json["features"].is_array());
        REQUIRE(json["features"].size() == 2);  // feature0 and feature1

        // Verify no error object when no error is set
        REQUIRE(!json.contains("error"));
    }

    SECTION("GLB attachment can be replaced")
    {
        tile->setGlbAttachment("city.glb", {0x67, 0x6c, 0x54, 0x46});
        REQUIRE(tile->glbAttachment() != nullptr);
        REQUIRE(tile->glbAttachment()->name_ == "city.glb");

        tile->setGlbAttachment("city-updated.glb", {0x01, 0x02});
        REQUIRE(tile->glbAttachment() != nullptr);
        REQUIRE(tile->glbAttachment()->name_ == "city-updated.glb");
        REQUIRE(tile->glbAttachment()->bytes_ == std::vector<uint8_t>({0x01, 0x02}));

        tile->clearGlbAttachment();
        REQUIRE(tile->glbAttachment() == nullptr);
    }

    SECTION("toJson with error information")
    {
        // Set error message and code
        tile->setError("Test error message");
        tile->setErrorCode(404);

        auto json = tile->toJson();

        // Verify error object
        REQUIRE(json.contains("error"));
        REQUIRE(json["error"]["message"] == "Test error message");
        REQUIRE(json["error"]["code"] == 404);
    }

    SECTION("Serialization with errorCode")
    {
        // Set error information
        tile->setError("Connection timeout");
        tile->setErrorCode(504);
        tile->setTtl(std::chrono::milliseconds(60000));

        std::stringstream tileBytes;
        tile->write(tileBytes);
        auto serializedTile = tileBytes.str();
        std::vector<uint8_t> tileBuffer(serializedTile.begin(), serializedTile.end());

        auto deserializedTile = std::make_shared<TileFeatureLayer>(
            tileBuffer,
            [&](auto&& mapName, auto&& layerName){
                return layerInfo;
            },
            [&](auto&& nodeId){
                return strings;
            }
        );

        REQUIRE(deserializedTile->error() == tile->error());
        REQUIRE(deserializedTile->error().value() == "Connection timeout");
        REQUIRE(deserializedTile->errorCode() == tile->errorCode());
        REQUIRE(deserializedTile->errorCode().value() == 504);
        REQUIRE(deserializedTile->ttl() == tile->ttl());
        REQUIRE(deserializedTile->ttl().value().count() == 60000);
    }

    SECTION("Serialization with stage")
    {
        tile->setStage(3U);

        std::stringstream tileBytes;
        tile->write(tileBytes);
        auto serializedTile = tileBytes.str();
        std::vector<uint8_t> tileBuffer(serializedTile.begin(), serializedTile.end());

        auto deserializedTile = std::make_shared<TileFeatureLayer>(
            tileBuffer,
            [&](auto&&, auto&&) {
                return layerInfo;
            },
            [&](auto&&) {
                return strings;
            }
        );

        REQUIRE(deserializedTile->stage().has_value());
        REQUIRE(deserializedTile->stage().value() == 3U);
    }

    SECTION("Serialization without stage")
    {
        tile->setStage({});

        std::stringstream tileBytes;
        tile->write(tileBytes);
        auto serializedTile = tileBytes.str();
        std::vector<uint8_t> tileBuffer(serializedTile.begin(), serializedTile.end());

        auto deserializedTile = std::make_shared<TileFeatureLayer>(
            tileBuffer,
            [&](auto&&, auto&&) {
                return layerInfo;
            },
            [&](auto&&) {
                return strings;
            }
        );

        REQUIRE_FALSE(deserializedTile->stage().has_value());
    }
}

TEST_CASE("FeatureLayer stores geometry source-data refs compactly for singleton geometries",
          "[test.featurelayer][test.featurelayer.sourcedatarefs]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Way",
                "uniqueIdCompositions": [
                    [
                        {
                            "partId": "wayId",
                            "description": "Globally unique 32b integer.",
                            "datatype": "U32"
                        }
                    ]
                ]
            }
        ]
    })"_json);

    auto strings = std::make_shared<StringPool>("SourceDataRefNode");
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "SourceDataRefNode",
        "SourceDataRefMap",
        layerInfo,
        strings);

    auto feature = tile->newFeature("Way", {{"wayId", 42}});
    auto singletonPoint = feature->geom()->newGeometry(GeomType::Points, 1, true);
    singletonPoint->append({42., 11., 0.});
    auto line = feature->geom()->newGeometry(GeomType::Line, 2);
    line->append({42., 11., 0.});
    line->append({42.1, 11.1, 0.});

    QualifiedSourceDataReference singletonPointRef{
        .address_ = SourceDataAddress::fromBitPosition(8, 16),
        .layerId_ = strings->emplace("DisplayLayer").value(),
        .qualifier_ = strings->emplace("Position2D").value(),
    };
    QualifiedSourceDataReference lineRef{
        .address_ = SourceDataAddress::fromBitPosition(24, 32),
        .layerId_ = strings->emplace("DisplayLayer").value(),
        .qualifier_ = strings->emplace("Line2D").value(),
    };

    singletonPoint->setSourceDataReferences(
        tile->newSourceDataReferenceCollection({&singletonPointRef, 1}));
    line->setSourceDataReferences(
        tile->newSourceDataReferenceCollection({&lineRef, 1}));

    SECTION("refs are accessible before serialization")
    {
        REQUIRE(singletonPoint->sourceDataReferences());
        REQUIRE(singletonPoint->sourceDataReferences()->size() == 1);
        std::string singletonPointQualifier;
        singletonPoint->sourceDataReferences()->forEachReference([&](auto const& ref) {
            singletonPointQualifier = std::string(ref.qualifier());
        });
        REQUIRE(singletonPointQualifier == "Position2D");

        REQUIRE(line->sourceDataReferences());
        REQUIRE(line->sourceDataReferences()->size() == 1);
        std::string lineQualifier;
        line->sourceDataReferences()->forEachReference([&](auto const& ref) {
            lineQualifier = std::string(ref.qualifier());
        });
        REQUIRE(lineQualifier == "Line2D");

        auto sizeStats = tile->serializationSizeStats();
        REQUIRE(sizeStats["feature-layer"]["geometry-source-data-references"].get<int64_t>() < 1024);
    }

    SECTION("refs survive serialization roundtrip")
    {
        std::stringstream tileBytes;
        REQUIRE(tile->write(tileBytes).has_value());
        auto serializedTile = tileBytes.str();
        std::vector<uint8_t> tileBuffer(serializedTile.begin(), serializedTile.end());

        auto deserializedTile = std::make_shared<TileFeatureLayer>(
            tileBuffer,
            [&](auto&&, auto&&) {
                return layerInfo;
            },
            [&](auto&&) {
                return strings;
            });

        auto deserializedFeature = deserializedTile->at(0);
        REQUIRE(deserializedFeature);
        auto deserializedGeometries = deserializedFeature->geomOrNull();
        REQUIRE(deserializedGeometries);
        REQUIRE(deserializedGeometries->numGeometries() == 2);

        std::vector<simfil::model_ptr<Geometry>> deserializedGeometryList;
        deserializedGeometries->forEachGeometry([&](auto const& geometry) {
            deserializedGeometryList.push_back(geometry);
            return true;
        });
        REQUIRE(deserializedGeometryList.size() == 2);

        auto const& deserializedPoint = deserializedGeometryList[0];
        auto const& deserializedLine = deserializedGeometryList[1];
        REQUIRE(deserializedPoint->sourceDataReferences());
        REQUIRE(deserializedLine->sourceDataReferences());

        std::string deserializedPointQualifier;
        deserializedPoint->sourceDataReferences()->forEachReference([&](auto const& ref) {
            deserializedPointQualifier = std::string(ref.qualifier());
        });
        REQUIRE(deserializedPointQualifier == "Position2D");

        std::string deserializedLineQualifier;
        deserializedLine->sourceDataReferences()->forEachReference([&](auto const& ref) {
            deserializedLineQualifier = std::string(ref.qualifier());
        });
        REQUIRE(deserializedLineQualifier == "Line2D");

        auto sizeStats = deserializedTile->serializationSizeStats();
        REQUIRE(sizeStats["feature-layer"]["geometry-source-data-references"].get<int64_t>() < 1024);
    }
}

TEST_CASE("Feature LOD Field", "[test.featurelayer][test.feature.lod]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Way",
                "uniqueIdCompositions": [[
                    {"partId": "areaId", "datatype": "STR"},
                    {"partId": "wayId", "datatype": "U32"}
                ]]
            }
        ]
    })"_json);

    auto strings = std::make_shared<StringPool>("FeatureLodNode");
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "FeatureLodNode",
        "Tropico",
        layerInfo,
        strings);
    tile->setIdPrefix({{"areaId", "A"}});

    auto feature = tile->newFeature("Way", {{"wayId", int64_t(42)}});
    auto lodValueResult = feature->evaluate("lod");
    REQUIRE(lodValueResult.has_value());
    auto lodValue = lodValueResult.value().as<simfil::ValueType::Int>();
    REQUIRE(lodValue >= 0);
    REQUIRE(lodValue <= 7);
    REQUIRE(static_cast<int64_t>(feature->lod()) == static_cast<int64_t>(lodValue));
}

TEST_CASE("Feature IDs infill optional primary parts", "[test.featurelayer][test.feature.id.optionals]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Way",
                "uniqueIdCompositions": [[
                    {"partId": "areaId", "datatype": "STR"},
                    {"partId": "sideId", "datatype": "U32", "isOptional": true},
                    {"partId": "wayId", "datatype": "U32"}
                ]]
            }
        ]
    })"_json);

    auto strings = std::make_shared<StringPool>("FeatureOptionalIdNode");
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "FeatureOptionalIdNode",
        "Tropico",
        layerInfo,
        strings);
    tile->setIdPrefix({{"areaId", "A"}});

    auto withoutOptional = tile->newFeature("Way", {{"wayId", int64_t(42)}});
    auto withOptional = tile->newFeature("Way", {{"sideId", int64_t(7)}, {"wayId", int64_t(43)}});

    auto const withoutOptionalPairs = withoutOptional->id()->keyValuePairs();
    REQUIRE(withoutOptional->id()->toString() == "Way.A.42");
    REQUIRE(withoutOptionalPairs.size() == 2);
    REQUIRE(withoutOptionalPairs[0].first == "areaId");
    REQUIRE(std::get<std::string_view>(withoutOptionalPairs[0].second) == "A");
    REQUIRE(withoutOptionalPairs[1].first == "wayId");
    REQUIRE(std::get<int64_t>(withoutOptionalPairs[1].second) == 42);

    auto const withOptionalPairs = withOptional->id()->keyValuePairs();
    REQUIRE(withOptional->id()->toString() == "Way.A.7.43");
    REQUIRE(withOptionalPairs.size() == 3);
    REQUIRE(withOptionalPairs[0].first == "areaId");
    REQUIRE(std::get<std::string_view>(withOptionalPairs[0].second) == "A");
    REQUIRE(withOptionalPairs[1].first == "sideId");
    REQUIRE(std::get<int64_t>(withOptionalPairs[1].second) == 7);
    REQUIRE(withOptionalPairs[2].first == "wayId");
    REQUIRE(std::get<int64_t>(withOptionalPairs[2].second) == 43);
}

TEST_CASE("Single-entry validity collections are exposed as singular nodes", "[test.featurelayer.validity]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Way",
                "uniqueIdCompositions": [[
                    {"partId": "wayId", "description": "way id", "datatype": "U32"}
                ]]
            }
        ]
    })"_json);

    auto strings = std::make_shared<StringPool>("ValidityNode");
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "ValidityNode",
        "Tropico",
        layerInfo,
        strings);
    auto feature = tile->newFeature("Way", {{"wayId", 1}});

    auto attr = feature->attributeLayers()->newLayer("limits")->newAttribute("speed");
    attr->validity()->newDirection(Validity::Direction::Both);

    auto relation = tile->newRelation(
        "connectedTo",
        tile->newFeatureId("Way", {{"wayId", 2}}, "ValidationMap"));
    relation->sourceValidity()->newDirection(Validity::Direction::Positive);
    relation->targetValidity()->newDirection(Validity::Direction::Negative);
    feature->addRelation(relation);

    auto materializedAttr = tile->resolve<Attribute>(attr->addr());
    REQUIRE(materializedAttr);
    auto const& attrNode = static_cast<simfil::ModelNode const&>(*materializedAttr);
    auto const attrValidityNode = attrNode.get(StringPool::ValidityStr);
    REQUIRE(attrValidityNode);
    REQUIRE(attrValidityNode->toJson() == nlohmann::json{{"direction", "COMPLETE"}});

    auto materializedRelation = tile->resolve<Relation>(relation->addr());
    REQUIRE(materializedRelation);
    REQUIRE(materializedRelation->target()->mapId() == "ValidationMap");
    auto const& relationNode = static_cast<simfil::ModelNode const&>(*materializedRelation);
    auto const targetNode = relationNode.get(StringPool::TargetStr);
    REQUIRE(targetNode);
    REQUIRE(targetNode->toJson() == nlohmann::json{
        {"id", "Way.2"},
        {"mapId", "ValidationMap"},
    });
    auto const sourceValidityNode = relationNode.get(StringPool::SourceValidityStr);
    REQUIRE(sourceValidityNode);
    REQUIRE(sourceValidityNode->toJson() == nlohmann::json{{"direction", "POSITIVE"}});

    auto const targetValidityNode = relationNode.get(StringPool::TargetValidityStr);
    REQUIRE(targetValidityNode);
    REQUIRE(targetValidityNode->toJson() == nlohmann::json{{"direction", "NEGATIVE"}});
}

TEST_CASE("Feature-id validities expose external map references", "[test.featurelayer.validity]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Way",
                "uniqueIdCompositions": [[
                    {"partId": "wayId", "description": "way id", "datatype": "U32"}
                ]]
            }
        ]
    })"_json);

    auto strings = std::make_shared<StringPool>("FeatureRefValidityNode");
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "FeatureRefValidityNode",
        "Tropico",
        layerInfo,
        strings);
    auto feature = tile->newFeature("Way", {{"wayId", 1}});
    auto attr = feature->attributeLayers()->newLayer("limits")->newAttribute("speed");

    auto externalReference = tile->newFeatureId("Way", {{"wayId", 2}}, "ValidationMap");
    attr->validity()->newFeatureId(externalReference, Validity::Direction::Positive);

    auto materializedAttr = tile->resolve<Attribute>(attr->addr());
    REQUIRE(materializedAttr);
    auto const& attrNode = static_cast<simfil::ModelNode const&>(*materializedAttr);
    auto const validityNode = attrNode.get(StringPool::ValidityStr);
    REQUIRE(validityNode);
    REQUIRE(validityNode->toJson() == nlohmann::json{
        {"direction", "POSITIVE"},
        {"featureId", {
            {"id", "Way.2"},
            {"mapId", "ValidationMap"},
        }},
    });
}

TEST_CASE("Semantic feature transition validities expose semantic nodes", "[test.featurelayer.validity]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Way",
                "uniqueIdCompositions": [[
                    {"partId": "wayId", "description": "way id", "datatype": "U32"}
                ]]
            }
        ]
    })"_json);

    auto strings = std::make_shared<StringPool>("TransitionValidityNode");
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "TransitionValidityNode",
        "Tropico",
        layerInfo,
        strings);

    auto fromFeature = tile->newFeature("Way", {{"wayId", 1}});
    auto fromGeometry = fromFeature->geom()->newGeometry(GeomType::Line, 2);
    fromGeometry->append({0., 0., 0.});
    fromGeometry->append({1., 0., 0.});

    auto toFeature = tile->newFeature("Way", {{"wayId", 2}});
    auto toGeometry = toFeature->geom()->newGeometry(GeomType::Line, 2);
    toGeometry->append({1., 0., 0.});
    toGeometry->append({2., 0., 0.});

    auto intersection = tile->newFeature("Way", {{"wayId", 3}});
    auto attr = intersection->attributeLayers()->newLayer("rules")->newAttribute("turn");
    attr->validity()->newFeatureTransition(
        fromFeature,
        Validity::End,
        toFeature,
        Validity::Start,
        7);

    auto materializedAttr = tile->resolve<Attribute>(attr->addr());
    REQUIRE(materializedAttr);
    auto const& attrNode = static_cast<simfil::ModelNode const&>(*materializedAttr);
    auto const attrValidityNode = attrNode.get(StringPool::ValidityStr);
    REQUIRE(attrValidityNode);
    REQUIRE(attrValidityNode->toJson() == nlohmann::json{
        {"from", "Way.1"},
        {"fromConnectedEnd", "END"},
        {"to", "Way.2"},
        {"toConnectedEnd", "START"},
        {"transitionNumber", 7},
    });
}

TEST_CASE("Validity GeoJSON exposes stage labels only beyond the default stage", "[test.featurelayer.validity]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Way",
                "uniqueIdCompositions": [[
                    {"partId": "wayId", "description": "way id", "datatype": "U32"}
                ]]
            }
        ],
        "stages": 3,
        "stageLabels": ["Low-Fi", "High-Fi", "ADAS"],
        "highFidelityStage": 1
    })"_json);

    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "StageValidityNode",
        "Tropico",
        layerInfo,
        std::make_shared<StringPool>("StageValidityNode"));

    auto highFiValidity = tile->newValidity();
    highFiValidity->setGeometryStage(1U);
    highFiValidity->setDirection(Validity::Positive);
    auto resolvedHighFiValidity = tile->resolve<Validity>(highFiValidity->addr());
    REQUIRE(resolvedHighFiValidity);
    REQUIRE(resolvedHighFiValidity->toJson() == nlohmann::json{{"direction", "POSITIVE"}});

    auto adasValidity = tile->newValidity();
    adasValidity->setGeometryStage(2U);
    adasValidity->setDirection(Validity::Positive);
    auto resolvedAdasValidity = tile->resolve<Validity>(adasValidity->addr());
    REQUIRE(resolvedAdasValidity);
    REQUIRE(resolvedAdasValidity->toJson() == nlohmann::json{
        {"direction", "POSITIVE"},
        {"geometryName", "ADAS"},
    });
}

TEST_CASE("FeatureLayer Overlay Merged Views", "[test.featurelayer.overlay]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Way",
                "uniqueIdCompositions": [
                    [
                        {
                            "partId": "wayId",
                            "description": "Globally unique 32b integer.",
                            "datatype": "U32"
                        }
                    ]
                ]
            }
        ]
    })"_json);

    auto strings = std::make_shared<StringPool>("OverlayNode");

    auto makeTile = [&](std::string const& nodeName) {
        return std::make_shared<TileFeatureLayer>(
            TileId::fromWgs84(42., 11., 13),
            nodeName,
            "OverlayMap",
            layerInfo,
            strings);
    };

    auto base = makeTile("OverlayNode");
    auto overlayStage1 = makeTile("OverlayNode");
    auto overlayStage2 = makeTile("OverlayNode");

    auto baseFeature = base->newFeature("Way", {{"wayId", 1}});
    auto baseGeom = baseFeature->geom()->newGeometry(GeomType::Points, 1);
    baseGeom->append({10., 10., 0.});
    REQUIRE(baseFeature->attributes()->addField("plainA", "base").has_value());
    REQUIRE(baseFeature->attributes()->addField("overrideA", "base").has_value());
    auto baseLayer = baseFeature->attributeLayers()->newLayer("baseLayer");
    auto baseAttr = baseLayer->newAttribute("baseAttr");
    REQUIRE(baseAttr->addField("value", "base").has_value());
    baseFeature->addRelation("baseRel", base->newFeatureId("Way", {{"wayId", 100}}));

    auto overlayFeature1 = overlayStage1->newFeature("Way", {{"wayId", 1}});
    auto overlayGeom1 = overlayFeature1->geom()->newGeometry(GeomType::Points, 1);
    overlayGeom1->append({20., 20., 0.});
    REQUIRE(overlayFeature1->attributes()->addField("overrideA", "overlay1").has_value());
    auto overlayLayer1 = overlayFeature1->attributeLayers()->newLayer("overlayLayer1");
    auto overlayAttr1 = overlayLayer1->newAttribute("overlayAttr1");
    REQUIRE(overlayAttr1->addField("value", "overlay1").has_value());
    overlayFeature1->addRelation("overlayRel1", overlayStage1->newFeatureId("Way", {{"wayId", 101}}));

    auto overlayFeature2 = overlayStage2->newFeature("Way", {{"wayId", 1}});
    auto overlayGeom2 = overlayFeature2->geom()->newGeometry(GeomType::Points, 1);
    overlayGeom2->append({30., 30., 0.});
    REQUIRE(overlayFeature2->attributes()->addField("plainB", "overlay2").has_value());
    REQUIRE(overlayFeature2->attributes()->addField("overrideA", "overlay2").has_value());
    auto overlayLayer2 = overlayFeature2->attributeLayers()->newLayer("overlayLayer2");
    auto overlayAttr2 = overlayLayer2->newAttribute("overlayAttr2");
    REQUIRE(overlayAttr2->addField("value", "overlay2").has_value());
    overlayFeature2->addRelation("overlayRel2", overlayStage2->newFeatureId("Way", {{"wayId", 102}}));

    base->attachOverlay(overlayStage1);
    base->attachOverlay(overlayStage2);

    auto mergedFeature = base->at(0);
    REQUIRE(mergedFeature);

    SECTION("Typed access sees merged data")
    {
        REQUIRE(mergedFeature->geomOrNull()->numGeometries() == 3);
        REQUIRE(mergedFeature->mergedAttributesOrNull()->size() == 3);
        REQUIRE(mergedFeature->evaluate("properties.plainA").value().toString() == "base");
        REQUIRE(mergedFeature->evaluate("properties.plainB").value().toString() == "overlay2");
        REQUIRE(mergedFeature->evaluate("properties.overrideA").value().toString() == "overlay2");
        REQUIRE(mergedFeature->attributeLayersOrNull()->size() == 3);
        REQUIRE(mergedFeature->numRelations() == 3);
    }

    SECTION("ModelNode access sees merged geometry and relations")
    {
        auto const& mergedFeatureNode = static_cast<simfil::ModelNode const&>(*mergedFeature);

        auto geometryNode = mergedFeatureNode.get(StringPool::GeometryStr);
        REQUIRE(geometryNode);
        auto geometryArrayNode = geometryNode->get(StringPool::GeometriesStr);
        REQUIRE(geometryArrayNode);
        REQUIRE(geometryArrayNode->size() == 3);

        auto relationsNode = mergedFeatureNode.get(StringPool::RelationsStr);
        REQUIRE(relationsNode);
        REQUIRE(relationsNode->size() == 3);

        auto relationsJson = relationsNode->toJson();
        REQUIRE(relationsJson[0]["name"] == "baseRel");
        REQUIRE(relationsJson[1]["name"] == "overlayRel1");
        REQUIRE(relationsJson[2]["name"] == "overlayRel2");
    }

    SECTION("ModelNode access sees merged attribute layers")
    {
        auto const& mergedFeatureNode = static_cast<simfil::ModelNode const&>(*mergedFeature);

        auto propertiesNode = mergedFeatureNode.get(StringPool::PropertiesStr);
        REQUIRE(propertiesNode);
        auto layersNode = propertiesNode->get(StringPool::LayerStr);
        REQUIRE(layersNode);
        REQUIRE(layersNode->size() == 3);

        auto layersJson = layersNode->toJson();
        REQUIRE(layersJson.contains("baseLayer"));
        REQUIRE(layersJson.contains("overlayLayer1"));
        REQUIRE(layersJson.contains("overlayLayer2"));
    }
}

TEST_CASE("FeatureLayer Overlay AttributeLayerList iteration uses owning model", "[test.featurelayer.overlay]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Way",
                "uniqueIdCompositions": [
                    [
                        {
                            "partId": "wayId",
                            "description": "Globally unique 32b integer.",
                            "datatype": "U32"
                        }
                    ]
                ]
            }
        ]
    })"_json);

    auto strings = std::make_shared<StringPool>("OverlayNode");

    auto makeTile = [&](std::string const& nodeName) {
        return std::make_shared<TileFeatureLayer>(
            TileId::fromWgs84(42., 11., 13),
            nodeName,
            "OverlayMap",
            layerInfo,
            strings);
    };

    auto base = makeTile("OverlayNode");
    auto overlay = makeTile("OverlayNode");

    auto baseFeature = base->newFeature("Way", {{"wayId", 1}});
    auto dummyBaseLayer = baseFeature->attributeLayers()->newLayer("dummyBaseLayer");
    auto dummyBaseAttr = dummyBaseLayer->newAttribute("dummyBaseAttr");
    REQUIRE(dummyBaseAttr->addField("value", "dummy").has_value());
    auto baseLayer = baseFeature->attributeLayers()->newLayer("baseLayer");
    auto baseAttr = baseLayer->newAttribute("baseAttr");
    REQUIRE(baseAttr->addField("value", "base").has_value());

    auto overlayFeature = overlay->newFeature("Way", {{"wayId", 1}});
    auto overlayLayer = overlayFeature->attributeLayers()->newLayer("overlayLayer");
    auto overlayAttr = overlayLayer->newAttribute("overlayAttr");
    REQUIRE(overlayAttr->addField("value", "overlay").has_value());

    base->attachOverlay(overlay);

    auto mergedFeature = base->at(0);
    REQUIRE(mergedFeature);

    auto layersSeen = collectLayerAttributeValues(mergedFeature->attributeLayersOrNull());

    REQUIRE(layersSeen == std::vector<std::tuple<std::string, std::string, std::string>>{
        {"dummyBaseLayer", "dummyBaseAttr", "dummy"},
        {"baseLayer", "baseAttr", "base"},
        {"overlayLayer", "overlayAttr", "overlay"},
    });
}

TEST_CASE("FeatureLayer clone preserves merged staged attribute layers geometry and relations", "[test.featurelayer.overlay]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Way",
                "uniqueIdCompositions": [
                    [
                        {
                            "partId": "wayId",
                            "description": "Globally unique 32b integer.",
                            "datatype": "U32"
                        }
                    ]
                ]
            }
        ]
    })"_json);

    auto strings = std::make_shared<StringPool>("OverlayNode");

    auto makeTile = [&](std::string const& nodeName) {
        return std::make_shared<TileFeatureLayer>(
            TileId::fromWgs84(42., 11., 13),
            nodeName,
            "OverlayMap",
            layerInfo,
            strings);
    };

    auto target = makeTile("TargetNode");
    auto sourceBase = makeTile("OverlayNode");
    auto sourceOverlay = makeTile("OverlayNode");

    auto sourceBaseFeature = sourceBase->newFeature("Way", {{"wayId", 1}});
    auto sourceBaseGeom = sourceBaseFeature->geom()->newGeometry(GeomType::Points, 1);
    sourceBaseGeom->append({10., 10., 0.});
    auto dummyBaseLayer = sourceBaseFeature->attributeLayers()->newLayer("dummyBaseLayer");
    auto dummyBaseAttr = dummyBaseLayer->newAttribute("dummyBaseAttr");
    REQUIRE(dummyBaseAttr->addField("value", "dummy").has_value());
    auto sourceBaseLayer = sourceBaseFeature->attributeLayers()->newLayer("baseLayer");
    auto sourceBaseAttr = sourceBaseLayer->newAttribute("baseAttr");
    REQUIRE(sourceBaseAttr->addField("value", "base").has_value());
    sourceBaseFeature->addRelation("baseRel", sourceBase->newFeatureId("Way", {{"wayId", 100}}));

    auto sourceOverlayFeature = sourceOverlay->newFeature("Way", {{"wayId", 1}});
    auto sourceOverlayGeom = sourceOverlayFeature->geom()->newGeometry(GeomType::Points, 1);
    sourceOverlayGeom->append({20., 20., 0.});
    auto sourceOverlayLayer = sourceOverlayFeature->attributeLayers()->newLayer("overlayLayer");
    auto sourceOverlayAttr = sourceOverlayLayer->newAttribute("overlayAttr");
    REQUIRE(sourceOverlayAttr->addField("value", "overlay").has_value());
    sourceOverlayFeature->addRelation("overlayRel", sourceOverlay->newFeatureId("Way", {{"wayId", 101}}));

    sourceBase->attachOverlay(sourceOverlay);

    auto mergedSourceFeature = sourceBase->at(0);
    REQUIRE(mergedSourceFeature);

    TileFeatureLayer::CloneCache clonedModelNodes;
    target->clone(clonedModelNodes, sourceBase, *mergedSourceFeature, "Way", {{"wayId", 1}});

    auto clonedFeature = target->at(0);
    REQUIRE(clonedFeature);

    auto layersSeen = collectLayerAttributeValues(clonedFeature->attributeLayersOrNull());

    REQUIRE(layersSeen == std::vector<std::tuple<std::string, std::string, std::string>>{
        {"dummyBaseLayer", "dummyBaseAttr", "dummy"},
        {"baseLayer", "baseAttr", "base"},
        {"overlayLayer", "overlayAttr", "overlay"},
    });

    std::vector<glm::dvec3> firstPoints;
    auto clonedGeom = clonedFeature->geomOrNull();
    REQUIRE(clonedGeom);
    REQUIRE(clonedGeom->forEachGeometry([&](model_ptr<Geometry> const& geom) {
        bool gotPoint = false;
        geom->forEachPoint([&](glm::dvec3 const& point) {
            firstPoints.push_back(point);
            gotPoint = true;
            return false;
        });
        REQUIRE(gotPoint);
        return true;
    }));
    REQUIRE(firstPoints.size() == 2);
    REQUIRE(firstPoints[0].x == 10.0);
    REQUIRE(firstPoints[0].y == 10.0);
    REQUIRE(firstPoints[0].z == 0.0);
    REQUIRE(firstPoints[1].x == 20.0);
    REQUIRE(firstPoints[1].y == 20.0);
    REQUIRE(firstPoints[1].z == 0.0);

    std::vector<std::string> relationNames;
    REQUIRE(clonedFeature->forEachRelation([&](model_ptr<Relation> const& relation) {
        relationNames.emplace_back(relation->name());
        return true;
    }));
    REQUIRE(relationNames == std::vector<std::string>{"baseRel", "overlayRel"});
}

TEST_CASE("FeatureLayer Overlay Size Check", "[test.featurelayer.overlay]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Way",
                "uniqueIdCompositions": [
                    [
                        {
                            "partId": "wayId",
                            "description": "Globally unique 32b integer.",
                            "datatype": "U32"
                        }
                    ]
                ]
            }
        ]
    })"_json);

    auto strings = std::make_shared<StringPool>("OverlayNode");
    auto base = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "OverlayNode",
        "OverlayMap",
        layerInfo,
        strings);
    auto overlay = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "OverlayNode",
        "OverlayMap",
        layerInfo,
        strings);

    base->newFeature("Way", {{"wayId", 1}});
    base->newFeature("Way", {{"wayId", 2}});
    overlay->newFeature("Way", {{"wayId", 1}});

    REQUIRE_THROWS(base->attachOverlay(overlay));
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

    SECTION("Neighbor") {
        TileId tile(0, 0, 1);
        REQUIRE(tile.neighbor(1, 0) == TileId(1, 0, 1));
        REQUIRE(tile.neighbor(0, 1) == TileId(0, 1, 1));
        REQUIRE(tile.neighbor(-1, -1) == TileId(3, 0, 1));  // Wrap around

        TileId tile2(3, 1, 1);
        REQUIRE(tile2.neighbor(-1, -1) == TileId(2, 0, 1));
        REQUIRE(tile2.neighbor(1, 1) == TileId(0, 1, 1));  // Wrap around

        REQUIRE_THROWS(tile2.neighbor(2, -2));
        REQUIRE_THROWS(tile2.neighbor(-2, 2));
        REQUIRE_THROWS(tile2.neighbor(0, 3));
        REQUIRE_THROWS(tile2.neighbor(0, -3));
        REQUIRE_THROWS(tile2.neighbor(2, 0));
        REQUIRE_THROWS(tile2.neighbor(-2, 0));
    }
}
