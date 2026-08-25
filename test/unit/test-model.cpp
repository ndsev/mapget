#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "mapget/model/featurelayer.h"
#include "mapget/model/sourcedata.h"
#include "mapget/model/sourcedatalayer.h"
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

TEST_CASE(
    "SourceData address scopes roundtrip through TileLayerStream",
    "[test.sourcedatalayer][stream]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "SourceData-Test",
        "type": "SourceData"
    })"_json);
    auto strings = std::make_shared<StringPool>("SourceDataScopeNode");
    auto tile = std::make_shared<TileSourceDataLayer>(
        TileId{},
        "SourceDataScopeNode",
        "SourceDataScopeMap",
        layerInfo,
        strings);

    auto structural = tile->newCompound(0);
    structural->setSourceDataAddress({0, 128});
    auto payload = tile->newCompound(0);
    payload->setSourceDataAddress({32, 64});
    payload->setSourceDataAddressScope();
    tile->addRoot(structural);
    tile->addRoot(payload);

    REQUIRE_FALSE(structural->isSourceDataAddressScope());
    REQUIRE(payload->isSourceDataAddressScope());

    std::string streamBytes;
    TileLayerStream::StringPoolOffsetMap offsets;
    TileLayerStream::Writer writer(
        [&](std::string bytes, TileLayerStream::MessageType) {
            streamBytes.append(bytes);
        },
        offsets);
    writer.write(tile);

    TileSourceDataLayer::Ptr parsed;
    TileLayerStream::Reader reader(
        [&](std::string_view const&, std::string_view const&) {
            return layerInfo;
        },
        [&](TileLayer::Ptr parsedLayer) {
            parsed = std::dynamic_pointer_cast<TileSourceDataLayer>(parsedLayer);
        });
    reader.read(streamBytes);

    REQUIRE(parsed);
    auto parsedStructural = parsed->resolve<SourceDataCompoundNode>(
        simfil::ModelNodeAddress{TileSourceDataLayer::Compound, 0});
    auto parsedPayload = parsed->resolve<SourceDataCompoundNode>(
        simfil::ModelNodeAddress{TileSourceDataLayer::Compound, 1});
    REQUIRE_FALSE(parsedStructural->isSourceDataAddressScope());
    REQUIRE(parsedPayload->isSourceDataAddressScope());
    REQUIRE(parsedPayload->sourceDataAddress().u64() == SourceDataAddress{32, 64}.u64());
}

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
            R"({"coordinates":[[[[3,0,0],[4,0,0],[4,1,0],[3,0,0]]],[[[4,1,0],[3,1,0],[3,0,0],[4,1,0]]]],"type":"MultiPolygon"})"  // Mesh
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
        REQUIRE(feature1->evaluate("attributes.main_ingredient").value().toString() == "Pepper");
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
        tile->setGlbAttachmentName("city.glb");

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
            [&](auto&& stringPoolId){
                REQUIRE(stringPoolId == "TastyTomatoSaladNode");
                return strings;
            }
        );

        REQUIRE(deserializedTile->tileId() == tile->tileId());
        REQUIRE(deserializedTile->stringPoolId() == tile->stringPoolId());
        REQUIRE(deserializedTile->mapId() == tile->mapId());
        REQUIRE(deserializedTile->layerInfo() == tile->layerInfo());
        REQUIRE(deserializedTile->error() == tile->error());
        REQUIRE(deserializedTile->errorCode() == tile->errorCode());
        REQUIRE(deserializedTile->timestamp().time_since_epoch() == tile->timestamp().time_since_epoch());
        REQUIRE(deserializedTile->ttl() == tile->ttl());
        REQUIRE(deserializedTile->mapVersion() == tile->mapVersion());
        REQUIRE(deserializedTile->info() == tile->info());
        REQUIRE(
            deserializedTile->glbAttachmentName() ==
            std::optional<std::string>{"city.glb"});

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
        tile->setGlbAttachmentName("city.glb");

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
        REQUIRE(readTiles[0]->glbAttachmentName() == "city.glb");
        REQUIRE(readTiles[1]->glbAttachmentName() == "city.glb");
        REQUIRE(readTiles[2]->glbAttachmentName() == "city.glb");
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
        tile->setGlbAttachmentName("city.glb");

        auto json = tile->toJson();

        // Verify required fields
        REQUIRE(json["type"] == "FeatureCollection");
        REQUIRE(json["mapgetTileId"].is_number_integer());
        REQUIRE(json["mapgetTileId"].get<int32_t>() == tile->tileId().value());
        REQUIRE(json["mapId"] == "Tropico");
        REQUIRE(json["mapgetLayerId"] == "WayLayer");

        // Verify timestamp stays in the binary microsecond representation.
        REQUIRE(json["timestamp"].is_number_integer());
        REQUIRE(json["timestamp"].get<int64_t>() ==
            std::chrono::duration_cast<std::chrono::microseconds>(
                tile->timestamp().time_since_epoch()).count());

        // Verify TTL
        REQUIRE(json["ttl"] == 3600000);

        REQUIRE(json["glbAttachmentName"] == "city.glb");

        // Verify features array exists
        REQUIRE(json["features"].is_array());
        REQUIRE(json["features"].size() == 2);  // feature0 and feature1

        // Verify no error object when no error is set
        REQUIRE(!json.contains("error"));
    }

    SECTION("GLB attachment name can be replaced")
    {
        tile->setGlbAttachmentName("city.glb");
        REQUIRE(tile->glbAttachmentName() == "city.glb");

        tile->setGlbAttachmentName("city-updated.glb");
        REQUIRE(
            tile->glbAttachmentName() ==
            "city-updated.glb");

        tile->setGlbAttachmentName(std::nullopt);
        REQUIRE_FALSE(tile->glbAttachmentName());
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
            [&](auto&& stringPoolId){
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

}

TEST_CASE("AttributeLayer duplicate names retain instance identity in GeoJSON", "[test.featurelayer]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "Road",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Road",
                "uniqueIdCompositions": [
                    [
                        {
                            "partId": "roadId",
                            "description": "Synthetic road id.",
                            "datatype": "U32"
                        }
                    ]
                ]
            }
        ]
    })"_json);
    auto strings = std::make_shared<StringPool>("test-node");
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromValue(545555028),
        "test-node",
        "TestMap",
        layerInfo,
        strings);

    auto road = tile->newFeature("Road", {{"roadId", 1}});
    auto firstRulesLayer = tile->newAttributeLayer();
    firstRulesLayer->setId(3);
    auto firstSpeedA = firstRulesLayer->newAttribute("SPEED_LIMIT");
    REQUIRE(firstSpeedA->addField("value", int64_t{50}).has_value());
    auto firstSpeedB = firstRulesLayer->newAttribute("SPEED_LIMIT");
    REQUIRE(firstSpeedB->addField("value", int64_t{80}).has_value());

    auto secondRulesLayer = tile->newAttributeLayer();
    secondRulesLayer->setId(4);
    auto secondSpeedA = secondRulesLayer->newAttribute("SPEED_LIMIT");
    REQUIRE(secondSpeedA->addField("value", int64_t{30}).has_value());
    auto secondSpeedB = secondRulesLayer->newAttribute("SPEED_LIMIT");
    REQUIRE(secondSpeedB->addField("value", int64_t{60}).has_value());

    auto layers = road->attributeLayers();
    layers->addLayer("RoadRulesLayer", firstRulesLayer);
    layers->addLayer("RoadRulesLayer", secondRulesLayer);

    auto json = road->toJson();
    auto const& layerMap = json.at("properties").at("layer");
    REQUIRE(layerMap.at("_multimap") == true);
    REQUIRE(layerMap.at("RoadRulesLayer").is_array());
    REQUIRE(layerMap.at("RoadRulesLayer").size() == 2);

    auto const& firstJson = layerMap.at("RoadRulesLayer")[0];
    REQUIRE(firstJson.at("id") == 3);
    REQUIRE(firstJson.at("_multimap") == true);
    REQUIRE(firstJson.at("SPEED_LIMIT").is_array());
    REQUIRE(firstJson.at("SPEED_LIMIT")[0].at("value") == 50);
    REQUIRE(firstJson.at("SPEED_LIMIT")[1].at("value") == 80);

    auto const& secondJson = layerMap.at("RoadRulesLayer")[1];
    REQUIRE(secondJson.at("id") == 4);
    REQUIRE(secondJson.at("_multimap") == true);
    REQUIRE(secondJson.at("SPEED_LIMIT").is_array());
    REQUIRE(secondJson.at("SPEED_LIMIT")[0].at("value") == 30);
    REQUIRE(secondJson.at("SPEED_LIMIT")[1].at("value") == 60);

    auto collected = collectLayerAttributeValues(layers);
    REQUIRE(collected.size() == 4);

    auto imported = std::make_shared<TileFeatureLayer>(
        TileId::fromValue(545555028),
        "test-node-import",
        "TestMap",
        layerInfo,
        std::make_shared<StringPool>("test-node-import"));
    REQUIRE_NOTHROW(imported->fromJson(tile->toJson()));
    REQUIRE(imported->toJson() == tile->toJson());
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

