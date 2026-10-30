#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include <sstream>
#include "mapget/log.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/incoming-relations.h"
#include "mapget/model/info.h"
#include "mapget/model/mapschema.h"
#include "mapget/model/simfilutil.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/stream.h"

using namespace mapget;

TEST_CASE("Layer visibility defaults are optional and preserve false", "[DataSourceInfo][pointcloud]")
{
    LayerInfo layer;
    layer.layerId_ = "Preview";
    CHECK_FALSE(layer.toJson().contains("defaultVisible"));
    layer.defaultVisible_ = false;
    auto parsed = LayerInfo::fromJson(layer.toJson());
    REQUIRE(parsed->defaultVisible_.has_value());
    CHECK_FALSE(*parsed->defaultVisible_);
    CHECK_FALSE(parsed->toJson()["defaultVisible"].get<bool>());
}

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

TEST_CASE(
    "Layer metadata separates schema roots from referenced identifier types",
    "[DataSourceInfo]")
{
    auto metadata = schemaAnnotatedLayerInfoJson();
    auto reference = metadata["featureTypes"][0];
    reference["name"] = "ReferenceOnly";
    metadata["featureTypes"].push_back(reference);
    // This is a derived projection, never an alternative source of schema truth.
    metadata["schemaFeatureTypes"] = {"ForgedType"};
    auto layer = LayerInfo::fromJson(metadata);
    auto published = layer->toJson();
    CHECK_FALSE(published.contains("featureTypes"));
    CHECK_FALSE(published.contains("schemaFeatureTypes"));
    CHECK_FALSE(published.contains("featureModelSchema"));
    CHECK(layer->featureTypes().size() == 2);
    CHECK(layer->layerSchema()->featureTypes() == std::vector<std::string>{"Carrier"});
    CHECK_FALSE(layer->layerSchema()->producesFeatureType("ReferenceOnly"));
    metadata.erase("featureModelSchema");
    CHECK_FALSE(LayerInfo::fromJson(metadata)->toJson().contains("schemaFeatureTypes"));
}

TEST_CASE("InfoToJson", "[DataSourceInfo]")
{
    mapget::setLogLevel("trace", log());


    // Create a DataSourceInfo object.
    std::unordered_map<std::string, std::shared_ptr<LayerInfo>> layers;
    auto layer = std::make_shared<LayerInfo>();
    layer->layerId_ = "testLayer";
    layer->zoomLevels_ = {0, 1, 2};
    layer->coverage_ = {
        {TileId::fromTileXY(0, 0, 0), TileId::fromTileXY(1, 0, 0), {}},
        {TileId::fromTileXY(0, 0, 1), TileId::fromTileXY(0, 0, 1), {}}};
    layer->version_ = {1, 0, 0};
    layers["testLayer"] = layer;

    DataSourceInfo info(DataSourceInfo{
        "testStringPoolId",
        "testMapId",
        layers,
        5,
        false,
        nlohmann::json::object(),
        TileLayerStream::CurrentProtocolVersion});

    auto owner = std::make_shared<MapSchema>();
    (void)owner->newLayer("testLayer", LayerType::Features);
    owner->finalize();
    info.setMapSchema(owner);

    // Serialize it to JSON.
    nlohmann::json j = info.toJson();
    log().trace("Serialized data source info: {}", to_string(j));

    // Deserialize it back into a DataSourceInfo object, then serialize it again.
    auto j2 = DataSourceInfo::fromJson(j, [owner](std::string_view) { return owner; }).toJson();

    // Check that the two DataSourceInfo objects are equal.
    REQUIRE(j == j2);

    SECTION("default datasource protocol is serialized as current stream protocol")
    {
        info.protocolVersion_ = Version{};
        REQUIRE(info.toJson().at("protocolVersion") == TileLayerStream::CurrentProtocolVersion.toJson());
    }
}

