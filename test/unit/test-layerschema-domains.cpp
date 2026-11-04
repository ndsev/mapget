#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json-schema.hpp>
#include <sstream>

#include "mapget/model/featurelayer.h"
#include "mapget/model/layerschema.h"
#include "mapget/model/mapschema.h"
#include "mapget/model/simfilutil.h"
#include "simfil/diagnostics.h"
#include "simfil/model/json.h"
#include "simfil/model/schema-model.h"
#include "simfil/simfil.h"

using namespace mapget;
using Kind = simfil::Schema::Kind;
using Composition = simfil::Schema::Composition;

namespace
{

/** Create a mutable layer view whose graph survives the local builder variable. */
LayerSchema::Ptr newTestSchema()
{
    return std::make_shared<MapSchema>()->newLayer("Test", LayerType::Features);
}

/** Supply real identity metadata without constraining the property-domain fixture. */
FeatureTypeInfo testFeatureType(std::string name, std::string idPart = "id")
{
    return {std::move(name), {{{std::move(idPart), {}, IdPartDataType::I64}}}};
}

/** Explicit JSON import builds a new immutable owner, never a detached runtime snapshot. */
LayerSchema::Ptr importTestSchema(
    nlohmann::json const& json,
    std::vector<FeatureTypeInfo> const& features = {},
    std::string name = "Test")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->importLayer(std::move(name), LayerType::Features, json, features);
    owner->finalize();
    return layer;
}

/** Exercise the codec with the same schema-owned identity declarations. */
LayerSchema::Ptr roundtripSchema(LayerSchema::Ptr const& schema)
{
    auto features = schema->featureTypeInfos();
    return importTestSchema(
        schema->toJsonSchema(),
        {features.begin(), features.end()},
        std::string(schema->name()));
}

/** Bind the same typed graph used by completion to a bounded metadata descriptor. */
nlohmann::json describe(LayerSchema::Ptr registry, simfil::TypeId root)
{
    auto strings = std::make_shared<StringPool>("SchemaDescriptorTest");
    auto env = makeEnvironment(strings);
    installCompletionLayerSchema(*env, registry, strings);
    auto model = std::make_shared<simfil::SchemaModel>(strings, env->querySchemaCallback);
    return model->root(root)->toJson();
}

/** Check owning candidate text without depending on incidental suggestion ordering. */
bool suggests(
    simfil::Environment& env,
    simfil::TypeId root,
    std::string_view query,
    std::string_view text)
{
    auto candidates = simfil::complete(env, query, query.size(), root);
    REQUIRE(candidates);
    return std::ranges::any_of(
        *candidates,
        [&](auto const& candidate) { return candidate.text == text; });
}

}  // namespace

TEST_CASE(
    "Closed geometry types preserve coordinate lookup through native and JSON transport",
    "[schema-domain][schema-native][geometry-schema]")
{
    auto owner = std::make_shared<MapSchema>();
    for (auto [name, builtin] :
         {std::pair{"Geometry", MapSchema::Builtin::Geometry},
          {"AttrPoint", MapSchema::Builtin::AttrPoint},
          {"Validity", MapSchema::Builtin::Validity}})
        owner->newLayer(name, LayerType::SourceData)->setRootSchema(owner->builtin(builtin));
    owner->finalize();
    std::ostringstream output;
    REQUIRE(owner->write(output));
    auto bytes = output.str();
    auto decoded = MapSchema::read({reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()});

    for (auto name : {"Geometry", "AttrPoint", "Validity"}) {
        auto importedOwner = std::make_shared<MapSchema>();
        auto imported =
            importedOwner
                ->importLayer(name, LayerType::SourceData, owner->layer(name)->toJsonSchema());
        importedOwner->finalize();
        for (auto const& layer : {owner->layer(name), decoded->layer(name), imported}) {
            CAPTURE(name, layer == imported);
            auto graph = layer->mapSchema();
            auto root = layer->rootSchema();
            auto position = layer->childSchema(
                root,
                std::string_view(name) == "Geometry" ? "coordinates" : "point");
            REQUIRE(position != simfil::NoTypeId);
            auto shape = graph->type(position);
            REQUIRE(shape->composition() == Composition::AnyOf);
            REQUIRE(shape->reachabilityComplete());
            CHECK_FALSE(layer->canHaveField(position, "payload"));
            REQUIRE(layer->canHaveField(position, "lon"));
            auto alternatives = shape->alternatives();
            REQUIRE_FALSE(alternatives.empty());
            auto point = alternatives.back();
            nlohmann::json_schema::json_validator validator;
            REQUIRE_NOTHROW(validator.set_root_schema(layer->toJsonSchema()));
            nlohmann::json coordinate = {11., 48., 7.};
            if (std::string_view(name) == "Geometry") {
                REQUIRE(alternatives.size() == 4);
                point = alternatives.front();
                for (size_t depth = 0; depth != alternatives.size(); ++depth) {
                    CAPTURE(depth);
                    auto current = alternatives[depth];
                    for (size_t i = 0; i < depth; ++i)
                        current = graph->type(current)->itemSchema();
                    CHECK(current == point);
                    CHECK_NOTHROW(
                        validator.validate({{"type", "LineString"}, {"coordinates", coordinate}}));
                    coordinate = nlohmann::json::array({coordinate});
                }
                CHECK_THROWS(
                    validator.validate({{"type", "LineString"}, {"coordinates", coordinate}}));
                for (auto invalid :
                     {R"([11,48])"_json,
                      R"([11,48,0,1])"_json,
                      R"([11,"north",0])"_json,
                      R"({"payload":1})"_json})
                    CHECK_THROWS(validator.validate({{"type", "Point"}, {"coordinates", invalid}}));
                auto bounds = layer->childSchema(root, "aabb");
                REQUIRE(layer->kind(bounds) == Kind::Object);
                CHECK(layer->childSchema(bounds, "origin") == point);
                CHECK(layer->childSchema(bounds, "size") == point);
                CHECK_FALSE(layer->canHaveField(bounds, "payload"));
                CHECK_NOTHROW(validator.validate(
                    R"({"type":"Polygon","aabb":{"origin":[11,48,0],"size":[1,2,3]}})"_json));
                CHECK_THROWS(
                    validator.validate(R"({"type":"Polygon","aabb":[[11,48,0],[1,2,3]]})"_json));
            }
            else {
                REQUIRE(alternatives.size() == 2);
                bool attribute = std::string_view(name) == "AttrPoint";
                CHECK(
                    layer->kind(alternatives.front()) == (attribute ? Kind::String : Kind::Float));
                auto value = nlohmann::json{{"point", coordinate}};
                if (attribute)
                    value["index"] = 1;
                CHECK_NOTHROW(validator.validate(value));
                value["point"] = attribute ? nlohmann::json("11,48,7") : nlohmann::json(3.5);
                CHECK_NOTHROW(validator.validate(value));
                value["point"] = true;
                CHECK_THROWS(validator.validate(value));
                value["point"] = {{"payload", 1}};
                CHECK_THROWS(validator.validate(value));
            }
            auto definition = graph->type(point);
            REQUIRE(definition);
            CHECK(definition->kind() == Kind::Array);
            CHECK(definition->typeName() == "mapget::Point");
            REQUIRE(definition->directFields().size() == 3);
            for (auto field : {"lon", "lat", "elevation"}) {
                auto id = graph->strings()->get(field);
                CHECK(definition->fieldRole(id) == simfil::Schema::FieldRole::Context);
                CHECK(definition->fieldSchema(id) == definition->itemSchema());
            }
            size_t enumerated = 0;
            definition->forEachEnumerableField([&](auto, auto) { ++enumerated; });
            CHECK(enumerated == 0);
            auto strings = std::make_shared<StringPool>("PointCompletion");
            auto env = makeEnvironment(strings);
            installCompletionLayerSchema(*env, layer, strings);
            CHECK(suggests(*env, point, "lo", "lon"));
            if (std::string_view(name) == "Geometry")
                CHECK(suggests(*env, root, "coordinates[0].lo", "lon"));
        }
    }
}

