#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json-schema.hpp>
#include <ranges>
#include <sstream>
#include <tuple>
#include <utility>

#include "mapget/model/featurelayer.h"
#include "mapget/model/layerschema.h"
#include "mapget/model/mapschema.h"
#include "mapget/model/simfilutil.h"
#include "simfil/diagnostics.h"
#include "simfil/model/json.h"
#include "simfil/model/schema-node.h"
#include "simfil/simfil.h"

using namespace mapget;
using Kind = simfil::SchemaType::Kind;
using Composition = simfil::SchemaType::Composition;

namespace
{

/** Keep the writable graph explicit while the layer scopes production metadata. */
std::pair<std::shared_ptr<MapSchema>, LayerSchema::Ptr> newTestSchema()
{
    auto owner = std::make_shared<MapSchema>();
    return {owner, owner->newLayer("Test", LayerType::Features)};
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

/** Materialize declarations only for the legacy JSON import boundary. */
std::vector<FeatureTypeInfo> identityDeclarations(LayerSchema::Ptr const& schema)
{
    std::vector<FeatureTypeInfo> declarations;
    for (auto identity : schema->mapSchema()->featureIdentities())
        declarations.push_back(identity.toDeclaration());
    return declarations;
}

/** Exercise the codec with the same schema-owned identity declarations. */
LayerSchema::Ptr roundtripSchema(LayerSchema::Ptr const& schema)
{
    return importTestSchema(
        schema->toJsonSchema(),
        identityDeclarations(schema),
        std::string(schema->name()));
}

/** Read a bounded descriptor directly from the canonical type, without a feature-data schema. */
nlohmann::json describe(LayerSchema::Ptr registry, simfil::TypeId root)
{
    return registry->mapSchema()->schemaType(root).modelNode()->toJson();
}

/** Compare type contracts independently of their current JSON source document.
 * Only descriptor nodes lose their location; literal metadata remains untouched. */
nlohmann::json withoutSourceLocations(nlohmann::json descriptor)
{
    descriptor.erase("jsonPointer");
    if (auto fields = descriptor.find("fields"); fields != descriptor.end())
        for (auto& [name, field] : fields->items())
            field = withoutSourceLocations(std::move(field));
    for (auto name : {"elements", "alternatives"})
        if (auto children = descriptor.find(name); children != descriptor.end())
            for (auto& child : *children)
                child = withoutSourceLocations(std::move(child));
    return descriptor;
}

/** Check owning candidate text without depending on incidental suggestion ordering. */
bool suggests(
    simfil::Environment& env,
    simfil::TypeId root,
    std::string_view query,
    std::string_view text)
{
    auto candidates =
        simfil::complete(env, query, query.size(), env.schema().pool()->schemaType(root));
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
            auto shape = graph->schemaType(position);
            REQUIRE(shape.composition() == Composition::AnyOf);
            REQUIRE(shape.reachabilityComplete());
            CHECK_FALSE(shape.canHaveField(graph->dictionary().get("payload")));
            REQUIRE(shape.canHaveField(graph->dictionary().get("lon")));
            auto alternatives = shape.alternatives();
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
                        current = graph->schemaType(current).itemSchema();
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
                REQUIRE(graph->schemaType(bounds).kind() == Kind::Object);
                CHECK(layer->childSchema(bounds, "origin") == point);
                CHECK(layer->childSchema(bounds, "size") == point);
                CHECK_FALSE(
                    graph->schemaType(bounds).canHaveField(graph->dictionary().get("payload")));
                CHECK_NOTHROW(validator.validate(
                    R"({"type":"Polygon","aabb":{"origin":[11,48,0],"size":[1,2,3]}})"_json));
                CHECK_THROWS(
                    validator.validate(R"({"type":"Polygon","aabb":[[11,48,0],[1,2,3]]})"_json));
            }
            else {
                REQUIRE(alternatives.size() == 2);
                bool attribute = std::string_view(name) == "AttrPoint";
                CHECK(
                    graph->schemaType(alternatives.front()).kind() ==
                    (attribute ? Kind::String : Kind::Float));
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
            auto definition = graph->schemaType(point);
            REQUIRE(definition);
            CHECK(definition.kind() == Kind::Array);
            CHECK(definition.typeName() == "mapget::Point");
            REQUIRE(definition.directFields().size() == 3);
            for (auto field : {"lon", "lat", "elevation"}) {
                auto id = graph->strings()->get(field);
                CHECK(definition.fieldRole(id) == simfil::SchemaType::FieldRole::Context);
                CHECK(definition.fieldSchema(id) == definition.itemSchema());
            }
            size_t enumerated = 0;
            definition.forEachDirectField(
                [&](auto const& field)
                { enumerated += field.role() == simfil::SchemaType::FieldRole::Data; });
            CHECK(enumerated == 0);
            auto strings = std::make_shared<StringPool>("PointCompletion");
            auto env = makeEnvironment(strings);
            env->setSchema(layer->rootType());
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
    graph->schemaType(properties)
        .asObject()
        .addField("payload", graph->schemaType(graph->primitive(Kind::Int)));
    REQUIRE(
        layer->newFeature(testFeatureType("Carrier", "identifier"), graph->schemaType(properties))
            .id());
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
        env->setSchema(registry->rootType());
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
    REQUIRE(layer->newFeature(testFeatureType("Carrier", "identifier")).id());
    auto integer = graph->primitive(Kind::Int);
    auto left = graph->newAttribute("left", "LeftAttribute").id();
    auto right = graph->newAttribute("right", "RightAttribute").id();
    graph->schemaType(left).asObject().addField("leftValue", graph->schemaType(integer));
    graph->schemaType(right).asObject().addField("rightValue", graph->schemaType(integer));
    auto choice = layer->addSchema(Kind::Union, "attribute-choice");
    graph->schemaType(choice).setComposition(composition);
    graph->schemaType(choice).asComposition().addAlternative(graph->schemaType(left));
    graph->schemaType(choice).asComposition().addAlternative(graph->schemaType(right));
    auto kindName = simfil::SchemaType::kindNameId(graph->schemaType(choice).kind());
    auto affinities = simfil::SchemaType::affinities(layer->mapSchema()->schemaType(choice).kind());
    layer->bindAttribute(graph->schemaType(choice), "Carrier", "rules", "CHOICE");
    CHECK(
        simfil::SchemaType::affinities(layer->mapSchema()->schemaType(choice).kind()) ==
        affinities);
    CHECK(
        simfil::SchemaType::kindNameId(layer->mapSchema()->schemaType(choice).kind()) == kindName);

    auto mixed = layer->addSchema(Kind::Union, "mixed-choice");
    graph->schemaType(mixed).setComposition(composition);
    graph->schemaType(mixed).asComposition().addAlternative(graph->schemaType(left));
    graph->schemaType(mixed).asComposition().addAlternative(graph->schemaType(integer));
    auto mixedKind = layer->mapSchema()->schemaType(mixed).kind();
    REQUIRE_THROWS_AS(
        layer->bindAttribute(graph->schemaType(mixed), "Carrier", "rules", "MIXED"),
        std::invalid_argument);
    CHECK(layer->mapSchema()->schemaType(mixed).kind() == mixedKind);
    CHECK(layer->attributeOwners(mixed).empty());
    REQUIRE_NOTHROW(graph->finalize());
    // Finalization computes the aggregate's object affinity from its alternatives.
    affinities = simfil::SchemaType::affinities(layer->mapSchema()->schemaType(choice).kind());
    CHECK(affinities == simfil::valueTypeAffinity(simfil::ValueType::Object));
    std::ostringstream output;
    REQUIRE(graph->write(output));
    auto bytes = output.str();
    auto decoded = MapSchema::read({reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()});
    for (auto const& registry : {layer, decoded->layer("Test"), roundtripSchema(layer)}) {
        auto payload = registry->schemaId("attribute-choice");
        REQUIRE(payload != simfil::NoTypeId);
        CHECK(
            simfil::SchemaType::kindNameId(registry->mapSchema()->schemaType(payload).kind()) ==
            kindName);
        CHECK(
            simfil::SchemaType::affinities(registry->mapSchema()->schemaType(payload).kind()) ==
            affinities);
        CHECK(registry->mapSchema()->schemaType(payload).composition() == composition);
        REQUIRE(registry->mapSchema()->schemaType(payload).alternatives().size() == 2);
        auto query = registry->attributeQuerySchema("Carrier", payload);
        REQUIRE(query != simfil::NoTypeId);
        CHECK(registry->mapSchema()->schemaType(query).baseType().id() == payload);
        CHECK(registry->mapSchema()->schemaType(query).canHaveField(
            registry->mapSchema()->dictionary().get("leftValue")));
        CHECK(registry->mapSchema()->schemaType(query).canHaveField(
            registry->mapSchema()->dictionary().get("rightValue")));
        auto strings = std::make_shared<StringPool>("AttributeAggregate");
        auto env = makeEnvironment(strings);
        env->setSchema(registry->rootType());
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
    CHECK(layer->mapSchema()->schemaType(layer->rootSchema()).directFields().empty());
    CHECK(layer->childSchema(layer->rootSchema(), "ghost") == simfil::NoTypeId);
    CHECK_FALSE(layer->mapSchema()
                    ->schemaType(layer->rootSchema())
                    .canHaveField(layer->mapSchema()->dictionary().get("ghost")));
}

TEST_CASE(
    "Feature lookup aliases resolve canonical schema paths without duplicating enumeration",
    "[schema-domain]")
{
    auto [owner, schema] = newTestSchema();
    auto root = schema->addSchema(Kind::Object, LayerSchema::featureKey("Intersection"), "Feature");
    schema->addFeatureType(testFeatureType("Intersection"), root);
    auto properties = schema->addSchema(Kind::Object);
    auto roads = schema->addSchema(Kind::Array);
    auto road = schema->addSchema(Kind::Int);
    auto wrapper = schema->addSchema(Kind::Object);
    // Keep this auxiliary query root in the selected layer's JSON projection.
    schema->registerSchemaKey("query-wrapper", wrapper);
    owner->schemaType(root).asObject().addField("properties", owner->schemaType(properties));
    owner->schemaType(properties).asObject().addField("connectedRoads", owner->schemaType(roads));
    owner->schemaType(roads).asArray().addElement(owner->schemaType(road));
    owner->schemaType(wrapper).asObject().addField("$feature", owner->schemaType(root));
    schema->finalize();

    for (bool imported : {false, true}) {
        CAPTURE(imported);
        auto registry = imported ? roundtripSchema(schema) : schema;
        REQUIRE(registry->schemaId("query-wrapper") == wrapper);
        auto strings = std::make_shared<StringPool>("FeatureAliasTest");
        auto env = makeEnvironment(strings);
        env->setSchema(registry->rootType());
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
            auto refs =
                simfil::referencedSchemaPaths(*env, **ast, env->schema().pool()->schemaType(root));
            REQUIRE(refs);
            REQUIRE_FALSE(refs->hasUnresolvedAccess);
            REQUIRE(refs->paths.size() == 1);
            CHECK(refs->paths[0].path[0].field == StringPool::PropertiesStr);
        }
        CHECK(suggests(*env, root, "attributes.connectedR", "connectedRoads"));
        auto fields = registry->mapSchema()->schemaType(root).fieldPaths(
            registry->mapSchema()->dictionary().get("connectedRoads"));
        REQUIRE(fields.size() == 1);
        CHECK(fields[0][0].field == StringPool::PropertiesStr);
        auto ast = simfil::compile(*env, "#$feature.attributes.connectedRoads > 3", {.any = false});
        REQUIRE(ast);
        auto refs =
            simfil::referencedSchemaPaths(*env, **ast, env->schema().pool()->schemaType(wrapper));
        REQUIRE(refs);
        REQUIRE_FALSE(refs->hasUnresolvedAccess);
        CHECK(refs->paths[0].path[1].field == StringPool::PropertiesStr);
        CHECK(registry->mapSchema()->schemaType(wrapper).canHaveField(
            registry->mapSchema()->dictionary().get("attributes")));
        auto recursive = simfil::compile(*env, "**.attributes.connectedRoads", {.any = false});
        REQUIRE(recursive);
        auto nestedRefs = simfil::referencedSchemaPaths(
            *env,
            **recursive,
            env->schema().pool()->schemaType(wrapper));
        REQUIRE(nestedRefs);
        REQUIRE_FALSE(nestedRefs->hasUnresolvedAccess);
        REQUIRE(nestedRefs->paths.size() == 1);
        CHECK(nestedRefs->paths[0].path[1].field == StringPool::PropertiesStr);
        auto aliases =
            registry->mapSchema()->schemaType(root).fieldPaths(StringPool::AttributesStr);
        REQUIRE(aliases.size() == 1);
        CHECK(aliases[0][0].field == StringPool::PropertiesStr);
        CHECK(
            std::ranges::count(
                registry->mapSchema()->schemaType(wrapper).nestedFields(),
                StringPool::AttributesStr) == 0);
    }

    // A new immutable contract gives a real attributes member precedence over the alias.
    REQUIRE_THROWS(schema->addSchema(Kind::Object));
    std::tie(owner, schema) = newTestSchema();
    root = schema->addSchema(Kind::Object, LayerSchema::featureKey("Intersection"), "Feature");
    schema->addFeatureType(testFeatureType("Intersection"), root);
    properties = schema->addSchema(Kind::Object);
    roads = schema->addSchema(Kind::Array);
    road = schema->addSchema(Kind::Int);
    owner->schemaType(root).asObject().addField("properties", owner->schemaType(properties));
    owner->schemaType(properties).asObject().addField("connectedRoads", owner->schemaType(roads));
    owner->schemaType(roads).asArray().addElement(owner->schemaType(road));
    auto real = schema->addSchema(Kind::Object);
    owner->schemaType(real).asObject().addField("actual", owner->schemaType(road));
    owner->schemaType(root).asObject().addField("attributes", owner->schemaType(real));
    schema->finalize();
    auto strings = std::make_shared<StringPool>("FeatureAliasShadowTest");
    auto env = makeEnvironment(strings);
    env->setSchema(schema->rootType());
    for (auto query : {"attributes.actual", "attributes.connectedRoads"}) {
        auto ast = simfil::compile(*env, query, {.any = false});
        REQUIRE(ast);
        auto refs =
            simfil::referencedSchemaPaths(*env, **ast, env->schema().pool()->schemaType(root));
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
    auto [owner, schema] = newTestSchema();
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
    owner->schemaType(flags).asScalar().addNamedValue(
        {.name = "LEFT", .value = uint64_t{1}, .role = simfil::SchemaType::NamedValue::Role::Flag});
    owner->schemaType(flags).asScalar().addNamedValue(
        {.name = "RIGHT",
         .value = uint64_t{2},
         .role = simfil::SchemaType::NamedValue::Role::Flag});
    for (auto const& symbol : std::vector<std::string>{"WAITING", "ACTIVE", "DONE"})
        owner->schemaType(phase).asEnum().addSymbol(symbol);
    owner->schemaType(attribute).asObject().addField("label", owner->schemaType(label));
    owner->schemaType(attribute).asObject().declareField("label").setRequired(true);
    owner->schemaType(root).asObject().addField("id", owner->schemaType(count)).setMultimap(true);
    owner->schemaType(root)
        .asObject()
        .addField("attribute", owner->schemaType(attribute))
        .setMultimap(true);
    owner->schemaType(root)
        .asObject()
        .addField("variant", owner->schemaType(attribute))
        .setMultimap(true);
    owner->schemaType(root)
        .asObject()
        .addField("variant", owner->schemaType(count))
        .setMultimap(true);
    owner->schemaType(root)
        .asObject()
        .addField("groups", owner->schemaType(children))
        .setMultimap(true);
    owner->schemaType(root).asObject().addField("flags", owner->schemaType(flags));
    owner->schemaType(root).asObject().addField("phase", owner->schemaType(phase));
    owner->schemaType(root).asObject().addField("bytes", owner->schemaType(bytes));
    owner->schemaType(root).asObject().addField("children", owner->schemaType(children));
    owner->schemaType(children).asArray().addElement(owner->schemaType(root));
    owner->schemaType(root).asObject().declareField("id").setRequired(true);
    schema->finalize();

    auto exported = schema->toJsonSchema();
    auto restored = importTestSchema(exported, identityDeclarations(schema));
    // The codec exports from the immutable graph; there is no retained transport JSON cache.
    REQUIRE(restored->toJsonSchema() == exported);
    REQUIRE_FALSE(describe(schema, root).contains("jsonPointer"));
    REQUIRE(describe(restored, root)["jsonPointer"] == "/definitions/" + std::to_string(root));
    REQUIRE(
        withoutSourceLocations(describe(restored, root)) ==
        withoutSourceLocations(describe(schema, root)));
    REQUIRE(restored->childSchema(root, "attribute") == attribute);
    REQUIRE(restored->mapSchema()->schemaType(flags).kind() == LayerSchema::BitmaskKind);
    REQUIRE(restored->mapSchema()->schemaType(bytes).kind() == Kind::Bytes);
    REQUIRE(
        std::ranges::count(
            restored->mapSchema()->schemaType(root).directFields(),
            restored->mapSchema()->dictionary().get("_multimap")) == 0);
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
    projected["flags"] = simfil::Bitmask{3, schema->mapSchema()->schemaType(flags)}.toJson();
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
    auto [owner, schema] = newTestSchema();
    auto root = schema->addSchema(Kind::Object);
    schema->setRootSchema(root);
    auto first = schema->addSchema(Kind::Object, "shared-context-key");
    auto second = schema->addSchema(Kind::Object);
    REQUIRE_THROWS(schema->registerSchemaKey("shared-context-key", second));
    schema->registerSchemaKey("second-context", second);
    // Reference traversal reaches the second context first, unlike producer allocation.
    owner->schemaType(root).asObject().addField("a", owner->schemaType(second));
    owner->schemaType(root).asObject().addField("b", owner->schemaType(first));
    schema->finalize();

    auto const transport = schema->toJsonSchema();
    auto restored = importTestSchema(transport, identityDeclarations(schema));
    REQUIRE(restored->schemaId("shared-context-key") == schema->schemaId("shared-context-key"));
    REQUIRE(restored->schemaId("second-context") == second);
    REQUIRE(restored->toJsonSchema() == transport);
}

TEST_CASE(
    "Converter fragments share native domains but keep local references isolated",
    "[schema-domain]")
{
    auto [owner, schema] = newTestSchema();
    auto shared = schema->addSchema(Kind::String, "shared");
    auto root = schema->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    schema->addFeatureType(testFeatureType("Carrier"), root);
    auto fragment = R"({"type":"object","additionalProperties":false,"properties":{
        "value":{"$ref":"#/$defs/Value"},"shared":{"$ref":"shared"}},
        "$defs":{"Value":{"type":"integer","minimum":0,"maximum":7}}})"_json;
    auto first = schema->addJsonSchema(fragment);
    fragment["$defs"]["Value"] = {{"type", "string"}, {"enum", {"ON", "OFF"}}};
    auto second = schema->addJsonSchema(fragment);
    owner->schemaType(root).asObject().addField("first", owner->schemaType(first));
    owner->schemaType(root).asObject().addField("second", owner->schemaType(second));
    schema->finalize();
    REQUIRE(schema->childSchema(first, "shared") == shared);
    REQUIRE(schema->childSchema(second, "shared") == shared);
    REQUIRE(
        schema->mapSchema()->schemaType(schema->childSchema(first, "value")).kind() == Kind::Int);
    REQUIRE(
        schema->mapSchema()->schemaType(schema->childSchema(second, "value")).kind() ==
        Kind::String);
    auto exported = schema->toJsonSchema();
    auto restored = importTestSchema(exported, identityDeclarations(schema));
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
    auto [nativeOwner, native] = newTestSchema();
    auto root = native->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    native->addFeatureType(testFeatureType("Carrier"), root);
    native->finalize();
    auto snapshot = native;
    auto before = native->toJsonSchema();
    REQUIRE(snapshot->mapSchema() == native->mapSchema());
    REQUIRE_THROWS(native->addSchema(Kind::Int));
    auto [replacementOwner, replacement] = newTestSchema();
    auto replacementRoot =
        replacement->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    replacement->addFeatureType(testFeatureType("Carrier"), replacementRoot);
    replacementOwner->schemaType(replacementRoot)
        .asObject()
        .addField("added", replacementOwner->schemaType(replacement->addSchema(Kind::Int)));
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
    REQUIRE(schema->mapSchema()->schemaType(root).kind() == LayerSchema::FeatureKind);
    REQUIRE(schema->mapSchema()->schemaType(root).typeName() == "Thing");
    REQUIRE(schema->mapSchema()->schemaType(root).field("items").required() == true);
    REQUIRE(schema->mapSchema()->schemaType(root).field("optional").required() == false);
    REQUIRE_FALSE(schema->mapSchema()->schemaType(root).reachabilityComplete());
    REQUIRE(schema->mapSchema()->schemaType(root).canHaveField(
        schema->mapSchema()->dictionary().get("undeclaredBelowUnknown")));
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
    env->setSchema(schema->rootType());
    REQUIRE(suggests(*env, root, "items[17].na", "name"));
    REQUIRE(suggests(*env, root, "items[17].name == ru", "\"rural\""));
    REQUIRE(suggests(*env, root, "both.ri", "right"));
    auto roundtrip = roundtripSchema(schema);
    REQUIRE(roundtrip->featureSchema("Thing") == root);
    REQUIRE(json["jsonPointer"] == "#");
    REQUIRE(describe(roundtrip, root)["jsonPointer"] == "/definitions/" + std::to_string(root));
    REQUIRE(withoutSourceLocations(describe(roundtrip, root)) == withoutSourceLocations(json));
}

