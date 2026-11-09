#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "mapget/model/mapschema.h"

#include <array>
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
        using Kind = simfil::SchemaType::Kind;
        // Different local IDs must not influence compatibility or the remap.
        if (addon)
            (void)layer->addSchema(Kind::Bool);
        feature = layer->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey("Thing"));
        properties = layer->addSchema(Kind::Object, LayerSchema::featurePropertiesKey("Thing"));
        scalar = layer->addSchema(addon ? Kind::String : Kind::Int);
        attribute = layer->addSchema(LayerSchema::AttributeKind, "attribute", "Attribute");
        graph->schemaType(feature).asObject().addField("properties", graph->schemaType(properties));
        graph->schemaType(properties).asObject().addField("self", graph->schemaType(properties));
        graph->schemaType(properties)
            .asObject()
            .addField(addon ? "label" : "count", graph->schemaType(scalar));
        graph->schemaType(feature).asObject().addField("attribute", graph->schemaType(attribute));
        graph->schemaType(attribute)
            .asObject()
            .addField(addon ? "label" : "count", graph->schemaType(scalar));
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
    // Core normalization must create the standard Feature alias on source graphs,
    // including manually declared Feature kinds, before composition copies it.
    for (auto const* fixture : {&base, &addon}) {
        auto type = fixture->graph->schemaType(fixture->feature);
        auto alias = type.field("attributes");
        REQUIRE(alias);
        CHECK(alias.name() == "properties");
        CHECK(fixture->layer->childSchema(fixture->feature, "attributes") == fixture->properties);
        type.forEachField([](auto const& field) { CHECK(field.name() != "attributes"); });
    }
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
    CHECK(
        composed->schemaType(layer->childSchema(properties, "count")).kind() ==
        simfil::SchemaType::Kind::Int);
    CHECK(
        composed->schemaType(layer->childSchema(properties, "label")).kind() ==
        simfil::SchemaType::Kind::String);
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
    CHECK(
        composed->schemaType(context).fieldRole(composed->dictionary().get("$feature")) ==
        simfil::SchemaType::FieldRole::Context);
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
            CHECK(
                composed->schemaType(composed->remapSchema(owner->schemaId(), simfil::TypeId(id))));
    CHECK(composed->checkForErrors().empty());
    CHECK(composed->constituents().size() == 2);
    CHECK(composed->statistics()["constituents"] == composed->constituents());
    auto restored = decode(*composed);
    CHECK(restored->schemaId() == composed->schemaId());
    CHECK(restored->constituents() == composed->constituents());
    CHECK(restored->layer("Things")->toJsonSchema() == layer->toJsonSchema());
    // Layer-local keys must be canonical alias records, not only a construction cache.
    for (auto const& owner : {composed, restored}) {
        auto current = owner->layer("Things");
        CHECK(current->schemaId("attribute") == attribute);
        CHECK(current->schemaId(LayerSchema::featurePropertiesKey("Thing")) == properties);
    }
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
    "Composition shares identities across layers and preserves absent targets",
    "[mapschema][composition]")
{
    auto base = std::make_shared<MapSchema>();
    auto addon = std::make_shared<MapSchema>();
    FeatureTypeInfo identity{"Road", {{{"roadId", {}, IdPartDataType::U32}}}};
    auto a = base->newLayer("A", LayerType::Features);
    auto b = addon->newLayer("B", LayerType::Features);
    (void)a->newFeature(identity);
    SECTION("Compatible declarations")
    {
        identity.uniqueIdCompositions_[0][0].description_ = "Equivalent identity, different prose";
        (void)b->newFeature(identity);
        addon->addFeatureType({"Unavailable", {{{"key", {}, IdPartDataType::STR}}}});
        base->finalize();
        addon->finalize();
        auto combined = MapSchema::compose({}, std::vector<MapSchema::ConstPtr>{base, addon});
        for (auto const& owner : {combined, decode(*combined)}) {
            CHECK(owner->featureIdentities().size() == 2);
            CHECK(owner->layer("A")->featureTypes() == std::vector<std::string>{"Road"});
            CHECK(owner->layer("B")->featureTypes() == std::vector<std::string>{"Road"});
            CHECK(owner->layer("A")->getTypeInfo("Road") == owner->layer("B")->getTypeInfo("Road"));
            CHECK(owner->layer("A")->getTypeInfo("Unavailable"));
            CHECK_FALSE(owner->layer("A")->producesFeatureType("Unavailable"));
            CHECK(owner->layer("A")->featureSchema("Unavailable") == simfil::NoTypeId);
        }
    }
    SECTION("Conflicts even between disjoint layers")
    {
        identity.uniqueIdCompositions_[0][0].datatype_ = IdPartDataType::I32;
        (void)b->newFeature(identity);
        base->finalize();
        addon->finalize();
        CHECK_THROWS_WITH(
            MapSchema::compose({}, std::vector<MapSchema::ConstPtr>{base, addon}),
            "Conflicting map-wide feature identity: Road");
    }
}