TEST_CASE(
    "Native geometry aliases remain queryable while unrelated wildcards skip coordinates",
    "[schema-domain][schema-native][geometry-schema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Test", LayerType::Features);
    auto properties = layer->addSchema(Kind::Object);
    layer->addFieldSchema(properties, "payload", graph->primitive(Kind::Int));
    REQUIRE(layer->newFeature(testFeatureType("Carrier", "identifier"), properties));
    graph->finalize();
    std::ostringstream output;
    REQUIRE(graph->write(output));
    auto bytes = output.str();
    auto decoded = MapSchema::read({reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()});
    for (auto const& registry : {layer, decoded->layer("Test"), roundtripSchema(layer)}) {
        DataSourceInfo info;
        info.mapId_ = "GeometryMap";
        info.stringPoolId_ = "GeometryQueries";
        auto layerInfo = std::make_shared<LayerInfo>();
        layerInfo->layerId_ = "Test";
        layerInfo->type_ = LayerType::Features;
        info.layers_.emplace("Test", layerInfo);
        info.setMapSchema(registry->mapSchema());
        auto strings = std::make_shared<StringPool>(info.stringPoolId_);
        auto tile = std::make_shared<PartitionFeatureLayer>(
            TileId::fromValue(131073),
            info.stringPoolId_,
            info.mapId_,
            info.getLayer("Test"),
            strings);
        auto env = makeEnvironment(strings);
        installLayerSchema(*env, registry, strings);
        size_t plannedEvaluations = 0;
        for (int64_t points : {2, 257}) {
            auto feature = tile->newFeature("Carrier", {{"identifier", points}});
            REQUIRE(feature->attributes()->addField("payload", int64_t{17}));
            auto geometry = feature->geom()->newGeometry(GeomType::Line, points, true);
            for (int64_t i = 0; i < points; ++i)
                geometry->append({11., 48., 7.});
            uint32_t basicEvaluations = 0;
            for (bool plans : {false, true}) {
                CAPTURE(points, plans);
                env->enableWildcardFieldPlans = plans;
                auto query = simfil::compile(*env, "**.payload", {.any = false});
                REQUIRE(query);
                simfil::Diagnostics diagnostics;
                auto values = simfil::eval(*env, **query, *feature, &diagnostics);
                REQUIRE(values);
                REQUIRE(values->size() == 1);
                CHECK(values->front().as<simfil::ValueType::Int>() == 17);
                REQUIRE(diagnostics.fieldData_.size() == 1);
                auto evaluations = diagnostics.fieldData_.front().evaluations;
                if (!plans)
                    basicEvaluations = evaluations;
                else {
                    CHECK(evaluations < basicEvaluations);
                    // The work is independent of vertex count, not merely lower on average.
                    if (points == 2)
                        plannedEvaluations = evaluations;
                    else
                        CHECK(evaluations == plannedEvaluations);
                }
                for (auto const& [text, expected] :
                     std::vector<std::pair<std::string, simfil::ScalarValueType>>{
                         {"count(**.lon)", points},
                         {"geometry.coordinates[0].lon", 11.},
                         {"geometry.coordinates[0].lat", 48.},
                         {"geometry.coordinates[0].elevation", 7.}})
                {
                    CAPTURE(text);
                    auto expression = simfil::compile(*env, text, {.any = false});
                    REQUIRE(expression);
                    auto result = simfil::eval(*env, **expression, *feature, nullptr);
                    REQUIRE(result);
                    REQUIRE(result->size() == 1);
                    CHECK(result->front().getScalar() == expected);
                }
            }
        }
    }
}

TEST_CASE(
    "Native Attribute aggregates retain their kind across query overlays and transport",
    "[schema-domain][schema-native][attribute-aggregate]")
{
    auto composition = GENERATE(Composition::AnyOf, Composition::OneOf, Composition::AllOf);
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Test", LayerType::Features);
    REQUIRE(layer->newFeature(testFeatureType("Carrier", "identifier")));
    auto integer = graph->primitive(Kind::Int);
    auto left = graph->newAttribute("left", "LeftAttribute");
    auto right = graph->newAttribute("right", "RightAttribute");
    layer->addFieldSchema(left, "leftValue", integer);
    layer->addFieldSchema(right, "rightValue", integer);
    auto choice = layer->addSchema(Kind::Union, "attribute-choice");
    layer->setComposition(choice, composition);
    layer->addAlternative(choice, left);
    layer->addAlternative(choice, right);
    auto affinities = simfil::Schema::affinities(layer->kind(choice));
    layer->bindAttribute(choice, "Carrier", "rules", "CHOICE");
    CHECK(simfil::Schema::affinities(layer->kind(choice)) == affinities);
    CHECK(
        simfil::Schema::kindNameId(layer->kind(choice)) ==
        simfil::Schema::kindNameId(LayerSchema::AttributeKind));
    auto container = layer->addSchema(Kind::Object);
    layer->addFieldSchema(layer->attributeLayerMapSchema("Carrier"), "rules", container);
    layer->addFieldSchema(container, "CHOICE", choice, true);

    auto mixed = layer->addSchema(Kind::Union, "mixed-choice");
    layer->setComposition(mixed, composition);
    layer->addAlternative(mixed, left);
    layer->addAlternative(mixed, integer);
    auto mixedKind = layer->kind(mixed);
    REQUIRE_THROWS_AS(
        layer->bindAttribute(mixed, "Carrier", "rules", "MIXED"),
        std::invalid_argument);
    CHECK(layer->kind(mixed) == mixedKind);
    CHECK(layer->attributeOwners(mixed).empty());
    REQUIRE_NOTHROW(graph->finalize());
    // Finalization computes the aggregate's object affinity from its alternatives.
    affinities = simfil::Schema::affinities(layer->kind(choice));
    CHECK(affinities == simfil::valueTypeAffinity(simfil::ValueType::Object));
    std::ostringstream output;
    REQUIRE(graph->write(output));
    auto bytes = output.str();
    auto decoded = MapSchema::read({reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()});
    for (auto const& registry : {layer, decoded->layer("Test"), roundtripSchema(layer)}) {
        auto payload = registry->schemaId("attribute-choice");
        REQUIRE(payload != simfil::NoTypeId);
        CHECK(
            simfil::Schema::kindNameId(registry->kind(payload)) ==
            simfil::Schema::kindNameId(LayerSchema::AttributeKind));
        CHECK(simfil::Schema::affinities(registry->kind(payload)) == affinities);
        CHECK(registry->composition(payload) == composition);
        REQUIRE(registry->mapSchema()->type(payload)->alternatives().size() == 2);
        auto query = registry->attributeQuerySchema("Carrier", payload);
        REQUIRE(query != simfil::NoTypeId);
        CHECK(registry->mapSchema()->baseSchema(query) == payload);
        CHECK(registry->canHaveField(query, "leftValue"));
        CHECK(registry->canHaveField(query, "rightValue"));
        auto strings = std::make_shared<StringPool>("AttributeAggregate");
        auto env = makeEnvironment(strings);
        installCompletionLayerSchema(*env, registry, strings);
        CHECK(suggests(*env, query, "leftV", "leftValue"));
        CHECK(suggests(*env, query, "rightV", "rightValue"));
    }
}