TEST_CASE("FeatureLayer clone preserves source-data references",
          "[test.featurelayer][test.featurelayer.sourcedatarefs]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [{
            "name": "Way",
            "uniqueIdCompositions": [[
                {"partId": "wayId", "datatype": "U32"}
            ]]
        }]
    })"_json);
    auto sourceStrings = std::make_shared<StringPool>("CloneSourceNode");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "CloneSourceNode",
        "CloneMap",
        layerInfo,
        sourceStrings);
    auto makeReference = [&](std::string_view qualifier) {
        QualifiedSourceDataReference ref{
            .address_ = SourceDataAddress::fromBitPosition(8, 16),
            .layerId_ = sourceStrings->emplace("SourceLayer").value(),
            .qualifier_ = sourceStrings->emplace(qualifier).value(),
        };
        return source->newSourceDataReferenceCollection({&ref, 1});
    };

    auto feature = source->newFeature("Way", {{"wayId", 42}});
    feature->setSourceDataReferences(makeReference("feature"));
    auto geometry = feature->geom()->newGeometry(GeomType::Line, 2, true);
    geometry->append({42., 11., 0.});
    geometry->append({42.1, 11.1, 0.});
    geometry->setName("centerline");
    geometry->setSourceDataReferences(makeReference("geometry"));
    auto sequence = source->newAttrPointSequence(feature, geometry);
    sequence->appendAttrPoint(
        1,
        {42.05, 11.05, 0.},
        makeReference("attribute-point"));
    sequence->setSourceDataReferences(makeReference("attribute-point-list"));
    auto attribute = feature->attributeLayers()
        ->newLayer("Rules")
        ->newAttribute("Access");
    attribute->validity()->newAttrPointIndexRange(sequence, 0, 2);
    attribute->setSourceDataReferences(makeReference("attribute"));
    auto relation = source->newRelation(
        "ConnectedTo",
        source->newFeatureId("Way", {{"wayId", 43}}));
    relation->setSourceDataReferences(makeReference("relation"));
    feature->addRelation(relation);

    auto targetStrings = std::make_shared<StringPool>("CloneTargetNode");
    targetStrings->emplace("ForceDifferentStringIds").value();
    auto target = std::make_shared<TileFeatureLayer>(
        source->tileId(),
        "CloneTargetNode",
        "CloneMap",
        layerInfo,
        targetStrings);
    TileFeatureLayer::CloneCache cache;
    target->clone(
        cache,
        source,
        *feature,
        feature->id()->typeId(),
        feature->id()->keyValuePairs());

    REQUIRE(target->toJson().at("features") == source->toJson().at("features"));
    REQUIRE(target->toJson().at("attrPointSequences")
        == source->toJson().at("attrPointSequences"));

    // One add-on feature may contribute to multiple destination features.
    // Reuse the same cache to exercise the target-scoped clone identity.
    KeyValuePairs firstTargetId{{"wayId", int64_t{100}}};
    KeyValuePairs secondTargetId{{"wayId", int64_t{101}}};
    target->clone(
        cache,
        source,
        *feature,
        "Way",
        castToKeyValueView(firstTargetId));
    target->clone(
        cache,
        source,
        *feature,
        "Way",
        castToKeyValueView(secondTargetId));

    REQUIRE(target->numAttrPointSequences() == 3);
    REQUIRE(target->attrPointSequenceAt(1)->featureId()->toString() == "Way.100");
    REQUIRE(target->attrPointSequenceAt(2)->featureId()->toString() == "Way.101");
    REQUIRE(target->attrPointSequenceAt(1)->geometry()->addr().value_ !=
        target->attrPointSequenceAt(2)->geometry()->addr().value_);
    REQUIRE(target->attrPointSequenceAt(1)->geometryIndex() == 0);
    REQUIRE(target->attrPointSequenceAt(2)->geometryIndex() == 0);
    REQUIRE(target->attrPointSequenceAt(1)->attrPoints()->attrPointAt(0)
        ->sourceDataReferences());
    REQUIRE(target->attrPointSequenceAt(2)->sourceDataReferences());

    auto const remappedJson = target->toJson();
    auto const& firstValidity = remappedJson["features"][1]["properties"]
        ["layer"]["Rules"]["Access"]["validity"];
    auto const& secondValidity = remappedJson["features"][2]["properties"]
        ["layer"]["Rules"]["Access"]["validity"];
    REQUIRE(firstValidity["attrPointIndexRange"]["sequence"]
        ["$mapgetAttrPointSequence"] == 1);
    REQUIRE(secondValidity["attrPointIndexRange"]["sequence"]
        ["$mapgetAttrPointSequence"] == 2);
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

