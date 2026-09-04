#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include "mapget/model/featurelayer.h"
#include "mapget/model/info.h"
#include "mapget/model/simfilutil.h"
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
                        "limits": {"$ref": "#/$defs/LimitsLayer"},
                        "advisoryLimits": {"$ref": "#/$defs/LimitsLayer"}
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
                        "attributeTypeCode": "speed",
                        "attributeType": "synthetic.SpeedAttributeType"
                    },
                    "properties": {
                        "unit": {
                            "type": "string",
                            "enum": ["km/h", "mph"],
                            "x-mapget": {
                                "zserioType": "synthetic.SpeedUnit"
                            }
                        },
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

bool hasCompletion(
    std::vector<simfil::CompletionCandidate> const& completions,
    std::string_view text,
    simfil::CompletionCandidate::Type type)
{
    return std::ranges::any_of(completions, [&](auto const& candidate) {
        return candidate.text == text && candidate.type == type;
    });
}

std::string namedSchemaPathString(LayerSchema::NamedSchemaPath const& path)
{
    std::string result;
    for (auto const& segment : path) {
        if (!result.empty()) {
            result += ".";
        }
        result += segment.kind_ == simfil::SchemaPathSegment::Kind::ArrayElement
            ? "[]"
            : segment.field_;
    }
    return result;
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
        std::vector<Coverage>{
            {TileId::fromTileXY(0, 0, 0), TileId::fromTileXY(1, 0, 0), {}},
            {TileId::fromTileXY(0, 0, 1), TileId::fromTileXY(0, 0, 1), {}}},
        true,
        false,
        Version{1, 0, 0}});

    DataSourceInfo info(DataSourceInfo{
        "testStringPoolId",
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

    SECTION("default datasource protocol is serialized as current stream protocol")
    {
        info.protocolVersion_ = Version{};
        REQUIRE(info.toJson().at("protocolVersion") == TileLayerStream::CurrentProtocolVersion.toJson());
    }
}

TEST_CASE("MapTileKey percent-escapes map and layer identifier components", "[DataSourceInfo]")
{
    MapTileKey key(
        LayerType::Features,
        "Map.A:B/C,D~%",
        "Layer:X/Y,Z~%",
        TileId::fromTileXY(1, 0, 1));

    auto const encoded = key.toString();
    REQUIRE(encoded == "Features:Map.A%3AB%2FC%2CD%7E%25:Layer%3AX%2FY%2CZ%7E%25:131073");

    auto const parsed = MapTileKey(encoded);
    REQUIRE(parsed.layer_ == key.layer_);
    REQUIRE(parsed.mapId_ == key.mapId_);
    REQUIRE(parsed.layerId_ == key.layerId_);
    REQUIRE(parsed.tileId_ == key.tileId_);
    REQUIRE_THROWS(MapTileKey(encoded + ":2"));
}

TEST_CASE("MapTileKey accepts removed mapget tile-id layout", "[DataSourceInfo]")
{
    auto const legacyTileId = (int64_t{1} << 32) | int64_t{1};
    auto const parsed = MapTileKey("Features:Map:Layer:" + std::to_string(legacyTileId));
    REQUIRE(parsed.tileId_ == TileId::fromTileXY(3, 0, 1));
}

TEST_CASE("MapTileKey accepts removed hexadecimal mapget tile-id layout", "[DataSourceInfo]")
{
    auto const parsed = MapTileKey("Features:Map:Layer:21fa0777000d");
    REQUIRE(parsed.tileId_ == TileId::fromTileXY(0x01fa, 0x0888, 13));
}

TEST_CASE("MapTileKey keeps SourceData tile zero as metadata sentinel", "[DataSourceInfo]")
{
    auto const parsed = MapTileKey("SourceData:Map:Layer:0");
    REQUIRE(parsed.tileId_.value() == 0);
}

TEST_CASE("DataSourceInfo validates reserved characters in raw metadata identifiers", "[DataSourceInfo]")
{
    auto valid = R"({
        "stringPoolId": "ReservedNamesNode",
        "mapId": "ValidMap",
        "layers": {
            "ValidLayer": {
                "layerId": "ValidLayer",
                "type": "Features",
                "featureTypes": [
                    {
                        "name": "Road",
                        "uniqueIdCompositions": [
                            [
                                {"partId": "roadId", "datatype": "U32"}
                            ]
                        ]
                    }
                ]
            }
        }
    })"_json;

    REQUIRE_NOTHROW(DataSourceInfo::fromJson(valid));

    SECTION("map ids may use slashes as UI grouping separators")
    {
        auto grouped = valid;
        grouped["mapId"] = "Group/Subgroup/ValidMap";
        REQUIRE_NOTHROW(DataSourceInfo::fromJson(grouped));
    }

    SECTION("map ids still reject non-path protocol delimiters")
    {
        auto invalid = valid;
        invalid["mapId"] = "Invalid:Map";
        REQUIRE_THROWS(DataSourceInfo::fromJson(invalid));
    }

    SECTION("layer ids are raw datasource identifiers")
    {
        auto invalid = valid;
        invalid["layers"] = nlohmann::json::object({
            {"Invalid/Layer", valid["layers"]["ValidLayer"]}
        });
        REQUIRE_THROWS(DataSourceInfo::fromJson(invalid));
    }

    SECTION("feature type names delimit feature-id strings")
    {
        auto invalid = valid;
        invalid["layers"]["ValidLayer"]["featureTypes"][0]["name"] = "Road.Type";
        REQUIRE_THROWS(DataSourceInfo::fromJson(invalid));
    }

    SECTION("id-part labels are raw field-like identifiers")
    {
        auto invalid = valid;
        invalid["layers"]["ValidLayer"]["featureTypes"][0]["uniqueIdCompositions"][0][0]["partId"] = "road/id";
        REQUIRE_THROWS(DataSourceInfo::fromJson(invalid));
    }
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

    auto invalidSchema = layerInfo->featureModelSchema_->toJsonSchema();
    invalidSchema["properties"]["typeId"]["const"] = "Road";
    layerInfo->featureModelSchema_ = LayerSchema::fromJsonSchema(invalidSchema);
    REQUIRE_THROWS(tile->validateSchema());
}