TEST_CASE(
    "MapSchema composition rejects contradictory common field meaning",
    "[mapschema][composition]")
{
    ComposeFixture base, addon(true);
    SECTION("Scalar kind")
    {
        addon.graph->schemaType(addon.properties)
            .asObject()
            .addField("count", addon.graph->schemaType(addon.scalar));
    }
    SECTION("Required presence")
    {
        base.graph->schemaType(base.properties).field("self").setRequired(true);
        addon.graph->schemaType(addon.properties).field("self").setRequired(false);
    }
    SECTION("Context edge")
    {
        addon.graph->schemaType(addon.properties)
            .field("self")
            .setRole(simfil::SchemaType::FieldRole::Context);
    }
    SECTION("Multimap")
    {
        addon.graph->schemaType(addon.properties)
            .asObject()
            .addField("self", addon.graph->schemaType(addon.properties))
            .setMultimap(true);
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
            Catch::Matchers::ContainsSubstring("Conflicting map-wide feature identity"));
        return;
    }
    SECTION("Numeric unit")
    {
        auto value = addon.layer->addSchema(simfil::SchemaType::Kind::Int);
        addon.graph->schemaType(addon.properties)
            .asObject()
            .addField("count", addon.graph->schemaType(value));
        simfil::SchemaType::NumericDomain left, right;
        left.unit = "m";
        right.unit = "km";
        base.graph->schemaType(base.scalar).asScalar().setNumericDomain(left);
        addon.graph->schemaType(value).asScalar().setNumericDomain(right);
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
        auto alternatives = view->rootType().alternatives();
        REQUIRE(alternatives.size() == 2);
        CHECK(alternatives[0] == alternatives[1]);
        nlohmann::json_schema::json_validator validator;
        REQUIRE_NOTHROW(validator.set_root_schema(view->toJsonSchema()));
        CHECK_THROWS(validator.validate(7));
    }
}