TEST_CASE(
    "Direct rich LayerSchema transport preserves IDs metadata and plural edges",
    "[DataSourceInfo][schema-domain]")
{
    auto [owner, schema] = newTestSchema();
    auto root = schema->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    schema->addFeatureType(testFeatureType("Carrier"), root);
    auto number = schema->addSchema(Kind::Int, "count");
    auto text = schema->addSchema(Kind::String, "text");
    auto array = schema->addSchema(Kind::Array, "list");
    auto unionId = schema->addSchema(Kind::Union, "choice");
    auto never = schema->addSchema(Kind::Never, "none");
    auto unknown = schema->addSchema(Kind::Unknown, "unknown");
    auto bytes = schema->addSchema(Kind::Bytes, "bytes");
    owner->schemaType(root).setTypeName("ConcreteCarrier");
    owner->schemaType(text).setTypeName("Label");
    owner->schemaType(text).setNullable(true);
    owner->schemaType(number).setNullable(false);
    owner->schemaType(text).asEnum().addSymbol("FIRST");
    owner->schemaType(number).asEnum().addValue(int64_t{4});
    owner->schemaType(bytes).asEnum().addValue(simfil::ByteArray{"blob"});
    owner->schemaType(unionId).setComposition(Composition::OneOf);
    owner->schemaType(unionId).asComposition().addAlternative(owner->schemaType(number));
    owner->schemaType(unionId).asComposition().addAlternative(owner->schemaType(text));
    owner->schemaType(array).asArray().addElement(owner->schemaType(number));
    owner->schemaType(array).asArray().addElement(owner->schemaType(text));
    owner->schemaType(root).asObject().addField("list", owner->schemaType(array));
    owner->schemaType(root).asObject().addField("many", owner->schemaType(number));
    owner->schemaType(root).asObject().addField("many", owner->schemaType(text));
    owner->schemaType(root).asObject().addField("odd.name", owner->schemaType(unionId));
    owner->schemaType(root).asObject().addField("none", owner->schemaType(never));
    owner->schemaType(root).asObject().addField("unknown", owner->schemaType(unknown));
    owner->schemaType(root).asObject().addField("bytes", owner->schemaType(bytes));
    owner->schemaType(root).asObject().addField("unspecified", {});
    owner->schemaType(root).asObject().declareField("list").setRequired(true);
    owner->schemaType(root).asObject().declareField("many").setRequired(false);
    schema->registerSchemaKey("textAlias", text);
    schema->finalize();
    auto json = schema->toJsonSchema();
    auto roundtrip = importTestSchema(json, identityDeclarations(schema));
    REQUIRE(roundtrip->featureSchema("Carrier") == root);
    REQUIRE(roundtrip->schemaId("textAlias") == text);
    REQUIRE(
        roundtrip->mapSchema()->schemaType(number).kind() ==
        schema->mapSchema()->schemaType(number).kind());
    REQUIRE(
        roundtrip->mapSchema()->schemaType(text).kind() ==
        schema->mapSchema()->schemaType(text).kind());
    REQUIRE(roundtrip->mapSchema()->schemaType(never).kind() == Kind::Never);
    REQUIRE(roundtrip->mapSchema()->schemaType(unknown).kind() == Kind::Unknown);
    std::vector<simfil::SchemaType::EnumValue> byteValues;
    roundtrip->mapSchema()->schemaType(bytes).forEachEnumValue(
        [&](auto const& value) { byteValues.push_back(value); });
    REQUIRE(byteValues.size() == 1);
    REQUIRE(std::get<simfil::ByteArray>(byteValues[0]).bytes == "blob");
    REQUIRE_FALSE(describe(schema, root).contains("jsonPointer"));
    REQUIRE(describe(roundtrip, root)["jsonPointer"] == "/definitions/" + std::to_string(root));
    REQUIRE(
        withoutSourceLocations(describe(roundtrip, root)) ==
        withoutSourceLocations(describe(schema, root)));
    REQUIRE(roundtrip->toJsonSchema() == json);

    auto invalid = json;
    invalid["definitions"]["bad"] = invalid["definitions"]["1"];
    REQUIRE_THROWS(importTestSchema(invalid, identityDeclarations(schema)));
}