TEST_CASE("LayerInfo builds LayerSchema from x-mapget annotations", "[DataSourceInfo]")
{
    auto layerInfo = LayerInfo::fromJson(schemaAnnotatedLayerInfoJson());
    auto registry = layerInfo->layerSchema();

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

    auto const typeIdId = registry->childSchema(
        carrierSchema->id_,
        "typeId",
        simfil::Schema::Kind::Value);
    auto const unitId = registry->childSchema(
        speedId,
        "unit",
        simfil::Schema::Kind::Value);
    REQUIRE(typeIdId != simfil::NoSchemaId);
    REQUIRE(unitId != simfil::NoSchemaId);
    REQUIRE(registry->kind(typeIdId) == simfil::Schema::Kind::Value);
    REQUIRE(registry->kind(unitId) == simfil::Schema::Kind::Value);
    REQUIRE(registry->canHaveEnumSymbol(carrierSchema->id_, "Carrier"));
    REQUIRE(registry->canHaveEnumSymbol(carrierSchema->id_, "km/h"));
    REQUIRE(registry->canHaveEnumSymbol(unitId, "mph"));
    REQUIRE_FALSE(registry->canHaveEnumSymbol(carrierSchema->id_, "notAnEnum"));
    REQUIRE(
        registry->constantTypeNames(carrierSchema->id_, "km/h") ==
        std::vector<std::string>{"synthetic.SpeedUnit"});
    REQUIRE(
        registry->constantTypeNames(carrierSchema->id_, "speed") ==
        std::vector<std::string>{"synthetic.SpeedAttributeType"});

    auto strings = std::make_shared<StringPool>("LayerSchemaEnums");
    auto carrierSymbol = strings->emplace("Carrier").value();
    auto speedUnitSymbol = strings->emplace("km/h").value();
    auto missingSymbol = strings->emplace("missing").value();
    simfil::Environment env(strings);
    installLayerSchema(env, registry, strings);
    auto schema = env.querySchema(carrierSchema->id_);
    REQUIRE(schema != nullptr);
    REQUIRE(schema->canHaveEnumSymbol(carrierSymbol));
    REQUIRE(schema->canHaveEnumSymbol(speedUnitSymbol));
    REQUIRE_FALSE(schema->canHaveEnumSymbol(missingSymbol));
}