TEST_CASE("MapPartitionKey percent-escapes map and layer identifier components", "[DataSourceInfo]")
{
    MapPartitionKey
        key(LayerType::Features, "Map.A:B/C,D~%", "Layer:X/Y,Z~%", TileId::fromTileXY(1, 0, 1));

    auto const encoded = key.toString();
    REQUIRE(encoded == "Features:Map.A%3AB%2FC%2CD%7E%25:Layer%3AX%2FY%2CZ%7E%25:131073");

    auto const parsed = MapPartitionKey(encoded);
    REQUIRE(parsed.layer_ == key.layer_);
    REQUIRE(parsed.mapId_ == key.mapId_);
    REQUIRE(parsed.layerId_ == key.layerId_);
    REQUIRE(parsed.partitionId_ == key.partitionId_);
    REQUIRE_THROWS(MapPartitionKey(encoded + ":2"));
}

TEST_CASE("MapPartitionKey accepts removed mapget tile-id layout", "[DataSourceInfo]")
{
    auto const legacyTileId = (int64_t{1} << 32) | int64_t{1};
    auto const parsed = MapPartitionKey("Features:Map:Layer:" + std::to_string(legacyTileId));
    REQUIRE(parsed.partitionId_ == TileId::fromTileXY(3, 0, 1));
}

TEST_CASE("MapPartitionKey accepts removed hexadecimal mapget tile-id layout", "[DataSourceInfo]")
{
    auto const parsed = MapPartitionKey("Features:Map:Layer:21fa0777000d");
    REQUIRE(parsed.partitionId_ == TileId::fromTileXY(0x01fa, 0x0888, 13));
}

TEST_CASE("MapPartitionKey keeps SourceData tile zero as metadata sentinel", "[DataSourceInfo]")
{
    auto const parsed = MapPartitionKey("SourceData:Map:Layer:0");
    REQUIRE(parsed.partitionId_.value() == 0);
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

TEST_CASE("LayerInfo imports legacy featureModelSchema without publishing it", "[DataSourceInfo]")
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
    REQUIRE_FALSE(json.contains("featureModelSchema"));
    REQUIRE_FALSE(json.contains("featureTypes"));
    REQUIRE(layerInfo->schemaOwner()->finalized());
    REQUIRE(layerInfo->getTypeInfo("Way")->uniqueIdCompositions_.size() == 1);
}

TEST_CASE(
    "PartitionFeatureLayer validates emitted features against LayerInfo schema",
    "[DataSourceInfo]")
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

    auto tile = std::make_shared<PartitionFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "SchemaTestNode",
        "SchemaTestMap",
        layerInfo,
        std::make_shared<StringPool>("SchemaTestNode"));
    tile->newFeature("Way", {{"wayId", 1}});

    REQUIRE_NOTHROW(tile->validateSchema());

    // Published graphs cannot be changed underneath a live partition.
    REQUIRE_THROWS(layerInfo->layerSchema()->addSchema(simfil::Schema::Kind::Int));
    REQUIRE_NOTHROW(tile->validateSchema());
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
    // JSON Schema objects are open unless additionalProperties explicitly closes them.
    REQUIRE(registry->canHaveField(carrierSchema->id_, "notDeclaredBySchema"));

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
    REQUIRE(registry->kind(typeIdId) == simfil::Schema::Kind::String);
    REQUIRE(registry->kind(unitId) == simfil::Schema::Kind::String);
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

    auto tile = std::make_shared<PartitionFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "LayerSchemaReadonlyNode",
        "LayerSchemaReadonlyMap",
        layerInfo,
        strings);
    REQUIRE(tile->layerSchema());
    REQUIRE(strings->highest() == highestBefore);
    REQUIRE(strings->size() == sizeBefore);
}

TEST_CASE(
    "LayerSchema direct construction supports immutable snapshots and escaped field paths",
    "[DataSourceInfo]")
{
    auto owner = std::make_shared<MapSchema>();
    auto schema = owner->newLayer("Layer", LayerType::Features);
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
    schema->addFeatureType(
        FeatureTypeInfo::fromJson(
            {{"name", "Road"},
             {"uniqueIdCompositions", {{{{"partId", "id"}, {"datatype", "I64"}}}}}}),
        featureSchema);
    schema->setZserioType(valueSchema, "synthetic.SpeedValue");
    schema->addEnumSymbol(valueSchema, "FAST");
    schema->finalize();

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

    std::ostringstream binary;
    REQUIRE(owner->write(binary));
    auto bytes = binary.str();
    auto restoredOwner = MapSchema::read(
        {reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()},
        owner->schemaId());
    auto restored = restoredOwner->layer("Layer");
    REQUIRE(restored->featureTypes() == std::vector<std::string>{"Road"});
    REQUIRE(restored->featureSchema("Road") == featureSchema);
    REQUIRE(restored->kind(speedSchema) == LayerSchema::AttributeKind);
}