TEST_CASE(
    "Attribute query roots preserve payload aliases and exact runtime overlays",
    "[DataSourceInfo][schema-domain]")
{
    auto [owner, schema] = newTestSchema();
    auto feature = schema->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    schema->addFeatureType(testFeatureType("Carrier"), feature);
    auto attribute = schema->addSchema(Kind::Object, "speed", "Attribute");
    auto value = schema->addSchema(Kind::Int);
    owner->schemaType(feature).asObject().addField("speed", owner->schemaType(attribute));
    owner->schemaType(feature).asObject().addField("identifier", owner->schemaType(value));
    owner->schemaType(attribute).asObject().addField("value", owner->schemaType(value));
    owner->schemaType(attribute).setTypeName("SpeedAttribute");
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
    env->setSchema(schema->rootType());
    REQUIRE(suggests(*env, root, "$feature.id", "identifier"));
    REQUIRE(suggests(*env, root, "val", "value"));
    auto ast = simfil::compile(
        *env,
        "SPEED == 4",
        simfil::CompileOptions{
            .any = false,
            .rewriteMode = simfil::RewriteMode::Schema,
            .rootSchema = env->schema().pool()->schemaType(root)});
    REQUIRE(ast);
    REQUIRE((*ast)->expr().toString().find("value") != std::string::npos);
    REQUIRE(sourceStrings->highest() == highest);
    auto restored = roundtripSchema(schema);
    REQUIRE(restored->attributeQuerySchema("Carrier", attribute) != simfil::NoTypeId);
    // Generated query overlays have no JSON location; their physical payloads do.
    REQUIRE_FALSE(describe(schema, attribute).contains("jsonPointer"));
    REQUIRE(
        describe(restored, attribute)["jsonPointer"] ==
        "/definitions/" + std::to_string(attribute));
    REQUIRE(
        withoutSourceLocations(
            describe(restored, restored->attributeQuerySchema("Carrier", attribute))) ==
        withoutSourceLocations(json));
}