TEST_CASE(
    "Unannotated JSON arrays do not invent named lookups from ignored properties",
    "[schema-domain][geometry-schema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->importLayer(
        "Raw",
        LayerType::SourceData,
        R"({"type":"array","items":{"type":"number"},"properties":{"ghost":{"type":"string"}}})"_json);
    graph->finalize();
    CHECK(layer->directFields(layer->rootSchema()).empty());
    CHECK(layer->childSchema(layer->rootSchema(), "ghost") == simfil::NoTypeId);
    CHECK_FALSE(layer->canHaveField(layer->rootSchema(), "ghost"));
}

TEST_CASE(
    "Feature lookup aliases resolve canonical schema paths without duplicating enumeration",
    "[schema-domain]")
{
    auto schema = newTestSchema();
    auto root = schema->addSchema(Kind::Object, LayerSchema::featureKey("Intersection"), "Feature");
    schema->addFeatureType(testFeatureType("Intersection"), root);
    auto properties = schema->addSchema(Kind::Object);
    auto roads = schema->addSchema(Kind::Array);
    auto road = schema->addSchema(Kind::Int);
    auto wrapper = schema->addSchema(Kind::Object);
    // Keep this auxiliary query root in the selected layer's JSON projection.
    schema->registerSchemaKey("query-wrapper", wrapper);
    schema->addFieldSchema(root, "properties", properties);
    schema->addFieldSchema(properties, "connectedRoads", roads);
    schema->addElementSchema(roads, road);
    schema->addFieldSchema(wrapper, "$feature", root);
    schema->finalize();

    for (bool imported : {false, true}) {
        CAPTURE(imported);
        auto registry = imported ? roundtripSchema(schema) : schema;
        REQUIRE(registry->schemaId("query-wrapper") == wrapper);
        auto strings = std::make_shared<StringPool>("FeatureAliasTest");
        auto env = makeEnvironment(strings);
        installCompletionLayerSchema(*env, registry, strings);
        for (auto query :
             {"#attributes.connectedRoads > 3",
              "attributes.connectedRoads == 'x'",
              "**.attributes.connectedRoads",
              "**.attributes.connectedRoads == 'x'",
              "#properties.connectedRoads > 3"})
        {
            CAPTURE(imported, query);
            auto ast = simfil::compile(*env, query, {.any = false});
            REQUIRE(ast);
            auto refs = simfil::referencedSchemaPaths(*env, **ast, root);
            REQUIRE(refs);
            REQUIRE_FALSE(refs->hasUnresolvedAccess);
            REQUIRE(refs->paths.size() == 1);
            CHECK(refs->paths[0].path[0].field == StringPool::PropertiesStr);
        }
        CHECK(suggests(*env, root, "attributes.connectedR", "connectedRoads"));
        auto fields = simfil::Schema::fieldPaths(
            root,
            env->querySchemaCallback,
            strings->get("connectedRoads"));
        REQUIRE(fields.size() == 1);
        CHECK(fields[0][0].field == StringPool::PropertiesStr);
        auto ast = simfil::compile(*env, "#$feature.attributes.connectedRoads > 3", {.any = false});
        REQUIRE(ast);
        auto refs = simfil::referencedSchemaPaths(*env, **ast, wrapper);
        REQUIRE(refs);
        REQUIRE_FALSE(refs->hasUnresolvedAccess);
        CHECK(refs->paths[0].path[1].field == StringPool::PropertiesStr);
        CHECK(registry->canHaveField(wrapper, "attributes"));
        auto recursive = simfil::compile(*env, "**.attributes.connectedRoads", {.any = false});
        REQUIRE(recursive);
        auto nestedRefs = simfil::referencedSchemaPaths(*env, **recursive, wrapper);
        REQUIRE(nestedRefs);
        REQUIRE_FALSE(nestedRefs->hasUnresolvedAccess);
        REQUIRE(nestedRefs->paths.size() == 1);
        CHECK(nestedRefs->paths[0].path[1].field == StringPool::PropertiesStr);
        auto aliases =
            simfil::Schema::fieldPaths(root, env->querySchemaCallback, StringPool::AttributesStr);
        REQUIRE(aliases.size() == 1);
        CHECK(aliases[0][0].field == StringPool::PropertiesStr);
        CHECK(
            std::ranges::find(registry->nestedFields(wrapper), "attributes") ==
            registry->nestedFields(wrapper).end());
    }

    // A new immutable contract gives a real attributes member precedence over the alias.
    REQUIRE_THROWS(schema->addSchema(Kind::Object));
    schema = newTestSchema();
    root = schema->addSchema(Kind::Object, LayerSchema::featureKey("Intersection"), "Feature");
    schema->addFeatureType(testFeatureType("Intersection"), root);
    properties = schema->addSchema(Kind::Object);
    roads = schema->addSchema(Kind::Array);
    road = schema->addSchema(Kind::Int);
    schema->addFieldSchema(root, "properties", properties);
    schema->addFieldSchema(properties, "connectedRoads", roads);
    schema->addElementSchema(roads, road);
    auto real = schema->addSchema(Kind::Object);
    schema->addFieldSchema(real, "actual", road);
    schema->addFieldSchema(root, "attributes", real);
    schema->finalize();
    auto strings = std::make_shared<StringPool>("FeatureAliasShadowTest");
    auto env = makeEnvironment(strings);
    installCompletionLayerSchema(*env, schema, strings);
    for (auto query : {"attributes.actual", "attributes.connectedRoads"}) {
        auto ast = simfil::compile(*env, query, {.any = false});
        REQUIRE(ast);
        auto refs = simfil::referencedSchemaPaths(*env, **ast, root);
        REQUIRE(refs);
        CHECK(
            refs->hasUnresolvedAccess == (std::string_view(query) == "attributes.connectedRoads"));
        if (!refs->paths.empty())
            CHECK(refs->paths[0].path[0].field == StringPool::AttributesStr);
    }
}