TEST_CASE("LayerSchema infers attribute scope for a nested recursive wildcard path", "[DataSourceInfo]")
{
    using Kind = simfil::Schema::Kind;
    auto owner = std::make_shared<MapSchema>();
    auto schema = owner->newLayer("Layer", LayerType::Features);
    auto feature = schema->addSchema(Kind::Object, LayerSchema::featureKey("Link"), "Feature");
    auto properties = schema->addSchema(Kind::Object, LayerSchema::featurePropertiesKey("Link"), "FeatureProperties");
    auto layers = schema->addSchema(Kind::Object, LayerSchema::attributeLayerMapKey("Link"), "AttributeLayerMap");
    auto guidance = schema->addSchema(Kind::Object, LayerSchema::attributeContainerKey("Link", "Guidance"), "AttributeContainer");
    auto attribute = schema->addSchema(Kind::Object, LayerSchema::attributeKey("Link", "Guidance", "NUM_LANES"), "Attribute");
    auto numLanes = schema->addSchema(Kind::Object, "NumLanes");
    auto number = schema->addSchema(Kind::Value, "NormalLanes");
    schema->addFieldSchema(feature, "properties", properties);
    schema->addFieldSchema(properties, "layer", layers);
    schema->addFieldSchema(layers, "Guidance", guidance);
    schema->addFieldSchema(guidance, "NUM_LANES", attribute);
    schema->addFieldSchema(attribute, "numLanes", numLanes);
    schema->addFieldSchema(numLanes, "normalLanes", number);
    schema->setAttributeMetadata(attribute,
        LayerSchema::AttributePathOwner{"Link", "Guidance", "NUM_LANES", attribute}, "NumLanesAttribute");

    schema->addFeatureType(
        FeatureTypeInfo::fromJson(
            {{"name", "Link"},
             {"uniqueIdCompositions", {{{{"partId", "id"}, {"datatype", "I64"}}}}}}),
        feature);
    bool mixedPaths = false;
    SECTION("Direct schema graph") {}
    SECTION("Same field under basic properties makes scope ambiguous")
    {
        schema->addFieldSchema(properties, "numLanes", numLanes);
        mixedPaths = true;
    }
    SECTION("Canonical transport with a large shared conditions graph")
    {
        // These unrelated paths previously consumed the wildcard enumeration
        // budget before reaching the attribute's actual value. Keep this fixture
        // synthetic while exercising the same direct-schema transport as producers.
        auto condition = schema->addSchema(Kind::Int, "ConditionValue");
        for (auto depth = 0; depth < 15; ++depth) {
            auto parent = schema->addSchema(Kind::Object, "Condition" + std::to_string(depth));
            schema->addFieldSchema(parent, "left", condition);
            schema->addFieldSchema(parent, "right", condition);
            condition = parent;
        }
        schema->addFieldSchema(attribute, "conditions", condition);
        schema->finalize();
        // The produced feature alias, not an optional descriptor type name, identifies Link.
        REQUIRE(schema->mapSchema()->domain(feature)->typeName().empty());
        schema = LayerSchema::fromJsonSchema(schema->toJsonSchema());
        REQUIRE(schema->featureSchema("Link") == feature);
        REQUIRE(schema->mapSchema()->domain(feature)->typeName().empty());
        REQUIRE(schema->featureTypes() == std::vector<std::string>{"Link"});
    }
    schema->finalize();

    for (auto query : {"**.normalLanes", "**.numLanes.normalLanes"}) {
        CAPTURE(query);
        auto normalized =
            schema->normalizeSearchQuery(query, LayerSchema::SearchQueryRequestedScope::Auto);
        REQUIRE(normalized);
        if (mixedPaths) {
            REQUIRE(normalized->concreteScope_ == LayerSchema::SearchQueryConcreteScope::Feature);
            continue;
        }
        REQUIRE(normalized->concreteScope_ == LayerSchema::SearchQueryConcreteScope::Attribute);
        REQUIRE(normalized->attributeScopes_.size() == 1);
        REQUIRE(normalized->attributeScopes_.front().attributeName_ == "NUM_LANES");
        REQUIRE(normalized->normalizedQuery_.find("numLanes.normalLanes") != std::string::npos);
        REQUIRE(normalized->normalizedQuery_.find("Guidance") != std::string::npos);
    }

    // Explicit feature scope must not turn one feature result into several attributes.
    auto featureScoped = schema->normalizeSearchQuery("**.numLanes.normalLanes", LayerSchema::SearchQueryRequestedScope::Feature);
    REQUIRE(featureScoped);
    REQUIRE(featureScoped->concreteScope_ == LayerSchema::SearchQueryConcreteScope::Feature);
    REQUIRE(featureScoped->normalizedQuery_ == "**.numLanes.normalLanes");
}

