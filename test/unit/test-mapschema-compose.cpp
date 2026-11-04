#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "mapget/model/mapschema.h"

#include <nlohmann/json-schema.hpp>
#include <sstream>

using namespace mapget;

namespace
{
/** Two independent namespaces with common recursive structure and additive leaves. */
class ComposeFixture
{
public:
    MapSchema::Ptr graph = std::make_shared<MapSchema>(MapFormat{"compose-test", "1", {}});
    LayerSchema::Ptr layer = graph->newLayer("Things", LayerType::Features);
    simfil::TypeId feature, properties, scalar, attribute;

    explicit ComposeFixture(bool addon = false, std::string datatype = "U64")
    {
        using Kind = simfil::Schema::Kind;
        // Different local IDs must not influence compatibility or the remap.
        if (addon)
            (void)layer->addSchema(Kind::Bool);
        feature = layer->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey("Thing"));
        properties = layer->addSchema(Kind::Object, LayerSchema::featurePropertiesKey("Thing"));
        scalar = layer->addSchema(addon ? Kind::String : Kind::Int);
        attribute = layer->addSchema(LayerSchema::AttributeKind, "attribute", "Attribute");
        layer->addFieldSchema(feature, "properties", properties);
        layer->addFieldSchema(properties, "self", properties);
        layer->addFieldSchema(properties, addon ? "label" : "count", scalar);
        layer->addFieldSchema(feature, "attribute", attribute);
        layer->addFieldSchema(attribute, addon ? "label" : "count", scalar);
        layer->setAttributeMetadata(attribute, {"Thing", "rules", "VALUE", attribute}, "Value");
        layer->addFeatureType(
            FeatureTypeInfo::fromJson(
                {{"name", "Thing"},
                 {"uniqueIdCompositions",
                  {{{{"partId", "id"}, {"datatype", std::move(datatype)}}}}}}),
            feature);
        layer->setRootSchema(feature);
    }

    /** Declare a complete empty inventory or one forward relation on the produced root. */
    void relations(std::string name = {}, bool complete = true)
    {
        auto values = nlohmann::json::array();
        if (!name.empty())
            values.push_back(
                {{"ownerType", "Thing"},
                 {"name", name},
                 {"targetTypes", {"Thing"}},
                 {"complete", complete},
                 {"locality", "same-container"}});
        layer
            ->setJsonSchemaAnnotations(feature, {{"x-mapget", {{"relationDeclarations", values}}}});
    }
};

/** Exercise artifact decoding rather than retaining construction-cache translations. */
MapSchema::ConstPtr decode(MapSchema const& graph)
{
    std::ostringstream stream(std::ios::binary);
    REQUIRE(graph.write(stream));
    auto data = stream.str();
    return MapSchema::read(std::span(reinterpret_cast<uint8_t const*>(data.data()), data.size()));
}
}  // namespace

TEST_CASE(
    "MapSchema composes additive graphs and regenerates query contexts",
    "[mapschema][composition]")
{
    ComposeFixture base, addon(true);
    base.relations("next");
    addon.relations("previous");
    base.graph->finalize();
    addon.graph->finalize();
    auto baseJson = base.layer->toJsonSchema(), addonJson = addon.layer->toJsonSchema();
    auto owners = std::vector<MapSchema::ConstPtr>{base.graph, addon.graph};
    auto composed = MapSchema::compose(base.graph->format(), owners);
    auto layer = composed->layer("Things");
    REQUIRE(layer);
    auto root = layer->featureSchema("Thing");
    auto properties = layer->childSchema(root, "properties");
    REQUIRE(properties);
    CHECK(layer->childSchema(root, "attributes") == properties);
    CHECK(layer->childSchema(properties, "self") == properties);
    CHECK(layer->kind(layer->childSchema(properties, "count")) == simfil::Schema::Kind::Int);
    CHECK(layer->kind(layer->childSchema(properties, "label")) == simfil::Schema::Kind::String);
    CHECK(composed->remapSchema(base.graph->schemaId(), base.properties) == properties);
    CHECK(composed->remapSchema(addon.graph->schemaId(), addon.properties) == properties);
    CHECK(base.layer->toJsonSchema() == baseJson);
    CHECK(addon.layer->toJsonSchema() == addonJson);
    CHECK(layer->hasRelationDeclarations());
    CHECK(layer->relationDeclarations().size() == 2);
    CHECK(layer->relationsTo("Thing").size() == 2);
    auto attribute = layer->childSchema(root, "attribute");
    auto context = layer->attributeQuerySchema("Thing", attribute);
    REQUIRE(context);
    CHECK(layer->childSchema(context, "$feature") == root);
    CHECK(layer->childSchema(context, "count"));
    CHECK(layer->childSchema(context, "label"));
    CHECK(layer->fieldRole(context, "$feature") == simfil::Schema::FieldRole::Context);
    CHECK(
        composed->remapSchema(
            base.graph->schemaId(),
            base.layer->attributeQuerySchema("Thing", base.attribute)) == context);
    CHECK(
        composed->remapSchema(
            addon.graph->schemaId(),
            addon.layer->attributeQuerySchema("Thing", addon.attribute)) == context);
    for (auto const& owner : owners)
        for (size_t id = 1; id <= owner->typeCount(); ++id)
            CHECK(composed->type(composed->remapSchema(owner->schemaId(), simfil::TypeId(id))));
    CHECK(composed->checkForErrors().empty());
    CHECK(composed->constituents().size() == 2);
    CHECK(composed->statistics()["constituents"] == composed->constituents());
    auto restored = decode(*composed);
    CHECK(restored->schemaId() == composed->schemaId());
    CHECK(restored->constituents() == composed->constituents());
    CHECK(restored->layer("Things")->toJsonSchema() == layer->toJsonSchema());
    CHECK_THROWS(restored->schemaRemap(base.graph->schemaId()));
    CHECK(MapSchema::compose(base.graph->format(), owners).get() == composed.get());
    CHECK(
        MapSchema::compose(
            base.graph->format(),
            std::vector<MapSchema::ConstPtr>{base.graph, base.graph})
            .get() == base.graph.get());
    auto reversed = MapSchema::compose(
        base.graph->format(),
        std::vector<MapSchema::ConstPtr>{addon.graph, base.graph});
    CHECK(reversed->schemaId() != composed->schemaId());
}