TEST_CASE(
    "Shared Attribute query names select use sites within the current feature and map layer",
    "[schema-domain][schema-native][attribute-query-symbols]")
{
    auto graph = std::make_shared<MapSchema>();
    auto schema = graph->newLayer("Test", LayerType::Features);
    auto attribute = graph->newAttribute("shared-speed", "SpeedAttribute").id();
    graph->schemaType(attribute)
        .asObject()
        .addField("value", graph->schemaType(graph->primitive(Kind::Int)));
    for (std::string feature : {"Carrier", "Other"}) {
        REQUIRE(
            schema->newFeature(testFeatureType(feature, "identifier")).id() != simfil::NoTypeId);
        auto container = schema->addSchema(Kind::Object, {}, "AttributeContainer");
        graph->schemaType(schema->attributeLayerMapSchema(feature))
            .asObject()
            .addField("rules", graph->schemaType(container))
            .setMultimap(true);
        auto names = feature == "Carrier" ?
            std::vector<std::string>{"SPEED", "ADVISORY_SPEED"} :
            std::vector<std::string>{"OTHER_SPEED"};
        for (auto const& name : names) {
            schema->bindAttribute(graph->schemaType(attribute), feature, "rules", name);
        }
    }
    auto otherLayer = graph->newLayer("OtherLayer", LayerType::Features);
    REQUIRE(
        otherLayer->newFeature(testFeatureType("Carrier", "identifier")).id() != simfil::NoTypeId);
    auto container = otherLayer->addSchema(Kind::Object, {}, "AttributeContainer");
    graph->schemaType(otherLayer->attributeLayerMapSchema("Carrier"))
        .asObject()
        .addField("rules", graph->schemaType(container))
        .setMultimap(true);
    otherLayer->bindAttribute(graph->schemaType(attribute), "Carrier", "rules", "LAYER_SPEED");
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
        env->setSchema(registry->rootType());
        auto graph = registry->mapSchema();
        for (std::string feature : {"Carrier", "Other"}) {
            if (!registry->producesFeatureType(feature))
                continue;
            auto root = registry->attributeQuerySchema(feature, physical);
            REQUIRE(root != simfil::NoTypeId);
            for (auto const& owner : registry->attributeOwners(root)) {
                CHECK(owner.featureType_ == feature);
                CHECK(owner.featureLayer_ == registry->name());
            }
            REQUIRE(graph->schemaType(root).symbolEqualityPaths(simfil::StringPool::Empty).empty());
            for (std::string symbol :
                 {"SPEED", "ADVISORY_SPEED", "OTHER_SPEED", "LAYER_SPEED", "MISSING"}) {
                CAPTURE(feature, symbol);
                auto symbolId = graph->dictionary().get(symbol);
                CHECK(graph->schemaType(physical).symbolEqualityPaths(symbolId).empty());
                CHECK(graph->schemaType(registry->featureSchema(feature))
                          .symbolEqualityPaths(symbolId)
                          .empty());
                auto paths = graph->schemaType(root).symbolEqualityPaths(symbolId);
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
                CHECK(paths.front().front().field == graph->dictionary().get("$name"));
                auto scalarPaths = graph->schemaType(root).scalarFieldPathsForSymbol(symbolId);
                CHECK(scalarPaths.empty() == (registry->name() == "Test" && feature == "Carrier"));
                auto ast = simfil::compile(
                    *env,
                    symbol,
                    {.any = false,
                     .rewriteMode = simfil::RewriteMode::Schema,
                     .rootSchema = env->schema().pool()->schemaType(root)});
                REQUIRE(ast);
                for (std::string actual :
                     {"SPEED", "ADVISORY_SPEED", "OTHER_SPEED", "LAYER_SPEED", "MISSING"}) {
                    CAPTURE(actual);
                    auto data = std::make_shared<simfil::ModelPool>(strings);
                    data->setSchemaPool(registry->mapSchema());
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
    REQUIRE(schema->newFeature(testFeatureType("Carrier", "identifier")).id() != simfil::NoTypeId);
    auto container = schema->addSchema(Kind::Object, {}, "AttributeContainer");
    graph->schemaType(schema->attributeLayerMapSchema("Carrier"))
        .asObject()
        .addField("rules", graph->schemaType(container))
        .setMultimap(true);
    auto attribute = graph->newAttribute("shared-speed").id();
    graph->schemaType(attribute)
        .asObject()
        .addField("value", graph->schemaType(graph->primitive(Kind::Int)));
    for (auto name : {"SPEED", "ADVISORY_SPEED"}) {
        schema->bindAttribute(graph->schemaType(attribute), "Carrier", "rules", name);
    }
    graph->finalize();
    auto root = schema->attributeQuerySchema("Carrier", attribute);
    REQUIRE(root != simfil::NoTypeId);
    auto strings = std::make_shared<StringPool>("SharedAttributeScalarAliases");
    auto env = makeEnvironment(strings);
    env->setSchema(schema->rootType());
    auto options = simfil::CompileOptions{
        .any = true,
        .rewriteMode = simfil::RewriteMode::Schema,
        .rootSchema = env->schema().pool()->schemaType(root)};
    auto ambiguous = simfil::compile(*env, "SPEED > 40", options);
    REQUIRE(ambiguous);
    auto references =
        simfil::referencedSchemaPaths(*env, **ambiguous, env->schema().pool()->schemaType(root));
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
    auto guardedReferences =
        simfil::referencedSchemaPaths(*env, **guarded, env->schema().pool()->schemaType(root));
    REQUIRE(guardedReferences);
    CHECK_FALSE(guardedReferences->hasUnresolvedAccess);
    for (std::string actual : {"SPEED", "ADVISORY_SPEED"}) {
        CAPTURE(actual, normalized->normalizedQuery_);
        auto data = std::make_shared<simfil::ModelPool>(strings);
        data->setSchemaPool(schema->mapSchema());
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
    auto feature = schema->newFeature(testFeatureType("Carrier", "identifier")).id();
    auto attribute = graph->newAttribute("payload").id();
    schema->bindAttribute(graph->schemaType(attribute), "Carrier", "rules", "SPEED");
    graph->schemaType(schema->featurePropertiesSchema("Carrier"))
        .asObject()
        .addField("payload", graph->schemaType(attribute));
    graph->schemaType(attribute)
        .asObject()
        .addField("$name", graph->schemaType(graph->primitive(Kind::String)));
    graph->schemaType(attribute).asObject().addField("$feature", graph->schemaType(feature));
    graph->finalize();
    auto strings = std::make_shared<StringPool>("AttributePayloadQueryLookalike");
    auto env = makeEnvironment(strings);
    env->setSchema(schema->rootType());

    auto symbol = graph->dictionary().get("SPEED");
    CHECK(schema->attributeTypeCode(attribute) == "SPEED");
    CHECK(schema->childSchema(attribute, "$feature") == feature);
    CHECK(graph->schemaType(attribute).symbolEqualityPaths(symbol).empty());
    auto query = schema->attributeQuerySchema("Carrier", attribute);
    REQUIRE(query != simfil::NoTypeId);
    auto paths = graph->schemaType(query).symbolEqualityPaths(symbol);
    REQUIRE(paths.size() == 1);
    REQUIRE(paths.front().size() == 1);
    CHECK(paths.front().front().field == graph->dictionary().get("$name"));
}

TEST_CASE("Schema summaries preserve cycles across immutable contract variants", "[schema-domain]")
{
    auto variant = GENERATE(0, 1, 2);
    auto [owner, schema] = newTestSchema();
    auto first = schema->addSchema(Kind::Object);
    schema->setRootSchema(first);
    auto second = schema->addSchema(Kind::Object);
    auto array = schema->addSchema(Kind::Array);
    auto choice = schema->addSchema(Kind::Union);
    auto value = schema->addSchema(Kind::String);
    owner->schemaType(first).asObject().addField("next", owner->schemaType(second));
    owner->schemaType(first).asObject().addField("alias", owner->schemaType(array));
    owner->schemaType(second).asObject().addField("items", owner->schemaType(array));
    owner->schemaType(second).asObject().addField("state", owner->schemaType(value));
    owner->schemaType(array).asArray().addElement(owner->schemaType(choice));
    owner->schemaType(choice).setComposition(Composition::AnyOf);
    owner->schemaType(choice).asComposition().addAlternative(owner->schemaType(first));
    owner->schemaType(choice).asComposition().addAlternative(owner->schemaType(value));
    for (auto const& symbol : std::vector<std::string>{"ON", "OFF"})
        owner->schemaType(value).asEnum().addSymbol(symbol);
    if (variant != 0) {
        owner->schemaType(second).asObject().addField("late", owner->schemaType(value));
        owner->schemaType(value).asEnum().addSymbol("LATE");
    }
    owner->schemaType(second).asObject().setOpen(variant == 1);
    schema->finalize();

    for (auto id : {first, second, array, choice}) {
        CAPTURE(id);
        REQUIRE(std::ranges::is_sorted(schema->mapSchema()->schemaType(id).nestedFields()));
        REQUIRE(std::ranges::is_sorted(schema->mapSchema()->schemaType(id).nestedEnumSymbols()));
        // Use a value-returning projection rather than a prvalue transform
        // iterator (GCC 13 is_permutation retains the latter's expired reference).
        auto resolve = [&](auto name)
        {
            return schema->mapSchema()->dictionary().resolve(name).value();
        };
        REQUIRE(std::ranges::is_permutation(
            schema->mapSchema()->schemaType(id).nestedFields(),
            variant == 0 ? std::vector<std::string>{"alias", "items", "next", "state"} :
                           std::vector<std::string>{"alias", "items", "late", "next", "state"},
            {},
            resolve));
        REQUIRE(std::ranges::is_permutation(
            schema->mapSchema()->schemaType(id).nestedEnumSymbols(),
            variant == 0 ? std::vector<std::string>{"OFF", "ON"} :
                           std::vector<std::string>{"LATE", "OFF", "ON"},
            {},
            resolve));
        REQUIRE(schema->mapSchema()->schemaType(id).reachabilityComplete() == (variant != 1));
        REQUIRE(
            schema->mapSchema()->schemaType(id).canHaveField(
                schema->mapSchema()->dictionary().get("absent")) == (variant == 1));
    }
    REQUIRE(schema->mapSchema()->schemaType(value).nestedFields().empty());

    // A published completeness proof cannot be invalidated through a retained builder alias.
    REQUIRE_THROWS(owner->schemaType(second).asObject().setOpen(false));
}

TEST_CASE("Many attribute query roots retain the shared feature schema", "[schema-domain]")
{
    auto partial = GENERATE(false, true);
    auto [owner, schema] = newTestSchema();
    auto feature = schema->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    schema->addFeatureType(testFeatureType("Carrier"), feature);
    auto shared = schema->addSchema(Kind::Object);
    auto parent = shared;
    for (size_t i = 0; i < 128; ++i) {
        auto child = schema->addSchema(Kind::Object);
        owner->schemaType(parent).asObject().addField("left", owner->schemaType(child));
        owner->schemaType(parent).asObject().addField("right", owner->schemaType(child));
        parent = child;
    }
    auto value = schema->addSchema(Kind::String);
    owner->schemaType(value).asEnum().addSymbol("SHARED_VALUE");
    owner->schemaType(parent).asObject().addField("value", owner->schemaType(value));
    std::vector<simfil::TypeId> attributes;
    for (size_t i = 0; i < 400; ++i) {
        auto name = "attribute" + std::to_string(i);
        auto attribute = schema->addSchema(Kind::Object, {}, "Attribute");
        owner->schemaType(attribute).asObject().addField("payload", owner->schemaType(shared));
        owner->schemaType(feature).asObject().addField(name, owner->schemaType(attribute));
        schema->setAttributeMetadata(
            attribute,
            {"Carrier", "rules", name, attribute},
            "SharedAttribute");
        attributes.push_back(attribute);
    }
    if (partial)
        owner->schemaType(parent).asObject().addField("unknown", {});
    schema->finalize();
    for (auto attribute : attributes) {
        auto root = schema->attributeQuerySchema("Carrier", attribute);
        REQUIRE(root != simfil::NoTypeId);
        REQUIRE(schema->childSchema(root, "$feature") == feature);
        // $feature is an explicit context edge, not an implicit wildcard subtree.
        // Incomplete payload metadata must still leave absence proofs conservative.
        REQUIRE(
            schema->mapSchema()->schemaType(root).canHaveField(
                schema->mapSchema()->dictionary().get("attribute399")) == partial);
        REQUIRE(schema->mapSchema()
                    ->schemaType(schema->childSchema(root, "$feature"))
                    .canHaveField(schema->mapSchema()->dictionary().get("attribute399")));
        REQUIRE(schema->mapSchema()->schemaType(root).canHaveField(
            schema->mapSchema()->dictionary().get("value")));
        REQUIRE(schema->mapSchema()->schemaType(root).canHaveEnumSymbol(
            schema->mapSchema()->dictionary().get("SHARED_VALUE")));
        REQUIRE(schema->mapSchema()->schemaType(root).reachabilityComplete() == !partial);
        REQUIRE(
            schema->mapSchema()->schemaType(root).canHaveField(
                schema->mapSchema()->dictionary().get("absent")) == partial);
    }
    if (!partial)
        REQUIRE_FALSE(schema->mapSchema()->schemaType(feature).canHaveField(
            schema->mapSchema()->dictionary().get("$feature")));
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
    REQUIRE(schema->mapSchema()->schemaType(root).reachabilityComplete());
    REQUIRE_FALSE(schema->mapSchema()->schemaType(root).canHaveField(
        schema->mapSchema()->dictionary().get("absent")));
    auto multi = schema->childSchema(root, "multi");
    REQUIRE(schema->mapSchema()->schemaType(multi).kind() == Kind::Object);
    REQUIRE(schema->mapSchema()->schemaType(multi).composition() == Composition::None);
    auto json = describe(schema, root);
    REQUIRE(json["fields"]["next"]["truncated"] == "cycle");
    auto strings = std::make_shared<StringPool>("RecursiveCompletion");
    auto env = makeEnvironment(strings);
    env->setSchema(schema->rootType());
    REQUIRE(suggests(*env, root, "next.next.next.multi.la", "label"));
    auto ast = simfil::compile(
        *env,
        "DONE",
        simfil::CompileOptions{
            .any = false,
            .rewriteMode = simfil::RewriteMode::Schema,
            .rootSchema = env->schema().pool()->schemaType(root)});
    REQUIRE(ast);
    auto data = std::make_shared<simfil::ModelPool>(strings);
    REQUIRE(simfil::json::parse(R"({"next":{"next":{"state":"DONE"}}})", data));
    auto values = simfil::eval(*env, **ast, *data->root(0).value(), nullptr);
    REQUIRE(values);
    REQUIRE_FALSE(values->empty());
}

TEST_CASE(
    "Search preparation distinguishes attributes from enum meanings",
    "[schema-domain][search-preparation]")
{
    auto graph = std::make_shared<MapSchema>();
    auto schema = graph->newLayer("Road", LayerType::Features);
    REQUIRE(schema->newFeature(testFeatureType("Road", "roadId")).id() != simfil::NoTypeId);
    auto container = schema->addSchema(Kind::Object, {}, "AttributeContainer");
    graph->schemaType(schema->attributeLayerMapSchema("Road"))
        .asObject()
        .addField("rules", graph->schemaType(container))
        .setMultimap(true);
    auto speed = graph->newAttribute("SPEED_LIMIT").id();
    graph->schemaType(speed)
        .asObject()
        .addField("value", graph->schemaType(graph->primitive(Kind::Int)));
    schema->bindAttribute(graph->schemaType(speed), "Road", "rules", "SPEED_LIMIT");
    auto warning = graph->newAttribute("WARNING_SIGN").id();
    // Real datasource enums have a named semantic union and physical string
    // representation. The chooser must expose the declaration once, including
    // every attribute that uses it, rather than duplicate its storage forms.
    auto category = schema->addSchema(Kind::Union, {}, "WarningSign");
    graph->schemaType(category).setComposition(Composition::AnyOf);
    graph->schemaType(category).setTypeName("WarningSign");
    auto text = schema->addSchema(Kind::String);
    graph->schemaType(text).asEnum().addSymbol("SPEED_LIMIT");
    graph->schemaType(text).asEnum().addSymbol("ANIMALS");
    graph->schemaType(category).asComposition().addAlternative(graph->schemaType(text));
    graph->schemaType(category)
        .addNamedValue({.name = "SPEED_LIMIT", .value = std::string_view("SPEED_LIMIT")});
    graph->schemaType(category)
        .addNamedValue({.name = "ANIMALS", .value = std::string_view("ANIMALS")});
    graph->schemaType(warning).asObject().addField("warningSign", graph->schemaType(category));
    schema->bindAttribute(graph->schemaType(warning), "Road", "rules", "WARNING_SIGN");
    auto movable = graph->newAttribute("MOVABLE_WARNING_SIGN").id();
    graph->schemaType(movable)
        .asObject()
        .addField("movableWarningSign", graph->schemaType(category));
    schema->bindAttribute(graph->schemaType(movable), "Road", "rules", "MOVABLE_WARNING_SIGN");
    graph->finalize();
    for (auto query :
         {"SPEED_LIMIT", "\"SPEED_LIMIT\"", "any(SPEED_LIMIT)", "any(\"SPEED_LIMIT\")"}) {
        auto prepared =
            schema->searchInterpretations(query, LayerSchema::SearchQueryRequestedScope::Auto);
        REQUIRE(prepared);
        REQUIRE(prepared->size() == 2);
        CHECK((*prepared)[0].id_.find("attribute:") == 0);
        CHECK((*prepared)[1].id_.find("enum:") == 0);
        CHECK((*prepared)[1].query_.find("hasSymbol") != std::string::npos);
        CHECK((*prepared)[1].query_.find("WARNING_SIGN") != std::string::npos);
    }
    auto animals =
        schema->searchInterpretations("ANIMALS", LayerSchema::SearchQueryRequestedScope::Auto);
    REQUIRE(animals);
    REQUIRE(animals->size() == 1);
    CHECK(animals->front().attributeScopes_.size() == 2);
    CHECK(animals->front().id_ == "enum:attribute:WarningSign:ANIMALS");
    CHECK(animals->front().scope_ == LayerSchema::SearchQueryConcreteScope::Attribute);
    auto unknown =
        schema->searchInterpretations("UNDECLARED", LayerSchema::SearchQueryRequestedScope::Auto);
    REQUIRE(unknown);
    CHECK(unknown->empty());
}

TEST_CASE("Anonymous enum interpretations retain their artifact identity", "[search-preparation]")
{
    auto identity = [](std::string version)
    {
        auto graph =
            std::make_shared<MapSchema>(MapFormat{"AnonymousEnums", std::move(version), {}});
        auto layer = graph->newLayer("Road", LayerType::Features);
        auto root = layer->newFeature(testFeatureType("Road", "roadId"));
        auto value = layer->addSchema(Kind::String);
        graph->schemaType(value).asEnum().addSymbol("ANIMALS");
        root.properties().addField("category", graph->schemaType(value));
        graph->finalize();
        auto meanings =
            layer->searchInterpretations("ANIMALS", LayerSchema::SearchQueryRequestedScope::Auto);
        REQUIRE(meanings);
        REQUIRE(meanings->size() == 1);
        return meanings->front().id_;
    };
    // Equal graph-local type numbers are not evidence that two unnamed enums
    // describe the same concept. Named semantic types can group across sources.
    CHECK(identity("1") != identity("2"));
}