TEST_CASE(
    "Composition retains typed scalar metadata without JSON projection",
    "[mapschema][composition]")
{
    ComposeFixture base, addon(true);
    for (auto* fixture : {&base, &addon}) {
        auto numeric = fixture->layer->addSchema(simfil::SchemaType::Kind::Int, "measurement");
        fixture->graph->schemaType(fixture->properties)
            .asObject()
            .addField("measurement", fixture->graph->schemaType(numeric));
        auto scalar = fixture->graph->schemaType(numeric).asScalar();
        std::array<simfil::SchemaType::NumericRange, 1> ranges{
            simfil::SchemaType::NumericRange{uint64_t{0}, UINT64_MAX - uint64_t{1}}};
        simfil::SchemaType::NumericDomain domain;
        domain.encoding = "uint64";
        domain.unit = "mm";
        domain.minimum = uint64_t{0};
        domain.maximum = UINT64_MAX;
        domain.ordinaryRanges = std::span<simfil::SchemaType::NumericRange const>{ranges};
        domain.scale = 0.001;
        domain.complete = true;
        scalar.setNumericDomain(domain);
        simfil::SchemaType::NamedValue special;
        special.name = "UNAVAILABLE";
        special.value = UINT64_MAX;
        special.rawValue = UINT64_MAX;
        special.role = simfil::SchemaType::NamedValue::Role::Special;
        special.meaning = simfil::SchemaType::NamedValue::Meaning::Unknown;
        scalar.addNamedValue(special);

        auto enumeration = fixture->layer->addSchema(simfil::SchemaType::Kind::Int, "numeric-enum");
        fixture->graph->schemaType(fixture->properties)
            .asObject()
            .addField("numericEnum", fixture->graph->schemaType(enumeration));
        auto values = fixture->graph->schemaType(enumeration).asEnum();
        values.addValue(int64_t{7});
        values.addValue(int64_t{11});
        auto flags = fixture->layer->addSchema(simfil::SchemaType::Kind::Bitmask, "flags");
        fixture->graph->schemaType(fixture->properties)
            .asObject()
            .addField("flags", fixture->graph->schemaType(flags));
        auto mask = fixture->graph->schemaType(flags).asBitmask();
        mask.setBitWidth(64);
        mask.addFlag("HIGH", uint64_t{1} << 63);
        mask.addFlag("NONE", 0);
        fixture->graph->finalize();
    }
    auto composed = MapSchema::compose(
        base.graph->format(),
        std::vector<MapSchema::ConstPtr>{base.graph, addon.graph});
    for (auto const& owner : {composed, decode(*composed)}) {
        auto layer = owner->layer("Things");
        auto properties = layer->childSchema(layer->featureSchema("Thing"), "properties");
        auto scalar = owner->schemaType(layer->childSchema(properties, "measurement"));
        auto domain = scalar.numericDomain();
        REQUIRE(domain);
        CHECK(domain->encoding == "uint64");
        CHECK(domain->unit == "mm");
        CHECK(domain->maximum == simfil::SchemaType::NumericValue{UINT64_MAX});
        CHECK(domain->scale == 0.001);
        CHECK(domain->complete);
        REQUIRE(domain->ordinaryRanges.size() == 1);
        CHECK(
            domain->ordinaryRanges[0].max ==
            simfil::SchemaType::NumericValue{UINT64_MAX - uint64_t{1}});
        auto special = scalar.namedValue("UNAVAILABLE");
        REQUIRE(special);
        CHECK(std::get<uint64_t>(special->value) == UINT64_MAX);
        REQUIRE(special->rawValue);
        CHECK(std::get<uint64_t>(*special->rawValue) == UINT64_MAX);
        CHECK(special->meaning == simfil::SchemaType::NamedValue::Meaning::Unknown);
        auto enumeration = owner->schemaType(layer->childSchema(properties, "numericEnum"));
        std::vector<simfil::SchemaType::EnumValue> values;
        enumeration.forEachEnumValue([&](auto const& value) { values.push_back(value); });
        CHECK(values == std::vector<simfil::SchemaType::EnumValue>{int64_t{7}, int64_t{11}});
        auto flags = owner->schemaType(layer->childSchema(properties, "flags"));
        CHECK(flags.isBitmask());
        CHECK(flags.bitWidth() == 64);
        auto high = flags.namedValue("HIGH");
        REQUIRE(high);
        CHECK(std::get<uint64_t>(high->value) == uint64_t{1} << 63);
        auto none = flags.namedValue("NONE");
        REQUIRE(none);
        CHECK(std::get<uint64_t>(none->value) == 0);
    }
}