TEST_CASE(
    "PartitionFeatureLayer completes schema fields and enum symbols without mutating datasource "
    "strings",
    "[DataSourceInfo]")
{
    auto layerInfo = LayerInfo::fromJson(schemaAnnotatedLayerInfoJson());
    auto strings = std::make_shared<StringPool>("SchemaCompletionNode");
    auto tile = std::make_shared<PartitionFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "SchemaCompletionNode",
        "SchemaCompletionMap",
        layerInfo,
        strings);

    auto feature = tile->newFeature("Carrier", {{"carrierId", 7}});
    auto layer = feature->attributeLayers()->newLayer("limits");
    auto speed = layer->newAttribute("speed");

    REQUIRE(strings->get("unit") == simfil::StringPool::SchemaUnit);
    REQUIRE(strings->get("km/h") == simfil::StringPool::Empty);
    auto const initialStringCount = strings->size();
    auto const initialStringBytes = strings->bytes();

    simfil::CompletionOptions opts;
    opts.showWildcardHints = false;

    simfil::ModelNode::Ptr speedNode = speed;
    auto fieldCompletions = tile->complete("u", 1, *speedNode, opts);
    REQUIRE(fieldCompletions);
    REQUIRE(hasCompletion(*fieldCompletions, "unit", simfil::CompletionCandidate::Type::FIELD));

    auto enumCompletions = tile->complete("k", 1, *speedNode, opts);
    REQUIRE(enumCompletions);
    REQUIRE(hasCompletion(*enumCompletions, "\"km/h\"", simfil::CompletionCandidate::Type::CONSTANT));

    REQUIRE(strings->get("unit") == simfil::StringPool::SchemaUnit);
    REQUIRE(strings->get("km/h") == simfil::StringPool::Empty);
    CHECK(strings->size() == initialStringCount);
    CHECK(strings->bytes() == initialStringBytes);
}