TEST_CASE("LayerSchema does not mutate datasource StringPool", "[DataSourceInfo]")
{
    auto layerInfo = LayerInfo::fromJson(schemaAnnotatedLayerInfoJson());
    auto strings = std::make_shared<StringPool>("LayerSchemaReadonlyNode");
    auto const highestBefore = strings->highest();
    auto const sizeBefore = strings->size();

    auto registry = layerInfo->layerSchema();
    REQUIRE(registry);
    REQUIRE(strings->highest() == highestBefore);
    REQUIRE(strings->size() == sizeBefore);

    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "LayerSchemaReadonlyNode",
        "LayerSchemaReadonlyMap",
        layerInfo,
        strings);
    REQUIRE(tile->layerSchema());
    REQUIRE(strings->highest() == highestBefore);
    REQUIRE(strings->size() == sizeBefore);
}

TEST_CASE("LayerSchema direct construction supports detached snapshots and escaped field paths", "[DataSourceInfo]")
{
    auto schema = std::make_shared<LayerSchema>();
    auto const featureSchema = schema->addSchema(
        simfil::Schema::Kind::Object,
        LayerSchema::featureKey("Road"),
        "Feature");
    auto const propertiesSchema = schema->addSchema(
        simfil::Schema::Kind::Object,
        LayerSchema::featurePropertiesKey("Road"),
        "FeatureProperties");
    auto const layerMapSchema = schema->addSchema(
        simfil::Schema::Kind::Object,
        LayerSchema::attributeLayerMapKey("Road"),
        "AttributeLayerMap");
    auto const rulesSchema = schema->addSchema(
        simfil::Schema::Kind::Object,
        LayerSchema::attributeContainerKey("Road", "rules"),
        "AttributeContainer");
    auto const speedSchema = schema->addSchema(
        simfil::Schema::Kind::Object,
        LayerSchema::attributeKey("Road", "rules", "speed.limit"),
        "Attribute");
    auto const valueSchema = schema->addSchema(
        simfil::Schema::Kind::Value,
        "Road.rules.speed-limit.value");

    schema->addFieldSchema(featureSchema, "properties", propertiesSchema);
    schema->addFieldSchema(propertiesSchema, "layer", layerMapSchema);
    schema->addFieldSchema(layerMapSchema, "rules", rulesSchema);
    schema->addFieldSchema(rulesSchema, "speed.limit", speedSchema);
    schema->addFieldSchema(speedSchema, "value.with.dot", valueSchema);
    schema->setAttributeMetadata(
        speedSchema,
        LayerSchema::AttributePathOwner{"Road", "rules", "speed.limit", speedSchema},
        "synthetic.SpeedAttribute");
    schema->setZserioType(valueSchema, "synthetic.SpeedValue");
    schema->addEnumSymbol(valueSchema, "FAST");
    schema->finalize();

    auto schemaEmitterCalls = 0;
    auto transportSchema = nlohmann::json{{"type", "object"}, {"x-test", "direct"}};
    schema->setJsonSchemaEmitter([&] {
        ++schemaEmitterCalls;
        return transportSchema;
    });

    REQUIRE(schema->featureTypes() == std::vector<std::string>{"Road"});
    REQUIRE(schema->canHaveField(featureSchema, "value.with.dot"));
    REQUIRE(schema->constantTypeNames(speedSchema, "speed.limit") ==
            std::vector<std::string>{"synthetic.SpeedAttribute"});

    auto scopes = schema->attributeScopes();
    REQUIRE(scopes.size() == 1);
    REQUIRE(scopes.front().featureType_ == "Road");
    REQUIRE(scopes.front().attributeLayerName_ == "rules");
    REQUIRE(scopes.front().attributeName_ == "speed.limit");

    auto normalized = schema->normalizeSearchQuery(
        R"(properties.layer.rules["speed.limit"]["value.with.dot"] == 42)",
        LayerSchema::SearchQueryRequestedScope::Auto);
    REQUIRE(normalized.has_value());
    REQUIRE(normalized->concreteScope_ == LayerSchema::SearchQueryConcreteScope::Attribute);
    REQUIRE(normalized->normalizedQuery_.find(R"(["value.with.dot"] == 42)") != std::string::npos);
    REQUIRE(normalized->normalizedQuery_.find(R"(.["value.with.dot"])") == std::string::npos);

    auto detached = schema->detachedCopy();
    REQUIRE(schemaEmitterCalls == 1);
    REQUIRE(detached->featureTypes() == std::vector<std::string>{"Road"});
    REQUIRE(detached->toJsonSchema() == transportSchema);
    REQUIRE(schemaEmitterCalls == 1);
}