TEST_CASE(
    "Composition preserves bases, unknown fields and lookup-only aliases",
    "[mapschema][composition]")
{
    ComposeFixture base, addon(true);
    for (auto* fixture : {&base, &addon}) {
        auto inherited = fixture->layer->addSchema(simfil::SchemaType::Kind::Object, "inherited");
        auto scalar = fixture->layer->addSchema(simfil::SchemaType::Kind::Int, "inherited-int");
        auto parent = fixture->graph->schemaType(inherited).asObject();
        parent.addField("first", fixture->graph->schemaType(scalar));
        auto object = fixture->graph->schemaType(fixture->properties).asObject();
        object.setBase(parent);
        object.addField("unknown");
        object.addAlias("legacy", "self");
        fixture->graph->finalize();
    }
    auto composed = MapSchema::compose(
        base.graph->format(),
        std::vector<MapSchema::ConstPtr>{base.graph, addon.graph});
    for (auto const& owner : {composed, decode(*composed)}) {
        auto layer = owner->layer("Things");
        auto id = layer->childSchema(layer->featureSchema("Thing"), "properties");
        auto object = owner->schemaType(id);
        REQUIRE(object.baseType());
        CHECK(object.field("first").parentType() == object.baseType());
        CHECK(object.field("legacy").name() == "self");
        REQUIRE(object.field("unknown"));
        REQUIRE(object.field("unknown").types().size() == 1);
        CHECK(object.field("unknown").types()[0] == simfil::NoTypeId);
        std::vector<std::string_view> names;
        object.forEachField([&](auto const& field) { names.push_back(field.name()); });
        CHECK(names == std::vector<std::string_view>{"first", "self", "count", "unknown", "label"});
        size_t aliases = 0;
        object.forEachFieldAlias([&](auto, auto) { ++aliases; });
        CHECK(aliases == 1);
    }
}

TEST_CASE(
    "Composition compares alias and exact scalar meaning after valid publication",
    "[mapschema][composition]")
{
    ComposeFixture base, addon(true);
    auto left = base.layer->addSchema(simfil::SchemaType::Kind::Int, "shared-int");
    auto right = addon.layer->addSchema(simfil::SchemaType::Kind::Int, "shared-int");
    base.graph->schemaType(base.properties)
        .asObject()
        .addField("sample", base.graph->schemaType(left));
    addon.graph->schemaType(addon.properties)
        .asObject()
        .addField("sample", addon.graph->schemaType(right));
    SECTION("Known and unknown child")
    {
        base.graph->schemaType(base.properties).asObject().addField("missing");
        addon.graph->schemaType(addon.properties)
            .asObject()
            .addField("missing", addon.graph->schemaType(right));
    }
    SECTION("Lookup alias versus enumerated field")
    {
        base.graph->schemaType(base.properties).asObject().addAlias("legacy", "sample");
        addon.graph->schemaType(addon.properties)
            .asObject()
            .addField("legacy", addon.graph->schemaType(right));
    }
    SECTION("Different named raw values")
    {
        simfil::SchemaType::NamedValue value;
        value.name = "SPECIAL";
        value.value = int64_t{-1};
        value.rawValue = UINT64_MAX;
        base.graph->schemaType(left).asScalar().addNamedValue(value);
        value.rawValue = UINT64_MAX - uint64_t{1};
        addon.graph->schemaType(right).asScalar().addNamedValue(value);
    }
    SECTION("Different bitmask encoding widths")
    {
        for (auto* fixture : {&base, &addon}) {
            auto id = fixture->layer->addSchema(simfil::SchemaType::Kind::Bitmask, "shared-mask");
            fixture->graph->schemaType(fixture->properties)
                .asObject()
                .addField("flags", fixture->graph->schemaType(id));
            auto flags = fixture->graph->schemaType(id).asBitmask();
            flags.setBitWidth(fixture == &base ? 64 : 32);
            flags.addFlag("SOME", uint64_t{1} << 31);
        }
    }
    SECTION("Different ordinary measurement ranges")
    {
        std::array<simfil::SchemaType::NumericRange, 1> ranges{
            simfil::SchemaType::NumericRange{int64_t{0}, int64_t{100}}};
        simfil::SchemaType::NumericDomain domain;
        domain.ordinaryRanges = std::span<simfil::SchemaType::NumericRange const>{ranges};
        base.graph->schemaType(left).asScalar().setNumericDomain(domain);
        ranges[0].max = int64_t{101};
        addon.graph->schemaType(right).asScalar().setNumericDomain(domain);
    }
    REQUIRE_NOTHROW(base.graph->finalize());
    REQUIRE_NOTHROW(addon.graph->finalize());
    CHECK_THROWS_WITH(
        MapSchema::compose(
            base.graph->format(),
            std::vector<MapSchema::ConstPtr>{base.graph, addon.graph}),
        Catch::Matchers::ContainsSubstring("Incompatible schema contribution"));
}