TEST_CASE("PartitionFeatureLayer schema rewrites use enum paths", "[DataSourceInfo]")
{
    auto layerInfo = LayerInfo::fromJson(schemaAnnotatedLayerInfoJson());
    auto strings = std::make_shared<StringPool>("SchemaRewriteNode");
    auto tile = std::make_shared<PartitionFeatureLayer>(
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

    REQUIRE(strings->get("mph") == simfil::StringPool::Empty);

    auto matchingEnum = tile->evaluate("mph", *feature, false, true);
    REQUIRE(matchingEnum);
    REQUIRE(matchingEnum->values.size() == 1);
    REQUIRE(matchingEnum->values.front().isa(simfil::ValueType::Bool));
    REQUIRE(matchingEnum->values.front().as<simfil::ValueType::Bool>());
    REQUIRE(strings->get("mph") == simfil::StringPool::Empty);

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

TEST_CASE("PartitionFeatureLayer exposes SchemaIds on feature-model nodes", "[DataSourceInfo]")
{
    auto layerInfo = LayerInfo::fromJson(schemaAnnotatedLayerInfoJson());
    auto tile = std::make_shared<PartitionFeatureLayer>(
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
    REQUIRE(layerMemory.total().allocatedBytes >= sizeof(LayerInfo));
    REQUIRE_FALSE(layerMemory.components.contains("schema.object"));
    REQUIRE(layer->schemaOwner()->memoryUsage().total().allocatedBytes > 0);

    auto info = DataSourceInfo::fromJson(nlohmann::json{
        {"stringPoolId", "MemoryPool"},
        {"mapId", "MemoryMap"},
        {"layers", {{"CarrierLayer", schemaAnnotatedLayerInfoJson()}}},
    });
    auto const infoMemory = info.memoryUsage();
    REQUIRE(infoMemory.total().allocatedBytes > sizeof(DataSourceInfo));
    REQUIRE(infoMemory.toJson()["quality"] == "capacity-lower-bound");
}

TEST_CASE("MapFormat is a flat order-independent identity", "[DataSourceInfo][schema2]")
{
    auto first = MapFormat::fromJson(
        {{"name", "NDS.Live"},
         {"version", "2024.05"},
         {"subsystems", {{"road", "2024.05"}, {"lane", "2024.07"}}}});
    auto second = MapFormat::fromJson(
        {{"subsystems", {{"lane", "2024.07"}, {"road", "2024.05"}}},
         {"version", "2024.05"},
         {"name", "NDS.Live"}});
    CHECK(first == second);
    CHECK(first.toJson() == second.toJson());
    REQUIRE_THROWS(MapFormat::fromJson({{"version", "2024.05"}}));
    REQUIRE_THROWS(MapFormat::fromJson({{"name", "NDS.Live"}, {"subsystems", {{"lane", 202407}}}}));
}

TEST_CASE(
    "Legacy metadata shares one immutable schema without sharing runtime identity",
    "[DataSourceInfo][schema2]")
{
    auto metadata = nlohmann::json{
        {"mapId", "First"},
        {"stringPoolId", "PoolA"},
        {"layers", {{"CarrierLayer", schemaAnnotatedLayerInfoJson()}}}};
    auto first = DataSourceInfo::fromJson(metadata);
    metadata["mapId"] = "Second";
    metadata["stringPoolId"] = "PoolB";
    metadata["layers"]["CarrierLayer"]["coverage"] = {TileId::fromWgs84(11., 48., 13).value()};
    auto second = DataSourceInfo::fromJson(metadata);
    REQUIRE(first.mapSchema_ == second.mapSchema_);
    CHECK(first.layers_.at("CarrierLayer") != second.layers_.at("CarrierLayer"));
    CHECK(first.schemaId_ == second.schemaId_);
    CHECK(first.stringPoolId_ != second.stringPoolId_);
    auto published = first.toJson();
    CHECK(published.contains("schemaId"));
    CHECK(published.contains("mapFormat"));
    CHECK_FALSE(published["layers"]["CarrierLayer"].contains("featureTypes"));
    CHECK_FALSE(published["layers"]["CarrierLayer"].contains("featureModelSchema"));

    auto pending = DataSourceInfo::fromJson(published);
    REQUIRE_FALSE(pending.mapSchema_);
    REQUIRE_THROWS(pending.validateSchema());
    REQUIRE_THROWS(std::make_shared<PartitionFeatureLayer>(
        TileId::fromWgs84(11., 48., 13),
        "PoolA",
        "First",
        pending.layers_.at("CarrierLayer"),
        std::make_shared<StringPool>("PoolA")));
    pending.setMapSchema(first.mapSchema_);
    REQUIRE_NOTHROW(pending.validateSchema());
    CHECK(pending.toJson() == published);

    published["layers"]["CarrierLayer"]["featureTypes"] = nlohmann::json::array();
    REQUIRE_THROWS(DataSourceInfo::fromJson(published));
}

TEST_CASE(
    "Schema replacement is atomic and partitions reject foreign graph identities",
    "[DataSourceInfo][schema2]")
{
    auto metadata = nlohmann::json{
        {"mapId", "Map"},
        {"stringPoolId", "Pool"},
        {"layers", {{"CarrierLayer", schemaAnnotatedLayerInfoJson()}}}};
    auto first = DataSourceInfo::fromJson(metadata);
    auto layer = first.layers_.at("CarrierLayer");
    auto strings = std::make_shared<StringPool>("Pool");
    auto tile = std::make_shared<
        PartitionFeatureLayer>(TileId::fromWgs84(11., 48., 13), "Pool", "Map", layer, strings);
    tile->newFeature("Carrier", {{"carrierId", 7}});
    std::ostringstream output;
    REQUIRE(tile->write(output));
    auto wire = output.str();
    std::vector<uint8_t> bytes(wire.begin(), wire.end());

    metadata["layers"]["CarrierLayer"]["featureModelSchema"]["$defs"]["CarrierProperties"]
            ["properties"]["extra"] = {{"type", "boolean"}};
    auto second = DataSourceInfo::fromJson(metadata);
    REQUIRE(first.mapFormat_ == second.mapFormat_);
    REQUIRE(first.schemaId_ != second.schemaId_);
    REQUIRE_THROWS(first.setMapSchema(second.mapSchema_));
    CHECK(first.layers_.at("CarrierLayer") == layer);
    REQUIRE_THROWS(tile->setLayerInfo(second.layers_.at("CarrierLayer")));
    CHECK(tile->layerInfo() == layer);
    REQUIRE_THROWS(std::make_shared<PartitionFeatureLayer>(
        bytes,
        [&second](auto const&, auto const&) { return second.layers_.at("CarrierLayer"); },
        [strings](auto const&) { return strings; }));

    first = {};
    auto restored = std::make_shared<PartitionFeatureLayer>(
        bytes,
        [layer](auto const&, auto const&) { return layer; },
        [strings](auto const&) { return strings; });
    CHECK(restored->find("Carrier.7"));
    CHECK(restored->layerSchema()->featureSchema("Carrier") != simfil::NoSchemaId);
}

TEST_CASE(
    "Native feature cloning preserves unknown dynamic container domains",
    "[DataSourceInfo][schema2][clone]")
{
    auto info = LayerInfo::fromJson(schemaAnnotatedLayerInfoJson());
    auto strings = std::make_shared<StringPool>("CloneOpen");
    auto source = std::make_shared<
        PartitionFeatureLayer>(TileId::fromWgs84(11., 48., 13), "CloneOpen", "Map", info, strings);
    auto feature = source->newFeature("Carrier", {{"carrierId", 7}});
    auto dynamic = source->newObject();
    auto values = source->newArray();
    values->append(source->newValue(int64_t{42}));
    REQUIRE(dynamic->addField("items", values));
    REQUIRE(feature->attributes()->addField("dynamic", dynamic));
    REQUIRE(dynamic->schema() == simfil::NoSchemaId);
    REQUIRE(values->schema() == simfil::NoSchemaId);
    auto snapshot = cloneInspectionFeatures(source, std::vector<std::string>{"Carrier.7"});
    auto cloned = snapshot->find("Carrier.7");
    REQUIRE(cloned);
    auto object = cloned->attributesOrNull()->get(strings->get("dynamic"));
    REQUIRE(object);
    CHECK(object->schema() == simfil::NoSchemaId);
    auto array = object->get(strings->get("items"));
    REQUIRE(array);
    CHECK(array->schema() == simfil::NoSchemaId);
    CHECK(array->at(0)->value() == simfil::ScalarValueType{int64_t{42}});
}

TEST_CASE(
    "Standalone legacy SourceData metadata receives an open bound schema",
    "[DataSourceInfo][schema2]")
{
    auto layer = LayerInfo::fromJson({{"layerId", "Raw"}, {"type", "SourceData"}});
    REQUIRE(layer->schemaOwner());
    CHECK(layer->schemaOwner()->finalized());
    REQUIRE(layer->layerSchema());
    CHECK(layer->layerSchema()->name() == "Raw");
    auto strings = std::make_shared<StringPool>("RawPool");
    CHECK_NOTHROW(std::make_shared<PartitionSourceDataLayer>(
        TileId::fromWgs84(11., 48., 13),
        "RawPool",
        "Map",
        layer,
        strings));
}