TEST_CASE(
    "Native schema transport preserves validation constraints and JSON projections",
    "[DataSourceInfo][schema-domain]")
{
    auto schema = newTestSchema();
    auto root = schema->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    schema->addFeatureType(testFeatureType("Carrier"), root);
    auto count = schema->addSchema(Kind::Int);
    auto label = schema->addSchema(Kind::String);
    auto phase = schema->addSchema(Kind::String);
    auto flags = schema->addSchema(LayerSchema::BitmaskKind);
    auto bytes = schema->addSchema(Kind::Bytes);
    auto attribute = schema->addSchema(Kind::Object);
    auto children = schema->addSchema(Kind::Array);
    schema->setJsonSchemaAnnotations(count, {{"minimum", 0}, {"maximum", 255}});
    schema->setJsonSchemaAnnotations(
        label,
        {{"pattern", "^[a-z]+$"}, {"description", "Lower-case label"}});
    schema->setJsonSchemaAnnotations(
        root,
        {{"title", "Carrier"}, {"x-mapget", {{"layerTarget", "example.Carrier"}}}});
    schema->addNamedValue(
        flags,
        {.name = "LEFT", .value = uint64_t{1}, .role = simfil::Schema::NamedValue::Role::Flag});
    schema->addNamedValue(
        flags,
        {.name = "RIGHT", .value = uint64_t{2}, .role = simfil::Schema::NamedValue::Role::Flag});
    schema->addEnumSymbols(phase, std::vector<std::string>{"WAITING", "ACTIVE", "DONE"});
    schema->addFieldSchema(attribute, "label", label);
    schema->setFieldRequired(attribute, "label", true);
    schema->addFieldSchema(root, "id", count, true);
    schema->addFieldSchema(root, "attribute", attribute, true);
    schema->addFieldSchema(root, "variant", attribute, true);
    schema->addFieldSchema(root, "variant", count, true);
    schema->addFieldSchema(root, "groups", children, true);
    schema->addFieldSchema(root, "flags", flags);
    schema->addFieldSchema(root, "phase", phase);
    schema->addFieldSchema(root, "bytes", bytes);
    schema->addFieldSchema(root, "children", children);
    schema->addElementSchema(children, root);
    schema->setFieldRequired(root, "id", true);
    schema->finalize();

    auto exported = schema->toJsonSchema();
    auto restored = importTestSchema(
        exported,
        {schema->featureTypeInfos().begin(), schema->featureTypeInfos().end()});
    // The codec exports from the immutable graph; there is no retained transport JSON cache.
    REQUIRE(restored->toJsonSchema() == exported);
    REQUIRE(describe(restored, root) == describe(schema, root));
    REQUIRE(restored->childSchema(root, "attribute") == attribute);
    REQUIRE(restored->kind(flags) == LayerSchema::BitmaskKind);
    REQUIRE(restored->kind(bytes) == Kind::Bytes);
    REQUIRE(
        std::ranges::find(restored->directFields(root), "_multimap") ==
        restored->directFields(root).end());
    auto const& namedFlags =
        exported["definitions"][std::to_string(flags)]["x-mapget"]["namedValues"];
    REQUIRE(namedFlags.size() == 2);
    REQUIRE(namedFlags[0]["name"] == "LEFT");
    REQUIRE(namedFlags[0]["value"] == 1);
    REQUIRE(namedFlags[0]["role"] == "flag");
    REQUIRE(namedFlags[1]["name"] == "RIGHT");
    REQUIRE(namedFlags[1]["value"] == 2);
    REQUIRE(namedFlags[1]["role"] == "flag");
    REQUIRE_FALSE(exported["definitions"][std::to_string(flags)].contains("enum"));

    nlohmann::json_schema::json_validator validator;
    REQUIRE_NOTHROW(validator.set_root_schema(exported));
    auto projected = R"({"id":2,"attribute":{"label":"road"},
        "bytes":{"_bytes":true,"hex":"abcd","number":43981}})"_json;
    projected["flags"] = simfil::Bitmask{3, {schema->mapSchema()->binding(), flags}}.toJson();
    REQUIRE_NOTHROW(validator.validate(projected));
    projected["flags"] = "LEFT|RIGHT";
    REQUIRE_THROWS(validator.validate(projected));
    REQUIRE_NOTHROW(validator.validate(
        R"({"id":[2],"attribute":[{"label":"road"},{"label":"lane"}],"_multimap":true})"_json));
    REQUIRE_NOTHROW(
        validator.validate(R"({"id":1,"children":[{"id":2,"children":[{"id":3}]}]})"_json));
    // Empty arrays match both JSON projections; they must not be rejected by oneOf.
    REQUIRE_NOTHROW(validator.validate(R"({"id":1,"groups":[],"variant":2})"_json));
    REQUIRE_NOTHROW(validator.validate(
        R"({"id":1,"groups":[[{"id":2}],[]],"variant":[2,{"label":"road"}]})"_json));
    REQUIRE_THROWS(validator.validate(R"({"id":1,"children":[{"id":256}]})"_json));
    REQUIRE_THROWS(validator.validate(R"({"id":256})"_json));
    REQUIRE_THROWS(validator.validate(R"({"id":2,"attribute":{"label":"UPPER"}})"_json));
    REQUIRE_THROWS(validator.validate(R"({"id":2,"bytes":"abcd"})"_json));
    REQUIRE_THROWS(schema->setJsonSchemaAnnotations(root, {{"type", "integer"}}));
    REQUIRE_THROWS(schema->setJsonSchemaAnnotations(root, {{"x-mapget", {{"kind", "attribute"}}}}));
}

TEST_CASE(
    "Native schema transport preserves explicit alias ownership",
    "[DataSourceInfo][schema-domain]")
{
    auto schema = newTestSchema();
    auto root = schema->addSchema(Kind::Object);
    schema->setRootSchema(root);
    auto first = schema->addSchema(Kind::Object, "shared-context-key");
    auto second = schema->addSchema(Kind::Object);
    REQUIRE_THROWS(schema->registerSchemaKey("shared-context-key", second));
    schema->registerSchemaKey("second-context", second);
    // Reference traversal reaches the second context first, unlike producer allocation.
    schema->addFieldSchema(root, "a", second);
    schema->addFieldSchema(root, "b", first);
    schema->finalize();

    auto const transport = schema->toJsonSchema();
    auto restored = importTestSchema(
        transport,
        {schema->featureTypeInfos().begin(), schema->featureTypeInfos().end()});
    REQUIRE(restored->schemaId("shared-context-key") == schema->schemaId("shared-context-key"));
    REQUIRE(restored->schemaId("second-context") == second);
    REQUIRE(restored->toJsonSchema() == transport);
}

TEST_CASE(
    "Converter fragments share native domains but keep local references isolated",
    "[schema-domain]")
{
    auto schema = newTestSchema();
    auto shared = schema->addSchema(Kind::String, "shared");
    auto root = schema->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    schema->addFeatureType(testFeatureType("Carrier"), root);
    auto fragment = R"({"type":"object","additionalProperties":false,"properties":{
        "value":{"$ref":"#/$defs/Value"},"shared":{"$ref":"shared"}},
        "$defs":{"Value":{"type":"integer","minimum":0,"maximum":7}}})"_json;
    auto first = schema->addJsonSchema(fragment);
    fragment["$defs"]["Value"] = {{"type", "string"}, {"enum", {"ON", "OFF"}}};
    auto second = schema->addJsonSchema(fragment);
    schema->addFieldSchema(root, "first", first);
    schema->addFieldSchema(root, "second", second);
    schema->finalize();
    REQUIRE(schema->childSchema(first, "shared") == shared);
    REQUIRE(schema->childSchema(second, "shared") == shared);
    REQUIRE(schema->kind(schema->childSchema(first, "value")) == Kind::Int);
    REQUIRE(schema->kind(schema->childSchema(second, "value")) == Kind::String);
    auto exported = schema->toJsonSchema();
    auto restored = importTestSchema(
        exported,
        {schema->featureTypeInfos().begin(), schema->featureTypeInfos().end()});
    REQUIRE(restored->toJsonSchema() == exported);
    nlohmann::json_schema::json_validator validator;
    REQUIRE_NOTHROW(validator.set_root_schema(exported));
    REQUIRE_NOTHROW(validator.validate(R"({"first":{"value":3},"second":{"value":"ON"}})"_json));
    REQUIRE_THROWS(validator.validate(R"({"first":{"value":8}})"_json));
}