TEST_CASE(
    "Composition never deduplicates exclusive alternatives across contributors",
    "[mapschema][composition]")
{
    std::vector<MapSchema::ConstPtr> inputs;
    for (bool padded : {false, true}) {
        auto graph = std::make_shared<MapSchema>();
        auto layer = graph->newLayer("Raw", LayerType::SourceData);
        if (padded)
            (void)layer->addSchema(simfil::SchemaType::Kind::Bool);
        auto integer = layer->addSchema(simfil::SchemaType::Kind::Int, "integer");
        auto root = layer->addSchema(simfil::SchemaType::Kind::OneOf);
        graph->schemaType(root).setComposition(simfil::SchemaType::Composition::OneOf);
        auto exclusive = graph->schemaType(root).asComposition();
        exclusive.addAlternative(graph->schemaType(integer));
        exclusive.addAlternative(graph->schemaType(integer));
        layer->setRootSchema(root);
        graph->finalize();
        inputs.push_back(graph);
    }
    auto composed = MapSchema::compose({}, inputs);
    for (auto const& owner : {composed, decode(*composed)}) {
        auto layer = owner->layer("Raw");
        auto alternatives = owner->schemaType(layer->rootSchema()).alternatives();
        REQUIRE(alternatives.size() == 2);
        CHECK(alternatives[0] == alternatives[1]);
        nlohmann::json_schema::json_validator validator;
        REQUIRE_NOTHROW(validator.set_root_schema(layer->toJsonSchema()));
        // If either branch were dropped, this value would wrongly become valid.
        CHECK_THROWS(validator.validate(7));
    }
}

TEST_CASE(
    "Composition preserves named values on an unfinished scalar union",
    "[mapschema][composition]")
{
    ComposeFixture base, addon(true);
    for (auto* fixture : {&base, &addon}) {
        auto type = fixture->graph->newCompositionType(simfil::SchemaType::Composition::AnyOf);
        type.addAlternative(fixture->graph->schemaType(
            fixture->graph->primitive(simfil::SchemaType::Kind::String)));
        type.addAlternative(
            fixture->graph->schemaType(fixture->graph->primitive(simfil::SchemaType::Kind::Int)));
        type.addNamedValue({.name = "UNKNOWN", .value = std::string_view("UNKNOWN")});
        fixture->graph->schemaType(fixture->properties).asObject().addField("status", type);
        fixture->graph->finalize();
    }
    auto composed =
        MapSchema::compose({}, std::vector<MapSchema::ConstPtr>{base.graph, addon.graph});
    auto root = composed->layer("Things")->featurePropertiesSchema("Thing");
    auto status = composed->schemaType(root).field("status").type();
    CHECK(status.composition() == simfil::SchemaType::Composition::AnyOf);
    auto value = status.namedValue("UNKNOWN");
    REQUIRE(value);
    CHECK(std::get<std::string_view>(value->value) == "UNKNOWN");
}
