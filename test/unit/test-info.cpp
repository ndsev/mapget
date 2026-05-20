#include <catch2/catch_test_macros.hpp>

#include "mapget/model/featurelayer.h"
#include "mapget/model/info.h"
#include "mapget/model/stream.h"
#include "mapget/log.h"

using namespace mapget;

namespace
{

/** Minimal x-mapget annotated schema that exercises feature, property and attribute containers. */
nlohmann::json schemaAnnotatedLayerInfoJson()
{
    return R"({
        "layerId": "CarrierLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Carrier",
                "uniqueIdCompositions": [
                    [
                        {
                            "partId": "carrierId",
                            "description": "Synthetic carrier id.",
                            "datatype": "U32"
                        }
                    ]
                ]
            }
        ],
        "featureModelSchema": {
            "$schema": "http://json-schema.org/draft-07/schema#",
            "oneOf": [
                {"$ref": "#/$defs/CarrierFeature"}
            ],
            "$defs": {
                "CarrierFeature": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "Feature",
                        "featureType": "Carrier"
                    },
                    "properties": {
                        "typeId": {"const": "Carrier"},
                        "properties": {"$ref": "#/$defs/CarrierProperties"},
                        "relations": {
                            "type": "array",
                            "items": {"$ref": "#/$defs/Relation"}
                        }
                    }
                },
                "CarrierProperties": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "FeatureProperties",
                        "featureType": "Carrier"
                    },
                    "properties": {
                        "displayName": {"type": "string"},
                        "layer": {"$ref": "#/$defs/CarrierLayerMap"}
                    }
                },
                "CarrierLayerMap": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "AttributeLayerMap",
                        "featureType": "Carrier"
                    },
                    "properties": {
                        "limits": {"$ref": "#/$defs/LimitsLayer"}
                    }
                },
                "LimitsLayer": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "AttributeContainer"
                    },
                    "properties": {
                        "speed": {"$ref": "#/$defs/SpeedAttribute"}
                    }
                },
                "SpeedAttribute": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "Attribute",
                        "attributeTypeCode": "speed"
                    },
                    "properties": {
                        "unit": {"type": "string"},
                        "value": {"type": "number"}
                    }
                },
                "Relation": {
                    "type": "object",
                    "properties": {
                        "target": {"type": "string"}
                    }
                }
            }
        }
    })"_json;
}

} // namespace

TEST_CASE("InfoToJson", "[DataSourceInfo]")
{
    mapget::setLogLevel("trace", log());


    // Create a DataSourceInfo object.
    std::unordered_map<std::string, std::shared_ptr<LayerInfo>> layers;
    layers["testLayer"] = std::make_shared<LayerInfo>(LayerInfo{
        "testLayer",
        LayerType::Features,
        std::vector<FeatureTypeInfo>(),
        std::vector<int>{0, 1, 2},
        std::vector<Coverage>{{1, 2, {}}, {3, 3, {}}},
        1,
        std::vector<std::string>{"Complete"},
        0,
        true,
        false,
        Version{1, 0, 0}});

    DataSourceInfo info(DataSourceInfo{
        "testNodeId",
        "testMapId",
        layers,
        5,
        false,
        nlohmann::json::object(),
        TileLayerStream::CurrentProtocolVersion});

    // Serialize it to JSON.
    nlohmann::json j = info.toJson();
    log().trace("Serialized data source info: {}", to_string(j));

    // Deserialize it back into a DataSourceInfo object, then serialize it again.
    auto j2 = DataSourceInfo::fromJson(j).toJson();

    // Check that the two DataSourceInfo objects are equal.
    REQUIRE(j == j2);
}

TEST_CASE("LayerInfo roundtrips featureModelSchema", "[DataSourceInfo]")
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
                            "description": "Test way id.",
                            "datatype": "U32"
                        }
                    ]
                ]
            }
        ],
        "featureModelSchema": {
            "$schema": "http://json-schema.org/draft-07/schema#",
            "type": "object",
            "required": ["type", "typeId"],
            "properties": {
                "type": {"const": "Feature"},
                "typeId": {"const": "Way"}
            },
            "additionalProperties": true
        }
    })"_json);

    auto json = layerInfo->toJson();
    REQUIRE(json.contains("featureModelSchema"));
    REQUIRE(json["featureModelSchema"]["properties"]["typeId"]["const"] == "Way");
    REQUIRE(LayerInfo::fromJson(json)->toJson() == json);
}

TEST_CASE("TileFeatureLayer validates emitted features against LayerInfo schema", "[DataSourceInfo]")
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
                            "description": "Test way id.",
                            "datatype": "U32"
                        }
                    ]
                ]
            }
        ],
        "featureModelSchema": {
            "$schema": "http://json-schema.org/draft-07/schema#",
            "type": "object",
            "required": ["type", "typeId"],
            "properties": {
                "type": {"const": "Feature"},
                "typeId": {"const": "Way"}
            },
            "additionalProperties": true
        }
    })"_json);

    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "SchemaTestNode",
        "SchemaTestMap",
        layerInfo,
        std::make_shared<StringPool>("SchemaTestNode"));
    tile->newFeature("Way", {{"wayId", 1}});

    REQUIRE_NOTHROW(tile->validateSchema());

    layerInfo->featureModelSchema_["properties"]["typeId"]["const"] = "Road";
    REQUIRE_THROWS(tile->validateSchema());
}