TEST_CASE(
    "Schema snapshots share frozen owners and replacement preserves imported validation",
    "[schema-domain]")
{
    auto imported = importTestSchema(R"({"type":"object","if":{"required":["mode"]},
        "then":{"required":["value"]},"properties":{"mode":{"type":"string"}}})"_json);
    REQUIRE(roundtripSchema(imported)->toJsonSchema() == imported->toJsonSchema());
    auto native = newTestSchema();
    auto root = native->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    native->addFeatureType(testFeatureType("Carrier"), root);
    native->finalize();
    auto snapshot = native;
    auto before = native->toJsonSchema();
    REQUIRE(snapshot->mapSchema() == native->mapSchema());
    REQUIRE_THROWS(native->addSchema(Kind::Int));
    auto replacement = newTestSchema();
    auto replacementRoot =
        replacement->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    replacement->addFeatureType(testFeatureType("Carrier"), replacementRoot);
    replacement->addFieldSchema(replacementRoot, "added", replacement->addSchema(Kind::Int));
    replacement->finalize();
    REQUIRE(replacement->toJsonSchema() != before);
    REQUIRE(replacement->mapSchema() != snapshot->mapSchema());
    REQUIRE(snapshot->toJsonSchema() == before);
}

TEST_CASE(
    "LayerSchema retains typed JSON domains rather than flattening combiners",
    "[DataSourceInfo][schema-domain]")
{
    auto schema = importTestSchema(
        R"({
        "type":"object", "additionalProperties":false,
        "x-mapget":{"metaType":"Feature", "featureType":"Thing"},
        "required":["items"],
        "properties":{
            "items":{"type":"array","items":{"anyOf":[
                {"type":"object","additionalProperties":false,"properties":{"name":{"type":"string","enum":["urban","rural"]}}},
                {"type":"integer","enum":[1,2]}
            ]}},
            "choice":{"oneOf":[{"type":"string"},{"type":"integer"}]},
            "both":{"allOf":[{"type":"object","properties":{"left":{"type":"boolean"}}},{"type":"object","properties":{"right":{"type":"integer"}}}]},
            "optional":{"type":["string","null"]},
            "nullableObject":{"type":["object","null"],"properties":{"name":{"type":"string"}}},
            "anything":true, "nothing":false, "unknown":{},
            "number":{"type":"number","enum":[2.5,3.5]},
            "bool":{"const":true}
        }
    })"_json,
        {testFeatureType("Thing")});
    auto root = schema->featureSchema("Thing");
    REQUIRE(root != simfil::NoTypeId);
    REQUIRE(schema->kind(root) == LayerSchema::FeatureKind);
    REQUIRE(schema->typeName(root) == "Thing");
    REQUIRE(schema->fieldRequired(root, "items") == true);
    REQUIRE(schema->fieldRequired(root, "optional") == false);
    REQUIRE_FALSE(schema->reachabilityComplete(root));
    REQUIRE(schema->canHaveField(root, "undeclaredBelowUnknown"));
    auto json = describe(schema, root);
    REQUIRE(json["kind"] == "Feature");
    REQUIRE(json["fields"]["items"]["required"] == true);
    REQUIRE(json["fields"]["items"]["elements"][0]["kind"] == "union");
    REQUIRE(json["fields"]["items"]["elements"][0]["alternatives"].size() == 2);
    REQUIRE(json["fields"]["choice"]["kind"] == "oneOf");
    REQUIRE(json["fields"]["both"]["kind"] == "intersection");
    REQUIRE(json["fields"]["optional"]["nullable"] == true);
    REQUIRE(json["fields"]["nullableObject"]["alternatives"][1]["kind"] == "null");
    REQUIRE(json["fields"]["anything"]["kind"] == "any");
    REQUIRE(json["fields"]["nothing"]["kind"] == "never");
    REQUIRE(json["fields"]["unknown"]["kind"] == "unknown");
    REQUIRE(json["fields"]["bool"]["enum"] == nlohmann::json::array({true}));
    REQUIRE(json["fields"]["number"]["enum"] == nlohmann::json::array({2.5, 3.5}));
    auto strings = std::make_shared<StringPool>("SchemaDomainCompletion");
    auto env = makeEnvironment(strings);
    installCompletionLayerSchema(*env, schema, strings);
    REQUIRE(suggests(*env, root, "items[17].na", "name"));
    REQUIRE(suggests(*env, root, "items[17].name == ru", "\"rural\""));
    REQUIRE(suggests(*env, root, "both.ri", "right"));
    auto roundtrip = roundtripSchema(schema);
    REQUIRE(roundtrip->featureSchema("Thing") == root);
    REQUIRE(describe(roundtrip, root) == json);
}

TEST_CASE(
    "Direct rich LayerSchema transport preserves IDs metadata and plural edges",
    "[DataSourceInfo][schema-domain]")
{
    auto schema = newTestSchema();
    auto root = schema->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    schema->addFeatureType(testFeatureType("Carrier"), root);
    auto number = schema->addSchema(Kind::Int, "count");
    auto text = schema->addSchema(Kind::String, "text");
    auto array = schema->addSchema(Kind::Array, "list");
    auto unionId = schema->addSchema(Kind::Union, "choice");
    auto never = schema->addSchema(Kind::Never, "none");
    auto unknown = schema->addSchema(Kind::Unknown, "unknown");
    auto bytes = schema->addSchema(Kind::Bytes, "bytes");
    schema->setTypeName(root, "ConcreteCarrier");
    schema->setTypeName(text, "Label");
    schema->setNullable(text, true);
    schema->setNullable(number, false);
    schema->addEnumSymbol(text, "FIRST");
    schema->addEnumValue(number, int64_t{4});
    schema->addEnumValue(bytes, simfil::ByteArray{"blob"});
    schema->setComposition(unionId, Composition::OneOf);
    schema->addAlternative(unionId, number);
    schema->addAlternative(unionId, text);
    schema->addElementSchema(array, number);
    schema->addElementSchema(array, text);
    schema->addFieldSchema(root, "list", array);
    schema->addFieldSchema(root, "many", number);
    schema->addFieldSchema(root, "many", text);
    schema->addFieldSchema(root, "odd.name", unionId);
    schema->addFieldSchema(root, "none", never);
    schema->addFieldSchema(root, "unknown", unknown);
    schema->addFieldSchema(root, "bytes", bytes);
    schema->addFieldSchema(root, "unspecified");
    schema->setFieldRequired(root, "list", true);
    schema->setFieldRequired(root, "many", false);
    schema->registerSchemaKey("textAlias", text);
    schema->finalize();
    auto json = schema->toJsonSchema();
    auto roundtrip = importTestSchema(
        json,
        {schema->featureTypeInfos().begin(), schema->featureTypeInfos().end()});
    REQUIRE(roundtrip->featureSchema("Carrier") == root);
    REQUIRE(roundtrip->schemaId("textAlias") == text);
    REQUIRE(roundtrip->kind(number) == schema->kind(number));
    REQUIRE(roundtrip->kind(text) == schema->kind(text));
    REQUIRE(roundtrip->kind(never) == Kind::Never);
    REQUIRE(roundtrip->kind(unknown) == Kind::Unknown);
    REQUIRE(roundtrip->enumValues(bytes).size() == 1);
    REQUIRE(std::get<simfil::ByteArray>(roundtrip->enumValues(bytes)[0]).bytes == "blob");
    REQUIRE(describe(roundtrip, root) == describe(schema, root));
    REQUIRE(roundtrip->toJsonSchema() == json);

    auto invalid = json;
    invalid["definitions"]["bad"] = invalid["definitions"]["1"];
    REQUIRE_THROWS(importTestSchema(
        invalid,
        {schema->featureTypeInfos().begin(), schema->featureTypeInfos().end()}));
}