TEST_CASE("LayerSchema compacts broad attribute searches beyond the guarded rewrite limit", "[DataSourceInfo]")
{
    auto makeSchema = [](int scopeCount) {
        auto schema = std::make_shared<LayerSchema>();
        for (auto index = 0; index < scopeCount; ++index) {
            auto const featureType = "Road" + std::to_string(index);
            auto const featureSchema = schema->addSchema(
                simfil::Schema::Kind::Object,
                LayerSchema::featureKey(featureType),
                "Feature");
            auto const propertiesSchema = schema->addSchema(
                simfil::Schema::Kind::Object,
                LayerSchema::featurePropertiesKey(featureType),
                "FeatureProperties");
            auto const layerMapSchema = schema->addSchema(
                simfil::Schema::Kind::Object,
                LayerSchema::attributeLayerMapKey(featureType),
                "AttributeLayerMap");
            auto const guidanceSchema = schema->addSchema(
                simfil::Schema::Kind::Object,
                LayerSchema::attributeContainerKey(featureType, "Guidance"),
                "AttributeContainer");
            auto const warningSchema = schema->addSchema(
                simfil::Schema::Kind::Object,
                LayerSchema::attributeKey(featureType, "Guidance", "WARNING_SIGN"),
                "Attribute");
            auto const valueSchema = schema->addSchema(
                simfil::Schema::Kind::Value,
                featureType + ".Guidance.WARNING_SIGN.warningSign");

            schema->addFieldSchema(featureSchema, "properties", propertiesSchema);
            schema->addFieldSchema(propertiesSchema, "layer", layerMapSchema);
            schema->addFieldSchema(layerMapSchema, "Guidance", guidanceSchema);
            schema->addFieldSchema(guidanceSchema, "WARNING_SIGN", warningSchema);
            schema->addFieldSchema(warningSchema, "warningSign", valueSchema);
            schema->setAttributeMetadata(
                warningSchema,
                LayerSchema::AttributePathOwner{
                    featureType,
                    "Guidance",
                    "WARNING_SIGN",
                    warningSchema},
                "synthetic.WarningSignAttribute");
        }
        schema->finalize();
        return schema;
    };

    auto bounded = makeSchema(1)->normalizeSearchQuery(
        "**.warningSign",
        LayerSchema::SearchQueryRequestedScope::Auto);
    REQUIRE(bounded.has_value());
    REQUIRE_FALSE(bounded->rewriteSuppressed_);
    REQUIRE(bounded->normalizedQuery_.find("**") == std::string::npos);

    constexpr auto scopeCount = 9;
    auto schema = makeSchema(scopeCount);

    auto normalized = schema->normalizeSearchQuery(
        "**.warningSign",
        LayerSchema::SearchQueryRequestedScope::Auto);
    REQUIRE(normalized.has_value());
    REQUIRE(normalized->concreteScope_ == LayerSchema::SearchQueryConcreteScope::Attribute);
    REQUIRE(normalized->attributeScopeCandidateCount_ == scopeCount);
    REQUIRE(normalized->rewriteSuppressed_);
    REQUIRE(normalized->attributeScopes_.empty());
    INFO(normalized->normalizedQuery_);
    REQUIRE(normalized->normalizedQuery_.find("**") == std::string::npos);
    auto const layerGuard = normalized->normalizedQuery_.find(R"($layer == "Guidance")");
    REQUIRE(layerGuard != std::string::npos);
    REQUIRE(normalized->normalizedQuery_.find(R"($layer == "Guidance")", layerGuard + 1) == std::string::npos);
    REQUIRE(normalized->normalizedQuery_.find(R"($name == "WARNING_SIGN")") != std::string::npos);
    REQUIRE(normalized->normalizedQuery_.find("warningSign") != std::string::npos);
}