TEST_CASE("Simple validities upgrade only their owning collection slot", "[test.featurelayer.validity]")
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

    auto strings = std::make_shared<StringPool>("SimpleValidityUpgradeIsolation");
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "SimpleValidityUpgradeIsolation",
        "Tropico",
        layerInfo,
        strings);
    auto feature = tile->newFeature("Way", {{"wayId", 1}});

    auto attr = feature->attributeLayers()->newLayer("limits")->newAttribute("speed");
    auto attrValidity = attr->validity()->newDirection(Validity::Direction::Positive);

    auto relation = tile->newRelation(
        "connectedTo",
        tile->newFeatureId("Way", {{"wayId", 2}}, "ValidationMap"));
    auto relationValidity = relation->targetValidity()->newDirection(Validity::Direction::Positive);
    relationValidity->setGeometryName("ADAS");
    feature->addRelation(relation);

    auto materializedAttr = tile->resolve<Attribute>(attr->addr());
    REQUIRE(materializedAttr);
    auto const& attrNode = static_cast<simfil::ModelNode const&>(*materializedAttr);
    auto attrValidityNode = attrNode.get(StringPool::ValidityStr);
    REQUIRE(attrValidityNode);

    auto materializedRelation = tile->resolve<Relation>(relation->addr());
    REQUIRE(materializedRelation);
    auto const& relationNode = static_cast<simfil::ModelNode const&>(*materializedRelation);
    auto targetValidityNode = relationNode.get(StringPool::TargetValidityStr);
    REQUIRE(targetValidityNode);

    REQUIRE(attrValidityNode->toJson() == nlohmann::json{{"direction", "POSITIVE"}});
    REQUIRE(targetValidityNode->toJson() == nlohmann::json{
        {"direction", "POSITIVE"},
        {"geometryName", "ADAS"},
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
    auto transition = tile->resolve<Validity>(*attr->validity()->at(0));
    REQUIRE(transition);
    REQUIRE(transition->transitionFromFeatureId()->toString() == "Way.1");
    REQUIRE(transition->transitionToFeatureId()->toString() == "Way.2");
    REQUIRE(transition->transitionFromFeature() == fromFeature);
    REQUIRE(transition->transitionToFeature() == toFeature);
    REQUIRE(attrValidityNode->toJson() == nlohmann::json{
        {"from", "Way.1"},
        {"fromConnectedEnd", "END"},
        {"to", "Way.2"},
        {"toConnectedEnd", "START"},
        {"transitionNumber", 7},
    });
}

TEST_CASE("Semantic feature transitions preserve cross-tile endpoint IDs", "[test.featurelayer.validity]")
{
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Way",
                "uniqueIdCompositions": [
                    [
                        {"partId": "wayId", "description": "way id", "datatype": "U32"}
                    ],
                    [
                        {"partId": "tileId", "description": "source tile", "datatype": "I64"},
                        {"partId": "wayIndex", "description": "source index", "datatype": "U32"}
                    ]
                ]
            }
        ]
    })"_json);

    auto strings = std::make_shared<StringPool>("CrossTileTransitionNode");
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "CrossTileTransitionNode",
        "Tropico",
        layerInfo,
        strings);

    auto host = tile->newFeature("Way", {{"wayId", 1}});
    auto fromFeatureId = tile->newFeatureId(
        "Way", {{"tileId", int64_t{1001}}, {"wayIndex", 7}});
    auto toFeatureId = tile->newFeatureId(
        "Way", {{"tileId", int64_t{1002}}, {"wayIndex", 9}});
    auto transition = host->attributeLayers()
                          ->newLayer("rules")
                          ->newAttribute("turn")
                          ->validity()
                          ->newFeatureTransition(
                              fromFeatureId,
                              Validity::End,
                              toFeatureId,
                              Validity::Start,
                              11);

    REQUIRE(transition->transitionFromFeatureId()->toString() == "Way.1001.7");
    REQUIRE(transition->transitionToFeatureId()->toString() == "Way.1002.9");
    REQUIRE_FALSE(transition->transitionFromFeature());
    REQUIRE_FALSE(transition->transitionToFeature());
    auto materializedTransition = tile->resolve<Validity>(transition->addr());
    REQUIRE(materializedTransition->toJson() == nlohmann::json{
        {"from", "Way.1001.7"},
        {"fromConnectedEnd", "END"},
        {"to", "Way.1002.9"},
        {"toConnectedEnd", "START"},
        {"transitionNumber", 11},
    });

    std::string error;
    REQUIRE(transition->computeGeometry({}, &error).points_.empty());
    REQUIRE(error == "Transition source geometry is unavailable for feature Way.1001.7.");
}