TEST_CASE(
    "Attribute query roots preserve payload aliases and exact runtime overlays",
    "[DataSourceInfo][schema-domain]")
{
    auto schema = newTestSchema();
    auto feature = schema->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    schema->addFeatureType(testFeatureType("Carrier"), feature);
    auto attribute = schema->addSchema(Kind::Object, "speed", "Attribute");
    auto value = schema->addSchema(Kind::Int);
    schema->addFieldSchema(feature, "speed", attribute);
    schema->addFieldSchema(feature, "identifier", value);
    schema->addFieldSchema(attribute, "value", value);
    schema->setAttributeMetadata(
        attribute,
        {"Carrier", "rules", "SPEED", attribute},
        "SpeedAttribute");
    schema->finalize();
    auto root = schema->attributeQuerySchema("Carrier", attribute);
    REQUIRE(root != simfil::NoTypeId);
    REQUIRE(root != attribute);
    REQUIRE(schema->childSchema(root, "$feature") == feature);
    auto transport = schema->toJsonSchema();
    REQUIRE(schema->mapSchema()->finalized());
    REQUIRE(schema->toJsonSchema() == transport);
    REQUIRE(schema->attributeQuerySchema("Carrier", attribute) == root);
    REQUIRE(schema->attributeQuerySchema("Missing", attribute) == simfil::NoTypeId);
    auto json = describe(schema, root);
    REQUIRE(json["kind"] == "Attribute");
    REQUIRE(json["typename"] == "SpeedAttribute");
    REQUIRE(json["fields"]["$attributeIndex"]["kind"] == "integer");
    REQUIRE(json["fields"]["$validityCount"]["kind"] == "integer");
    REQUIRE(json["fields"]["$hasValidity"]["kind"] == "boolean");
    REQUIRE(json["fields"]["$name"]["kind"] == "string");
    auto sourceStrings = std::make_shared<StringPool>("Authoritative");
    auto highest = sourceStrings->highest();
    auto strings = std::make_shared<StringPool>("Completion");
    auto env = makeEnvironment(strings);
    installCompletionLayerSchema(*env, schema, strings);
    REQUIRE(suggests(*env, root, "$feature.id", "identifier"));
    REQUIRE(suggests(*env, root, "val", "value"));
    auto ast = simfil::compile(
        *env,
        "SPEED == 4",
        simfil::CompileOptions{
            .any = false,
            .rewriteMode = simfil::RewriteMode::Schema,
            .rootSchema = root});
    REQUIRE(ast);
    REQUIRE((*ast)->expr().toString().find("value") != std::string::npos);
    REQUIRE(sourceStrings->highest() == highest);
    auto restored = roundtripSchema(schema);
    REQUIRE(restored->attributeQuerySchema("Carrier", attribute) != simfil::NoTypeId);
    REQUIRE(describe(restored, restored->attributeQuerySchema("Carrier", attribute)) == json);
}

TEST_CASE(
    "Shared Attribute query names select use sites within the current feature and map layer",
    "[schema-domain][schema-native][attribute-query-symbols]")
{
    auto graph = std::make_shared<MapSchema>();
    auto schema = graph->newLayer("Test", LayerType::Features);
    auto attribute = graph->newAttribute("shared-speed", "SpeedAttribute");
    schema->addFieldSchema(attribute, "value", graph->primitive(Kind::Int));
    for (std::string feature : {"Carrier", "Other"}) {
        REQUIRE(schema->newFeature(testFeatureType(feature, "identifier")) != simfil::NoTypeId);
        auto container = schema->addSchema(Kind::Object, {}, "AttributeContainer");
        schema->addFieldSchema(schema->attributeLayerMapSchema(feature), "rules", container, true);
        auto names = feature == "Carrier" ?
            std::vector<std::string>{"SPEED", "ADVISORY_SPEED"} :
            std::vector<std::string>{"OTHER_SPEED"};
        for (auto const& name : names) {
            schema->bindAttribute(attribute, feature, "rules", name);
            schema->addFieldSchema(container, name, attribute, true);
        }
    }
    auto otherLayer = graph->newLayer("OtherLayer", LayerType::Features);
    REQUIRE(otherLayer->newFeature(testFeatureType("Carrier", "identifier")) != simfil::NoTypeId);
    auto container = otherLayer->addSchema(Kind::Object, {}, "AttributeContainer");
    otherLayer
        ->addFieldSchema(otherLayer->attributeLayerMapSchema("Carrier"), "rules", container, true);
    otherLayer->bindAttribute(attribute, "Carrier", "rules", "LAYER_SPEED");
    otherLayer->addFieldSchema(container, "LAYER_SPEED", attribute, true);
    graph->finalize();
    CHECK(schema->attributeTypeCode(attribute).empty());
    std::ostringstream binary(std::ios::binary);
    REQUIRE(graph->write(binary));
    auto payload = binary.str();
    auto decoded =
        MapSchema::read({reinterpret_cast<uint8_t const*>(payload.data()), payload.size()});
    for (auto const& registry :
         {schema,
          otherLayer,
          decoded->layer("Test"),
          decoded->layer("OtherLayer"),
          roundtripSchema(schema),
          roundtripSchema(otherLayer)})
    {
        CAPTURE(registry->name());
        auto physical = registry->schemaId("shared-speed");
        REQUIRE(physical != simfil::NoTypeId);
        auto strings = std::make_shared<StringPool>("SharedAttributeQueryNames");
        auto env = makeEnvironment(strings);
        installCompletionLayerSchema(*env, registry, strings);
        auto const& lookup = env->querySchemaCallback;
        for (std::string feature : {"Carrier", "Other"}) {
            if (!registry->producesFeatureType(feature))
                continue;
            auto root = registry->attributeQuerySchema(feature, physical);
            REQUIRE(root != simfil::NoTypeId);
            for (auto const& owner : registry->attributeOwners(root)) {
                CHECK(owner.featureType_ == feature);
                CHECK(owner.featureLayer_ == registry->name());
            }
            REQUIRE(lookup(root)->symbolEqualityPaths(simfil::StringPool::Empty, lookup).empty());
            for (std::string symbol :
                 {"SPEED", "ADVISORY_SPEED", "OTHER_SPEED", "LAYER_SPEED", "MISSING"}) {
                CAPTURE(feature, symbol);
                auto symbolId = strings->emplace(symbol).value();
                CHECK(lookup(physical)->symbolEqualityPaths(symbolId, lookup).empty());
                CHECK(lookup(registry->featureSchema(feature))
                          ->symbolEqualityPaths(symbolId, lookup)
                          .empty());
                auto paths = lookup(root)->symbolEqualityPaths(symbolId, lookup);
                auto allowed = registry->name() == "OtherLayer" ?
                    symbol == "LAYER_SPEED" :
                    feature == "Carrier" ?
                    symbol == "SPEED" || symbol == "ADVISORY_SPEED" :
                    symbol == "OTHER_SPEED";
                if (!allowed) {
                    // Neither another feature nor a same-named feature in another
                    // map layer contributes names to this query context.
                    CHECK(paths.empty());
                    continue;
                }
                REQUIRE(paths.size() == 1);
                REQUIRE(paths.front().size() == 1);
                CHECK(paths.front().front().field == strings->get("$name"));
                auto scalarPaths = lookup(root)->scalarFieldPathsForSymbol(symbolId, lookup);
                CHECK(scalarPaths.empty() == (registry->name() == "Test" && feature == "Carrier"));
                auto ast = simfil::compile(
                    *env,
                    symbol,
                    {.any = false, .rewriteMode = simfil::RewriteMode::Schema, .rootSchema = root});
                REQUIRE(ast);
                for (std::string actual :
                     {"SPEED", "ADVISORY_SPEED", "OTHER_SPEED", "LAYER_SPEED", "MISSING"}) {
                    CAPTURE(actual);
                    auto data = std::make_shared<simfil::ModelPool>(strings);
                    data->setSchemaBinding(registry->mapSchema()->binding());
                    auto object = data->newObject();
                    REQUIRE(object->setSchema(root));
                    REQUIRE(object->addField("$name", std::string_view(actual)));
                    REQUIRE(object->addField("value", int64_t{42}));
                    auto values = simfil::eval(*env, **ast, *object, nullptr);
                    REQUIRE(values);
                    REQUIRE(values->size() == 1);
                    CHECK(values->front().as<simfil::ValueType::Bool>() == (actual == symbol));
                }
            }
        }
    }
}