TEST_CASE("LayerInfo builds SchemaRegistry from x-mapget annotations", "[DataSourceInfo]")
{
    auto layerInfo = LayerInfo::fromJson(schemaAnnotatedLayerInfoJson());
    auto registry = layerInfo->schemaRegistry();

    REQUIRE(registry);

    auto const* carrierSchema = registry->getSchema("Carrier");
    REQUIRE(carrierSchema != nullptr);
    REQUIRE(carrierSchema->id_ != simfil::NoSchemaId);
    REQUIRE(registry->schemaId("Feature:Carrier") == carrierSchema->id_);

    auto const propertiesId = registry->featurePropertiesSchema("Carrier");
    auto const layerMapId = registry->attributeLayerMapSchema("Carrier");
    REQUIRE(propertiesId != simfil::NoSchemaId);
    REQUIRE(layerMapId != simfil::NoSchemaId);

    auto const limitsId = registry->childSchema(
        layerMapId,
        "limits",
        simfil::Schema::Kind::Object);
    auto const speedId = registry->childSchema(
        limitsId,
        "speed",
        simfil::Schema::Kind::Object);
    REQUIRE(limitsId != simfil::NoSchemaId);
    REQUIRE(speedId != simfil::NoSchemaId);

    REQUIRE(registry->canHaveField(carrierSchema->id_, "properties"));
    REQUIRE(registry->canHaveField(carrierSchema->id_, "value"));
    REQUIRE_FALSE(registry->canHaveField(carrierSchema->id_, "notDeclaredBySchema"));
}

TEST_CASE("SchemaRegistry does not mutate datasource StringPool", "[DataSourceInfo]")
{
    auto layerInfo = LayerInfo::fromJson(schemaAnnotatedLayerInfoJson());
    auto strings = std::make_shared<StringPool>("SchemaRegistryReadonlyNode");
    auto const highestBefore = strings->highest();
    auto const sizeBefore = strings->size();

    auto registry = layerInfo->schemaRegistry();
    REQUIRE(registry);
    REQUIRE(strings->highest() == highestBefore);
    REQUIRE(strings->size() == sizeBefore);

    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "SchemaRegistryReadonlyNode",
        "SchemaRegistryReadonlyMap",
        layerInfo,
        strings);
    REQUIRE(tile->schemaRegistry());
    REQUIRE(strings->highest() == highestBefore);
    REQUIRE(strings->size() == sizeBefore);
}

TEST_CASE("TileFeatureLayer exposes SchemaIds on feature-model nodes", "[DataSourceInfo]")
{
    auto layerInfo = LayerInfo::fromJson(schemaAnnotatedLayerInfoJson());
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "SchemaNode",
        "SchemaMap",
        layerInfo,
        std::make_shared<StringPool>("SchemaNode"));
    auto registry = tile->schemaRegistry();
    REQUIRE(registry);

    auto feature = tile->newFeature("Carrier", {{"carrierId", 7}});
    auto attrs = feature->attributes();
    REQUIRE(attrs->addField("displayName", "Carrier Seven").has_value());
    auto layer = feature->attributeLayers()->newLayer("limits");
    auto speed = layer->newAttribute("speed");
    REQUIRE(speed->addField("value", double{80.0}).has_value());
    REQUIRE(speed->addField("unit", "km/h").has_value());

    simfil::ModelNode::Ptr featureNode = feature;
    REQUIRE(featureNode->schema() == tile->getSchema("Carrier")->id_);
    REQUIRE(attrs->schema() == registry->featurePropertiesSchema("Carrier"));

    auto const layerMapId = registry->attributeLayerMapSchema("Carrier");
    auto const limitsId = registry->childSchema(
        layerMapId,
        "limits",
        simfil::Schema::Kind::Object);
    auto const speedId = registry->childSchema(
        limitsId,
        "speed",
        simfil::Schema::Kind::Object);
    REQUIRE(feature->attributeLayers()->schema() == layerMapId);
    REQUIRE(layer->schema() == limitsId);

    simfil::ModelNode::Ptr speedNode = speed;
    REQUIRE(speedNode->schema() == speedId);

    auto propertiesNode = featureNode->get(StringPool::PropertiesStr);
    REQUIRE(propertiesNode);
    REQUIRE(propertiesNode->schema() == registry->featurePropertiesSchema("Carrier"));

    auto layerNode = propertiesNode->get(StringPool::LayerStr);
    REQUIRE(layerNode);
    REQUIRE(layerNode->schema() == layerMapId);
}

TEST_CASE("InfoFromJson", "[DataSourceInfo]")
{
    // Create a JSON object with some mandatory fields missing.
    nlohmann::json j = R"({
        "nodeId": "testNodeId",
        "protocolVersion": {
            "major": 1,
            "minor": 0,
            "patch": 0
        }
    })"_json;

    // Attempting to deserialize should throw an exception because "mapId" is missing.
    REQUIRE_THROWS_AS(DataSourceInfo::fromJson(j), std::runtime_error);
}