TEST_CASE("TileFeatureLayer completes schema fields and enum symbols without mutating datasource strings", "[DataSourceInfo]")
{
    auto layerInfo = LayerInfo::fromJson(schemaAnnotatedLayerInfoJson());
    auto strings = std::make_shared<StringPool>("SchemaCompletionNode");
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "SchemaCompletionNode",
        "SchemaCompletionMap",
        layerInfo,
        strings);

    auto feature = tile->newFeature("Carrier", {{"carrierId", 7}});
    auto layer = feature->attributeLayers()->newLayer("limits");
    auto speed = layer->newAttribute("speed");

    REQUIRE(strings->get("unit") == simfil::StringPool::Empty);
    REQUIRE(strings->get("km/h") == simfil::StringPool::Empty);

    simfil::CompletionOptions opts;
    opts.showWildcardHints = false;

    simfil::ModelNode::Ptr speedNode = speed;
    auto fieldCompletions = tile->complete("u", 1, *speedNode, opts);
    REQUIRE(fieldCompletions);
    REQUIRE(hasCompletion(*fieldCompletions, "unit", simfil::CompletionCandidate::Type::FIELD));

    auto enumCompletions = tile->complete("k", 1, *speedNode, opts);
    REQUIRE(enumCompletions);
    REQUIRE(hasCompletion(*enumCompletions, "\"km/h\"", simfil::CompletionCandidate::Type::CONSTANT));

    REQUIRE(strings->get("unit") == simfil::StringPool::Empty);
    REQUIRE(strings->get("km/h") == simfil::StringPool::Empty);
}

TEST_CASE("TileFeatureLayer schema rewrites use enum paths", "[DataSourceInfo]")
{
    auto layerInfo = LayerInfo::fromJson(schemaAnnotatedLayerInfoJson());
    auto strings = std::make_shared<StringPool>("SchemaRewriteNode");
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "SchemaRewriteNode",
        "SchemaRewriteMap",
        layerInfo,
        strings);

    auto feature = tile->newFeature("Carrier", {{"carrierId", 7}});
    auto attrs = feature->attributes();
    REQUIRE(attrs->addField("displayName", "km/h").has_value());
    auto layer = feature->attributeLayers()->newLayer("limits");
    auto speed = layer->newAttribute("speed");
    REQUIRE(speed->addField("unit", "mph").has_value());

    auto matchingEnum = tile->evaluate("mph", *feature, false, true);
    REQUIRE(matchingEnum);
    REQUIRE(matchingEnum->values.size() == 1);
    REQUIRE(matchingEnum->values.front().isa(simfil::ValueType::Bool));
    REQUIRE(matchingEnum->values.front().as<simfil::ValueType::Bool>());

    auto unrelatedString = tile->evaluate(R"("km/h")", *feature, false, true);
    REQUIRE(unrelatedString);
    REQUIRE(unrelatedString->values.size() == 1);
    REQUIRE(unrelatedString->values.front().isa(simfil::ValueType::Bool));
    REQUIRE_FALSE(unrelatedString->values.front().as<simfil::ValueType::Bool>());
}