TEST_CASE(
    "Shared Attribute scalar shorthand requires an explicit owner guard",
    "[schema-domain][attribute-query-symbols]")
{
    auto graph = std::make_shared<MapSchema>();
    auto schema = graph->newLayer("Test", LayerType::Features);
    REQUIRE(schema->newFeature(testFeatureType("Carrier", "identifier")) != simfil::NoTypeId);
    auto container = schema->addSchema(Kind::Object, {}, "AttributeContainer");
    schema->addFieldSchema(schema->attributeLayerMapSchema("Carrier"), "rules", container, true);
    auto attribute = graph->newAttribute("shared-speed");
    schema->addFieldSchema(attribute, "value", graph->primitive(Kind::Int));
    for (auto name : {"SPEED", "ADVISORY_SPEED"}) {
        schema->bindAttribute(attribute, "Carrier", "rules", name);
        schema->addFieldSchema(container, name, attribute, true);
    }
    graph->finalize();
    auto root = schema->attributeQuerySchema("Carrier", attribute);
    REQUIRE(root != simfil::NoTypeId);
    auto strings = std::make_shared<StringPool>("SharedAttributeScalarAliases");
    auto env = makeEnvironment(strings);
    installCompletionLayerSchema(*env, schema, strings);
    auto options = simfil::CompileOptions{
        .any = true,
        .rewriteMode = simfil::RewriteMode::Schema,
        .rootSchema = root};
    auto ambiguous = simfil::compile(*env, "SPEED > 40", options);
    REQUIRE(ambiguous);
    auto references = simfil::referencedSchemaPaths(*env, **ambiguous, root);
    REQUIRE(references);
    CHECK(references->hasUnresolvedAccess);
    auto explicitField = simfil::compile(*env, "value > 40", options);
    REQUIRE(explicitField);

    auto normalized =
        schema->normalizeSearchQuery("SPEED > 40", LayerSchema::SearchQueryRequestedScope::Auto);
    REQUIRE(normalized);
    CHECK(normalized->concreteScope_ == LayerSchema::SearchQueryConcreteScope::Attribute);
    REQUIRE(normalized->attributeScopes_.size() == 1);
    CHECK(normalized->attributeScopes_.front().attributeName_ == "SPEED");
    auto guarded = simfil::compile(*env, normalized->normalizedQuery_, options);
    REQUIRE(guarded);
    auto guardedReferences = simfil::referencedSchemaPaths(*env, **guarded, root);
    REQUIRE(guardedReferences);
    CHECK_FALSE(guardedReferences->hasUnresolvedAccess);
    for (std::string actual : {"SPEED", "ADVISORY_SPEED"}) {
        CAPTURE(actual, normalized->normalizedQuery_);
        auto data = std::make_shared<simfil::ModelPool>(strings);
        data->setSchemaBinding(schema->mapSchema()->binding());
        auto object = data->newObject();
        REQUIRE(object->setSchema(root));
        REQUIRE(object->addField("$name", std::string_view(actual)));
        REQUIRE(object->addField("$layer", std::string_view("rules")));
        REQUIRE(object->addField("value", int64_t{42}));
        auto feature = data->newObject();
        REQUIRE(feature->addField("typeId", std::string_view("Carrier")));
        REQUIRE(object->addField("$feature", feature));
        auto unguarded = simfil::eval(*env, **ambiguous, *object, nullptr);
        REQUIRE(unguarded);
        REQUIRE(unguarded->size() == 1);
        CHECK_FALSE(unguarded->front().as<simfil::ValueType::Bool>());
        auto explicitResult = simfil::eval(*env, **explicitField, *object, nullptr);
        REQUIRE(explicitResult);
        REQUIRE(explicitResult->size() == 1);
        CHECK(explicitResult->front().as<simfil::ValueType::Bool>());
        auto guardedResult = simfil::eval(*env, **guarded, *object, nullptr);
        REQUIRE(guardedResult);
        REQUIRE(guardedResult->size() == 1);
        CHECK(guardedResult->front().as<simfil::ValueType::Bool>() == (actual == "SPEED"));
    }
}

TEST_CASE(
    "Attribute payload fields cannot impersonate a query context",
    "[schema-domain][attribute-query-symbols]")
{
    auto graph = std::make_shared<MapSchema>();
    auto schema = graph->newLayer("Test", LayerType::Features);
    auto feature = schema->newFeature(testFeatureType("Carrier", "identifier"));
    auto attribute = graph->newAttribute("payload");
    schema->bindAttribute(attribute, "Carrier", "rules", "SPEED");
    schema->addFieldSchema(schema->featurePropertiesSchema("Carrier"), "payload", attribute);
    schema->addFieldSchema(attribute, "$name", graph->primitive(Kind::String));
    schema->addFieldSchema(attribute, "$feature", feature);
    graph->finalize();
    auto strings = std::make_shared<StringPool>("AttributePayloadQueryLookalike");
    auto env = makeEnvironment(strings);
    installCompletionLayerSchema(*env, schema, strings);
    auto const& lookup = env->querySchemaCallback;
    auto symbol = strings->emplace("SPEED").value();
    CHECK(schema->attributeTypeCode(attribute) == "SPEED");
    CHECK(schema->childSchema(attribute, "$feature") == feature);
    CHECK(lookup(attribute)->symbolEqualityPaths(symbol, lookup).empty());
    auto query = schema->attributeQuerySchema("Carrier", attribute);
    REQUIRE(query != simfil::NoTypeId);
    auto paths = lookup(query)->symbolEqualityPaths(symbol, lookup);
    REQUIRE(paths.size() == 1);
    REQUIRE(paths.front().size() == 1);
    CHECK(paths.front().front().field == strings->get("$name"));
}