TEST_CASE("Validity GeoJSON exposes semantic geometry names", "[test.featurelayer.validity]")
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

    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "StageValidityNode",
        "Tropico",
        layerInfo,
        std::make_shared<StringPool>("StageValidityNode"));

    auto unnamedValidity = tile->newValidity();
    unnamedValidity->setDirection(Validity::Positive);
    auto resolvedUnnamedValidity = tile->resolve<Validity>(unnamedValidity->addr());
    REQUIRE(resolvedUnnamedValidity);
    REQUIRE(resolvedUnnamedValidity->toJson() == nlohmann::json{{"direction", "POSITIVE"}});

    auto adasValidity = tile->newValidity();
    adasValidity->setGeometryName("ADAS");
    adasValidity->setDirection(Validity::Positive);
    auto resolvedAdasValidity = tile->resolve<Validity>(adasValidity->addr());
    REQUIRE(resolvedAdasValidity);
    REQUIRE(resolvedAdasValidity->toJson() == nlohmann::json{
        {"direction", "POSITIVE"},
        {"geometryName", "ADAS"},
    });
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
        REQUIRE(tile.x() == 0);
        REQUIRE(tile.y() == 0);
        REQUIRE(tile.level() == 0);
    }

    SECTION("fromWgs84: positive longitude, positive latitude") {
        TileId tile = TileId::fromWgs84(90, 45, 1);
        REQUIRE(tile.x() == 1);
        REQUIRE(tile.y() == 0);
        REQUIRE(tile.level() == 1);
    }

    SECTION("fromWgs84: negative longitude, positive latitude") {
        TileId tile = TileId::fromWgs84(-90, 45, 1);
        REQUIRE(tile.x() == 3);
        REQUIRE(tile.y() == 0);
        REQUIRE(tile.level() == 1);
    }

    SECTION("fromWgs84: positive longitude, negative latitude") {
        TileId tile = TileId::fromWgs84(90, -45, 1);
        REQUIRE(tile.x() == 1);
        REQUIRE(tile.y() == 1);
        REQUIRE(tile.level() == 1);
    }

    SECTION("fromWgs84: negative longitude, negative latitude") {
        TileId tile = TileId::fromWgs84(-90, -45, 1);
        REQUIRE(tile.x() == 3);
        REQUIRE(tile.y() == 1);
        REQUIRE(tile.level() == 1);
    }

    SECTION("Tile center/SW/NE/size calculation") {
        TileId tile = TileId::fromTileXY(0, 0, 1);
        REQUIRE_EQUAL(Point(tile.centerWgs84()), {45, 45});
        REQUIRE_EQUAL(Point(tile.southWestWgs84()), {0, 0});
        REQUIRE_EQUAL(Point(tile.northEastWgs84()), {90, 90});
        REQUIRE_EQUAL(Point(tile.wgs84Size()), {90, 90});
    }

    SECTION("Neighbor") {
        TileId tile = TileId::fromTileXY(0, 0, 1);
        REQUIRE(tile.neighbour(1, 0) == TileId::fromTileXY(1, 0, 1));
        REQUIRE(tile.neighbour(0, 1) == TileId::fromTileXY(0, 1, 1));
        REQUIRE(tile.neighbour(-1, -1) == TileId::fromTileXY(3, 1, 1));  // Wrap around

        TileId tile2 = TileId::fromTileXY(3, 1, 1);
        REQUIRE(tile2.neighbour(-1, -1) == TileId::fromTileXY(2, 0, 1));
        REQUIRE(tile2.neighbour(1, 1) == TileId::fromTileXY(0, 0, 1));  // Wrap around
        REQUIRE(tile2.neighbour(4, 2) == tile2);
    }

    SECTION("Legacy mapget tile-id migration helpers") {
        auto const legacy = (int64_t{1} << 32) | int64_t{1};
        REQUIRE(isLegacyTileId(legacy));
        REQUIRE(legacyTileIdToPacked(legacy) == TileId::fromTileXY(3, 0, 1));

        // This fixed legacy tile covers Germany and exercises both origin
        // changes rather than merely producing a structurally valid tile.
        constexpr int64_t legacyGermanyTile = 0x21fa0777000d;
        REQUIRE(legacyTileIdToPacked(legacyGermanyTile) ==
            TileId::fromTileXY(0x01fa, 0x0888, 13));
        REQUIRE(legacyTileIdToPacked(legacyGermanyTile) ==
            TileId::fromWgs84(11.13, 48.0, 13));
        REQUIRE_FALSE(isLegacyTileId(-1));
        REQUIRE_FALSE(isLegacyTileId((int64_t{1} << 32) | (int64_t{2} << 16) | int64_t{1}));
        REQUIRE_THROWS(legacyTileIdToPacked(-1));
    }
}