TEST_CASE("LayerSchema scalar attribute shorthand skips metadata fields", "[DataSourceInfo]")
{
    auto json = schemaAnnotatedLayerInfoJson();
    auto& defs = json["featureModelSchema"]["$defs"];
    defs["LimitsLayer"]["properties"]["priority"] = {{"$ref", "#/$defs/PriorityAttribute"}};
    defs["PriorityAttribute"] = {
        {"type", "object"},
        {"x-mapget", {
            {"metaType", "Attribute"},
            {"attributeTypeCode", "priority"},
            {"attributeType", "synthetic.PriorityAttributeType"}
        }},
        {"properties", {
            {"_sourceData", {
                {"type", "array"},
                {"items", {
                    {"type", "object"},
                    {"properties", {
                        {"address", {{"type", "integer"}}}
                    }}
                }}
            }},
            {"conditions", {
                {"type", "object"},
                {"properties", {
                    {"conditionValue", {{"type", "integer"}}}
                }}
            }},
            {"properties", {
                {"type", "object"},
                {"properties", {
                    {"propertyValue", {{"type", "integer"}}}
                }}
            }},
            {"references", {
                {"type", "object"},
                {"properties", {
                    {"referenceValue", {{"type", "integer"}}}
                }}
            }},
            {"validity", {
                {"type", "object"},
                {"properties", {
                    {"validityValue", {{"type", "integer"}}}
                }}
            }},
            {"value", {{"type", "integer"}}}
        }}
    };

    auto layerInfo = LayerInfo::fromJson(json);
    auto registry = layerInfo->layerSchema();
    REQUIRE(registry);

    auto const featureSchema = registry->featureSchema("Carrier");
    auto featurePaths = registry->scalarFieldPathsForAttribute(featureSchema, "priority");
    std::vector<std::string> featurePathStrings;
    featurePathStrings.reserve(featurePaths.size());
    for (auto const& path : featurePaths) {
        featurePathStrings.push_back(namedSchemaPathString(path));
    }
    REQUIRE(featurePathStrings == std::vector<std::string>{
        "properties.layer.advisoryLimits.priority.value",
        "properties.layer.limits.priority.value"});

    auto const layerMapSchema = registry->attributeLayerMapSchema("Carrier");
    auto const limitsSchema = registry->childSchema(layerMapSchema, "limits", simfil::Schema::Kind::Object);
    auto const prioritySchema = registry->childSchema(limitsSchema, "priority", simfil::Schema::Kind::Object);
    auto attributePaths = registry->scalarFieldPathsForAttribute(prioritySchema, "priority");
    REQUIRE(attributePaths.size() == 1);
    REQUIRE(namedSchemaPathString(attributePaths.front()) == "value");
}