TEST_CASE("Schema summaries preserve cycles across immutable contract variants", "[schema-domain]")
{
    auto variant = GENERATE(0, 1, 2);
    auto schema = newTestSchema();
    auto first = schema->addSchema(Kind::Object);
    schema->setRootSchema(first);
    auto second = schema->addSchema(Kind::Object);
    auto array = schema->addSchema(Kind::Array);
    auto choice = schema->addSchema(Kind::Union);
    auto value = schema->addSchema(Kind::String);
    schema->addFieldSchema(first, "next", second);
    schema->addFieldSchema(first, "alias", array);
    schema->addFieldSchema(second, "items", array);
    schema->addFieldSchema(second, "state", value);
    schema->addElementSchema(array, choice);
    schema->setComposition(choice, Composition::AnyOf);
    schema->addAlternative(choice, first);
    schema->addAlternative(choice, value);
    schema->addEnumSymbols(value, std::vector<std::string>{"ON", "OFF"});
    if (variant != 0) {
        schema->addFieldSchema(second, "late", value);
        schema->addEnumSymbol(value, "LATE");
    }
    schema->setOpen(second, variant == 1);
    schema->finalize();

    for (auto id : {first, second, array, choice}) {
        CAPTURE(id);
        REQUIRE(std::ranges::equal(
            schema->nestedFields(id),
            variant == 0 ? std::vector<std::string>{"alias", "items", "next", "state"} :
                           std::vector<std::string>{"alias", "items", "late", "next", "state"}));
        REQUIRE(std::ranges::equal(
            schema->nestedEnumSymbols(id),
            variant == 0 ? std::vector<std::string>{"OFF", "ON"} :
                           std::vector<std::string>{"LATE", "OFF", "ON"}));
        REQUIRE(schema->reachabilityComplete(id) == (variant != 1));
        REQUIRE(schema->canHaveField(id, "absent") == (variant == 1));
    }
    REQUIRE(schema->nestedFields(value).empty());

    // A published completeness proof cannot be invalidated through a retained builder alias.
    REQUIRE_THROWS(schema->setOpen(second, false));
}

TEST_CASE("Many attribute query roots retain the shared feature schema", "[schema-domain]")
{
    auto partial = GENERATE(false, true);
    auto schema = newTestSchema();
    auto feature = schema->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    schema->addFeatureType(testFeatureType("Carrier"), feature);
    auto shared = schema->addSchema(Kind::Object);
    auto parent = shared;
    for (size_t i = 0; i < 128; ++i) {
        auto child = schema->addSchema(Kind::Object);
        schema->addFieldSchema(parent, "left", child);
        schema->addFieldSchema(parent, "right", child);
        parent = child;
    }
    auto value = schema->addSchema(Kind::String);
    schema->addEnumSymbol(value, "SHARED_VALUE");
    schema->addFieldSchema(parent, "value", value);
    std::vector<simfil::TypeId> attributes;
    for (size_t i = 0; i < 400; ++i) {
        auto name = "attribute" + std::to_string(i);
        auto attribute = schema->addSchema(Kind::Object, {}, "Attribute");
        schema->addFieldSchema(attribute, "payload", shared);
        schema->addFieldSchema(feature, name, attribute);
        schema->setAttributeMetadata(
            attribute,
            {"Carrier", "rules", name, attribute},
            "SharedAttribute");
        attributes.push_back(attribute);
    }
    if (partial)
        schema->addFieldSchema(parent, "unknown");
    schema->finalize();
    for (auto attribute : attributes) {
        auto root = schema->attributeQuerySchema("Carrier", attribute);
        REQUIRE(root != simfil::NoTypeId);
        REQUIRE(schema->childSchema(root, "$feature") == feature);
        // $feature is an explicit context edge, not an implicit wildcard subtree.
        // Incomplete payload metadata must still leave absence proofs conservative.
        REQUIRE(schema->canHaveField(root, "attribute399") == partial);
        REQUIRE(schema->canHaveField(schema->childSchema(root, "$feature"), "attribute399"));
        REQUIRE(schema->canHaveField(root, "value"));
        REQUIRE(schema->canHaveEnumSymbol(root, "SHARED_VALUE"));
        REQUIRE(schema->reachabilityComplete(root) == !partial);
        REQUIRE(schema->canHaveField(root, "absent") == partial);
    }
    if (!partial)
        REQUIRE_FALSE(schema->canHaveField(feature, "$feature"));
}

TEST_CASE(
    "Recursive domains and multimap transport remain logical model views",
    "[DataSourceInfo][schema-domain]")
{
    auto schema = importTestSchema(
        R"({
        "$ref":"#/$defs/Root", "$defs":{"Root":{
            "type":"object", "additionalProperties":false,
            "x-mapget":{"metaType":"Feature", "featureType":"Recursive"},
            "properties":{
                "next":{"$ref":"#/$defs/Root"},
                "state":{"type":"string","enum":["DONE"]},
                "multi":{"x-mapget-multimap":true,"oneOf":[
                    {"type":"object","additionalProperties":false,"properties":{"label":{"type":"string"}}},
                    {"type":"array","items":{"type":"object"}}
                ]}
            }
        }}
    })"_json,
        {testFeatureType("Recursive")});
    auto root = schema->featureSchema("Recursive");
    REQUIRE(schema->reachabilityComplete(root));
    REQUIRE_FALSE(schema->canHaveField(root, "absent"));
    auto multi = schema->childSchema(root, "multi");
    REQUIRE(schema->kind(multi) == Kind::Object);
    REQUIRE(schema->composition(multi) == Composition::None);
    auto json = describe(schema, root);
    REQUIRE(json["fields"]["next"]["truncated"] == "cycle");
    auto strings = std::make_shared<StringPool>("RecursiveCompletion");
    auto env = makeEnvironment(strings);
    installCompletionLayerSchema(*env, schema, strings);
    REQUIRE(suggests(*env, root, "next.next.next.multi.la", "label"));
    auto ast = simfil::compile(
        *env,
        "DONE",
        simfil::CompileOptions{
            .any = false,
            .rewriteMode = simfil::RewriteMode::Schema,
            .rootSchema = root});
    REQUIRE(ast);
    auto data = std::make_shared<simfil::ModelPool>(strings);
    REQUIRE(simfil::json::parse(R"({"next":{"next":{"state":"DONE"}}})", data));
    auto values = simfil::eval(*env, **ast, *data->root(0).value(), nullptr);
    REQUIRE(values);
    REQUIRE_FALSE(values->empty());
}