TEST_CASE(
    "MapSchema composition rejects contradictory common field meaning",
    "[mapschema][composition]")
{
    ComposeFixture base, addon(true);
    SECTION("Scalar kind")
    {
        addon.layer->addFieldSchema(addon.properties, "count", addon.scalar);
    }
    SECTION("Required presence")
    {
        base.layer->setFieldRequired(base.properties, "self", true);
        addon.layer->setFieldRequired(addon.properties, "self", false);
    }
    SECTION("Context edge")
    {
        addon.layer->setFieldRole(addon.properties, "self", simfil::Schema::FieldRole::Context);
    }
    SECTION("Multimap")
    {
        addon.layer->addFieldSchema(addon.properties, "self", addon.properties, true);
    }
    SECTION("Identity")
    {
        ComposeFixture other(true, "U32");
        base.graph->finalize();
        other.graph->finalize();
        CHECK_THROWS_WITH(
            MapSchema::compose(
                base.graph->format(),
                std::vector<MapSchema::ConstPtr>{base.graph, other.graph}),
            Catch::Matchers::ContainsSubstring("feature identity composition differs"));
        return;
    }
    SECTION("Numeric unit")
    {
        auto value = addon.layer->addSchema(simfil::Schema::Kind::Int);
        addon.layer->addFieldSchema(addon.properties, "count", value);
        simfil::Schema::NumericDomain left, right;
        left.unit = "m";
        right.unit = "km";
        base.layer->setNumericDomain(base.scalar, left);
        addon.layer->setNumericDomain(value, right);
    }
    SECTION("Forward contract")
    {
        base.relations("next", true);
        addon.relations("next", false);
    }
    base.graph->finalize();
    addon.graph->finalize();
    CHECK_THROWS_WITH(
        MapSchema::compose(
            base.graph->format(),
            std::vector<MapSchema::ConstPtr>{base.graph, addon.graph}),
        Catch::Matchers::ContainsSubstring("Incompatible schema contribution"));
}

TEST_CASE(
    "Composition preserves unknown relation inventory without dropping known declarations",
    "[mapschema][composition]")
{
    ComposeFixture base, addon(true);
    base.relations("next");
    base.graph->finalize();
    addon.graph->finalize();
    auto graph = MapSchema::compose(
        base.graph->format(),
        std::vector<MapSchema::ConstPtr>{base.graph, addon.graph});
    for (auto const& current : {graph, decode(*graph)}) {
        auto layer = current->layer("Things");
        CHECK_FALSE(layer->hasRelationDeclarations());
        CHECK(layer->relationDeclarations().size() == 1);
        CHECK(layer->relationsTo("Thing").size() == 1);
    }
}

TEST_CASE("Composition preserves repeated exclusive alternatives", "[mapschema][composition]")
{
    auto source = std::make_shared<MapSchema>();
    auto layer = source->importLayer("Raw", LayerType::SourceData, nlohmann::json::parse(R"({
        "definitions":{"integer":{"type":"integer"}},
        "oneOf":[{"$ref":"#/definitions/integer"},{"$ref":"#/definitions/integer"}]
    })"));
    source->finalize();
    auto composed = MapSchema::compose(
        MapFormat{"composed", "1", {}},
        std::vector<MapSchema::ConstPtr>{source});
    for (auto const& current : {MapSchema::ConstPtr(source), composed, decode(*composed)}) {
        auto view = current->layer("Raw");
        auto alternatives = current->type(view->rootSchema())->alternatives();
        REQUIRE(alternatives.size() == 2);
        CHECK(alternatives[0] == alternatives[1]);
        nlohmann::json_schema::json_validator validator;
        REQUIRE_NOTHROW(validator.set_root_schema(view->toJsonSchema()));
        CHECK_THROWS(validator.validate(7));
    }
}