TEST_CASE("LayerSchema classifies feature and attribute path owners", "[DataSourceInfo]")
{
    auto layerInfo = LayerInfo::fromJson(schemaAnnotatedLayerInfoJson());
    auto registry = layerInfo->layerSchema();
    REQUIRE(registry);

    auto const featureSchema = registry->featureSchema("Carrier");
    auto const typeIdPath = std::vector<std::string>{"typeId"};
    auto const featureOwner = registry->ownerForPath(
        "Carrier",
        featureSchema,
        typeIdPath);
    REQUIRE(featureOwner.kind_ == LayerSchema::PathOwnerKind::Feature);

    auto const displayNamePath = std::vector<std::string>{"properties", "displayName"};
    auto const displayNameOwner = registry->ownerForPath(
        "Carrier",
        featureSchema,
        displayNamePath);
    REQUIRE(displayNameOwner.kind_ == LayerSchema::PathOwnerKind::Feature);

    auto const layerMapPath = std::vector<std::string>{"properties", "layer"};
    auto const layerMapOwner = registry->ownerForPath(
        "Carrier",
        featureSchema,
        layerMapPath);
    REQUIRE(layerMapOwner.kind_ == LayerSchema::PathOwnerKind::Unknown);

    auto const attributeLayerPath = std::vector<std::string>{"properties", "layer", "limits"};
    auto const attributeLayerOwner = registry->ownerForPath(
        "Carrier",
        featureSchema,
        attributeLayerPath);
    REQUIRE(attributeLayerOwner.kind_ == LayerSchema::PathOwnerKind::Unknown);

    auto const speedUnitPath = std::vector<std::string>{"properties", "layer", "limits", "speed", "unit"};
    auto const attributeOwner = registry->ownerForPath(
        "Carrier",
        featureSchema,
        speedUnitPath);
    REQUIRE(attributeOwner.kind_ == LayerSchema::PathOwnerKind::Attribute);
    REQUIRE(attributeOwner.attribute_.featureType_ == "Carrier");
    REQUIRE(attributeOwner.attribute_.attributeLayerName_ == "limits");
    REQUIRE(attributeOwner.attribute_.attributeName_ == "speed");
    REQUIRE(attributeOwner.attribute_.attributeSchema_ != simfil::NoSchemaId);

    auto const advisorySpeedUnitPath = std::vector<std::string>{"properties", "layer", "advisoryLimits", "speed", "unit"};
    auto const advisoryAttributeOwner = registry->ownerForPath(
        "Carrier",
        featureSchema,
        advisorySpeedUnitPath);
    REQUIRE(advisoryAttributeOwner.kind_ == LayerSchema::PathOwnerKind::Attribute);
    REQUIRE(advisoryAttributeOwner.attribute_.attributeLayerName_ == "advisoryLimits");
    REQUIRE(advisoryAttributeOwner.attribute_.attributeName_ == "speed");
    REQUIRE(advisoryAttributeOwner.attribute_.attributeSchema_ != simfil::NoSchemaId);

    auto const propertiesSchema = registry->featurePropertiesSchema("Carrier");
    auto const propertiesRootAttributeOwner = registry->ownerForPath(
        "Carrier",
        propertiesSchema,
        std::vector<std::string>{"layer", "limits", "speed", "value"});
    REQUIRE(propertiesRootAttributeOwner.kind_ == LayerSchema::PathOwnerKind::Attribute);
    REQUIRE(propertiesRootAttributeOwner.attribute_.attributeLayerName_ == "limits");

    auto const speedSchema = attributeOwner.attribute_.attributeSchema_;
    auto const valuePath = std::vector<std::string>{"value"};
    auto const attributeRootOwner = registry->ownerForPath(
        "Carrier",
        speedSchema,
        valuePath);
    REQUIRE(attributeRootOwner.kind_ == LayerSchema::PathOwnerKind::Attribute);
    REQUIRE(attributeRootOwner.attribute_.attributeName_ == "speed");

    auto const invalidAttributeTailPath = std::vector<std::string>{"properties", "layer", "limits", "speed", "missing"};
    auto const invalidAttributeTailOwner = registry->ownerForPath(
        "Carrier",
        featureSchema,
        invalidAttributeTailPath);
    REQUIRE(invalidAttributeTailOwner.kind_ == LayerSchema::PathOwnerKind::Unknown);

    auto const invalidAttributeRootPath = std::vector<std::string>{"missing"};
    auto const invalidAttributeRootOwner = registry->ownerForPath(
        "Carrier",
        speedSchema,
        invalidAttributeRootPath);
    REQUIRE(invalidAttributeRootOwner.kind_ == LayerSchema::PathOwnerKind::Unknown);
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
    auto registry = tile->layerSchema();
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
        "stringPoolId": "testStringPoolId",
        "protocolVersion": {
            "major": 1,
            "minor": 0,
            "patch": 0
        }
    })"_json;

    // Attempting to deserialize should throw an exception because "mapId" is missing.
    REQUIRE_THROWS_AS(DataSourceInfo::fromJson(j), std::runtime_error);
}

TEST_CASE("Model metadata reports retained schema capacity", "[DataSourceInfo][memory]")
{
    auto layer = LayerInfo::fromJson(schemaAnnotatedLayerInfoJson());
    auto const layerMemory = layer->memoryUsage();
    REQUIRE(layerMemory.total().allocatedBytes > sizeof(LayerInfo));
    REQUIRE(layerMemory.components.contains("schema.object"));

    auto info = DataSourceInfo::fromJson(nlohmann::json{
        {"stringPoolId", "MemoryPool"},
        {"mapId", "MemoryMap"},
        {"layers", {{"MemoryLayer", layer->toJson()}}},
    });
    auto const infoMemory = info.memoryUsage();
    REQUIRE(infoMemory.total().allocatedBytes > sizeof(DataSourceInfo));
    REQUIRE(infoMemory.toJson()["quality"] == "capacity-lower-bound");
}
