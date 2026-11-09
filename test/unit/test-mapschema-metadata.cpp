#include <array>
#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include "mapget/model/mapschema.h"

using namespace mapget;

namespace
{
/** Keep public contract assertions independent of numeric row allocation order. */
FeatureTypeInfo identity(std::string name)
{
    return {std::move(name), {{{"localId", "", IdPartDataType::U64}}}};
}

/** Exercise canonical metadata decoding without retaining construction caches. */
MapSchema::ConstPtr decode(MapSchema const& owner)
{
    std::ostringstream output(std::ios::binary);
    REQUIRE(owner.write(output));
    auto bytes = output.str();
    return MapSchema::read({reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()});
}
}  // namespace

TEST_CASE(
    "Canonical produced roots preserve lexical order rather than dictionary order",
    "[mapschema][metadata]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Features", LayerType::Features);
    auto zulu = layer->newFeature(identity("Zulu"));
    auto alpha = layer->newFeature(identity("Alpha"));
    REQUIRE(zulu != alpha);
    CHECK(layer->featureTypes() == std::vector<std::string>{"Alpha", "Zulu"});
    owner->finalize();
    REQUIRE(layer->rootType().alternatives().size() == 2);
    CHECK(layer->rootType().alternatives()[0] == alpha.id());
    CHECK(layer->rootType().alternatives()[1] == zulu.id());
    auto restored = decode(*owner)->layer("Features");
    CHECK(restored->featureTypes() == layer->featureTypes());
    CHECK(restored->featureSchema("Alpha") == alpha.id());
    CHECK(restored->featureSchema("Zulu") == zulu.id());
}

TEST_CASE(
    "Replacing canonical relation inventories retires old rows without losing metadata",
    "[mapschema][metadata]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Road", LayerType::Features);
    (void)layer->newFeature(identity("Road"));
    using Locality = LayerSchema::RelationDeclaration::Locality;
    layer->setRelationDeclarations(
        {{"Road", "next", {"Road", "Lane"}, true, Locality::MayCrossContainer}});
    layer->setRelationDeclarations(
        {{"Road", "previous", {"Road", "Road"}, false, Locality::SameContainer}});
    REQUIRE(layer->relationDeclarations().size() == 1);
    CHECK(layer->relationDeclarations().front().name_ == "previous");
    owner->finalize();
    for (auto graph : {MapSchema::ConstPtr(owner), decode(*owner)}) {
        auto restored = graph->layer("Road");
        CHECK(restored->hasRelationDeclarations());
        CHECK(restored->relationsTo("Lane").empty());
        auto declarations = restored->relationsTo("Road");
        REQUIRE(declarations.size() == 1);
        CHECK(declarations[0].name_ == "previous");
        CHECK(declarations[0].targetTypes_ == std::vector<std::string>{"Road"});
        CHECK_FALSE(declarations[0].complete_);
        CHECK(declarations[0].locality_ == Locality::SameContainer);
        auto contract = restored->featureTypeContract("Road");
        REQUIRE(contract["forwardRelations"].size() == 1);
        CHECK(contract["forwardRelations"][0]["name"] == "previous");
    }
}

TEST_CASE("Empty replacement relation inventories remain explicitly known", "[mapschema][metadata]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Road", LayerType::Features);
    (void)layer->newFeature(identity("Road"));
    layer->setRelationDeclarations({{"Road", "next", {"Road"}, true}});
    layer->setRelationDeclarations({});
    owner->finalize();
    auto restored = decode(*owner)->layer("Road");
    CHECK(restored->hasRelationDeclarations());
    CHECK(restored->relationDeclarations().empty());
    CHECK(restored->relationsTo("Road").empty());
}

TEST_CASE("Invalid produced roots cannot register a new identity", "[mapschema][metadata]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Road", LayerType::Features);
    REQUIRE_THROWS(layer->addFeatureType(identity("Road"), UINT16_MAX));
    CHECK_FALSE(owner->getTypeInfo("Road", false));
    CHECK(owner->featureIdentities().empty());
    CHECK(layer->featureTypes().empty());
}

TEST_CASE(
    "Failed layer imports restore canonical layer and dictionary tails",
    "[mapschema][metadata]")
{
    auto owner = std::make_shared<MapSchema>();
    auto dictionary = owner->strings();
    while (dictionary->highest() < UINT16_MAX - 1)
        REQUIRE(dictionary->emplace("padding-" + std::to_string(dictionary->highest())));
    auto before = dictionary->size();
    nlohmann::json input{
        {"type", "object"},
        {"properties", {{"unallocatedField", {{"type", "integer"}}}}}};
    REQUIRE_THROWS(owner->importLayer("LateLayer", LayerType::SourceData, input));
    CHECK(owner->layerNames().empty());
    CHECK_FALSE(owner->layer("LateLayer"));
    CHECK(owner->typeCount() == 0);
    CHECK(dictionary->size() == before);
    CHECK(dictionary->get("LateLayer") == simfil::StringPool::Empty);
    CHECK(dictionary->get("unallocatedField") == simfil::StringPool::Empty);
}

TEST_CASE(
    "Native semantic views preserve pool ownership and mutability",
    "[mapschema][native-views]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Road", LayerType::Features);
    auto feature = layer->newFeature(identity("Road"));
    auto integer = owner->schemaType(owner->primitive(simfil::SchemaType::Kind::Int));
    CHECK(feature.pool() == owner.get());
    CHECK(feature.identity().name() == "Road");
    CHECK_FALSE(feature.ownsPool());
    feature.properties().addField("length", integer);
    CHECK(feature.properties().field("length").type() == integer);
    auto retained = feature.retain();
    CHECK(retained.ownsPool());
    CHECK(retained.properties().ownsPool());
    CHECK(retained.attributeLayers().ownsPool());
    auto readOnly = FeatureSchemaType(std::as_const(*owner).schemaType(feature.id()));
    CHECK_THROWS(readOnly.properties().addField("forbidden", integer));
    CHECK_FALSE(feature.properties().field("forbidden"));
    CHECK_THROWS(feature.addField("invented", integer));
    auto clone = owner->cloneType(feature);
    clone.setTypeName("A producer provenance label, not the identity");
    CHECK(FeatureSchemaType(clone).identity().name() == "Road");
    CHECK_THROWS(clone.asObject().addField("invented", integer));
    CHECK_THROWS(clone.asObject().addAlias("otherProperties", "properties"));
    CHECK_THROWS(clone.asObject().setBase(owner->newObjectType()));
}

TEST_CASE("Native reservations guard every generic write path", "[mapschema][native-views]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Road", LayerType::Features);
    auto feature = layer->newFeature(identity("Road"));
    auto attribute = owner->newAttribute({}, "Speed");
    auto integer = owner->schemaType(owner->primitive(simfil::SchemaType::Kind::Int));
    attribute.addField("speed", integer);
    CHECK_THROWS(attribute.addField("validity", attribute.validities()));
    CHECK_THROWS(attribute.field("validity").setRequired(true));
    CHECK_THROWS(attribute.field("validity").setRole(simfil::SchemaType::FieldRole::Context));
    CHECK_THROWS(attribute.field("validity").setMultimap(true));
    CHECK_THROWS(attribute.field("validity").addType(integer));
    CHECK_THROWS(attribute.addAlias("mask", "validity"));
    CHECK_THROWS(attribute.setComposition(simfil::SchemaType::Composition::AnyOf));
    CHECK_THROWS(attribute.setKindName(simfil::StringPool::SchemaObject));
    CHECK_THROWS(feature.properties().addField("layer", feature.attributeLayers()));
    CHECK_THROWS(feature.properties().setBase(owner->newObjectType()));
    CHECK_THROWS(feature.attributeLayers().setComposition(simfil::SchemaType::Composition::AnyOf));
    CHECK_THROWS(feature.attributeLayers().setKindName(simfil::StringPool::SchemaArray));
    auto cloned = AttributeSchemaType(owner->cloneType(attribute));
    cloned.addField(
        "displayUnit",
        owner->schemaType(owner->primitive(simfil::SchemaType::Kind::String)));
    CHECK_THROWS(cloned.field("_sourceData").setRequired(true));
}

TEST_CASE(
    "Native operations reject foreign identities before mutating their owner",
    "[mapschema][native-views]")
{
    auto owner = std::make_shared<MapSchema>();
    auto foreign = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Road", LayerType::Features);
    auto object = foreign->newObjectType();
    auto count = owner->typeCount();
    CHECK_THROWS(layer->newFeature(identity("Road"), object));
    CHECK(owner->typeCount() == count);
    CHECK_FALSE(owner->getTypeInfo("Road", false));
    auto feature = layer->newFeature(identity("Road"));
    auto attribute = foreign->newAttribute();
    count = owner->typeCount();
    CHECK_THROWS(layer->bindAttribute(attribute, "Road", "Rules", "SPEED"));
    CHECK(owner->typeCount() == count);
    CHECK_FALSE(feature.attributeLayers().field("Rules"));
}

TEST_CASE(
    "Native attribute binding creates consistent edges and clone provenance",
    "[mapschema][native-views]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Road", LayerType::Features);
    auto feature = layer->newFeature(identity("Road"));
    auto attribute = owner->newAttribute("physical-speed", "Speed");
    attribute.addField("speed", owner->schemaType(owner->primitive(simfil::SchemaType::Kind::Int)));
    layer->bindAttribute(attribute, "Road", "Rules", "SPEED");
    auto container = feature.attributeLayers().field("Rules").type();
    REQUIRE(container);
    CHECK(container.field("SPEED").type() == attribute);
    CHECK(container.field("SPEED").multimap());
    auto count = owner->typeCount();
    layer->bindAttribute(attribute, "Road", "Rules", "SPEED");
    CHECK(owner->typeCount() == count);
    CHECK(container.field("SPEED").types().size() == 1);
    auto clone = AttributeSchemaType(owner->cloneType(attribute));
    CHECK(clone.typeName() == "Speed");
    CHECK(layer->attributeTypeCode(clone.id()).empty());
    CHECK(layer->attributeOwners(clone.id()).empty());
    CHECK(owner->findSchema("physical-speed") == attribute.id());
    CHECK_THROWS(layer->bindAttribute(clone, "Road", "Rules", "SPEED"));
    CHECK(container.field("SPEED").type() == attribute);
    CHECK(layer->attributeOwners(clone.id()).empty());
    layer->bindAttribute(clone, "Road", "Rules", "SECOND_SPEED");
    CHECK(layer->attributeOwners(clone.id()).size() == 1);
    CHECK(layer->attributeOwners(attribute.id()).size() == 1);
    owner->finalize();
    auto restored = decode(*owner)->layer("Road");
    CHECK(restored->attributeOwners(attribute.id()).size() == 1);
    CHECK(restored->attributeOwners(clone.id()).size() == 1);
}

TEST_CASE(
    "Attribute unions retain their logical kind across query and transport",
    "[mapschema][native-views]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Road", LayerType::Features);
    (void)layer->newFeature(identity("Road"));
    auto integer = owner->schemaType(owner->primitive(simfil::SchemaType::Kind::Int));
    auto first = owner->newAttribute({}, "SpeedA");
    auto second = owner->newAttribute({}, "SpeedB");
    first.addField("speed", integer);
    second.addField("speed", integer);
    auto alternatives = owner->newCompositionType(simfil::SchemaType::Composition::AnyOf);
    alternatives.addAlternative(first);
    alternatives.addAlternative(second);
    auto kind = alternatives.kind();
    layer->bindAttribute(alternatives, "Road", "Rules", "SPEED");
    CHECK(alternatives.kind() == kind);
    owner->finalize();
    // Finalization narrows structural affinities; binding never changes the logical kind.
    kind = alternatives.kind();
    auto json = LayerSchema::fromJsonSchema(layer->toJsonSchema());
    for (auto candidate : {layer, decode(*owner)->layer("Road"), json}) {
        auto result =
            candidate->normalizeSearchQuery("SPEED", LayerSchema::SearchQueryRequestedScope::Auto);
        REQUIRE(result);
        CHECK(result->concreteScope_ == LayerSchema::SearchQueryConcreteScope::Attribute);
        REQUIRE(result->attributeScopes_.size() == 1);
        auto root =
            candidate->mapSchema()->schemaType(result->attributeScopes_[0].attributeSchema_);
        CHECK(root.composition() == simfil::SchemaType::Composition::AnyOf);
        CHECK(root.kind() == kind);
    }
}

TEST_CASE(
    "Imported native envelopes acquire their lookup alias before sealing",
    "[mapschema][native-views]")
{
    auto source = std::make_shared<MapSchema>();
    auto layer = source->newLayer("Road", LayerType::Features);
    (void)layer->newFeature(identity("Road"));
    source->finalize();
    auto json = layer->toJsonSchema();
    auto omitAliases = [&](auto&& self, nlohmann::json& node) -> void
    {
        if (node.is_object()) {
            if (auto meta = node.find("x-mapget"); meta != node.end() && meta->is_object())
                meta->erase("fieldAliases");
        }
        if (node.is_structured())
            for (auto& child : node)
                self(self, child);
    };
    omitAliases(omitAliases, json);
    auto owner = std::make_shared<MapSchema>();
    std::array<FeatureTypeInfo, 1> identities{identity("Road")};
    auto imported = owner->importLayer("Road", LayerType::Features, json, identities);
    auto feature = FeatureSchemaType(owner->schemaType(imported->featureSchema("Road")));
    CHECK(feature.field("attributes").type() == feature.properties());
    CHECK_THROWS(feature.properties().field("layer").setRequired(true));
    CHECK_NOTHROW(owner->finalize());
}

TEST_CASE(
    "Native binding rejects thin Attributes and protects adopted complete shapes",
    "[mapschema][native-views]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Road", LayerType::Features);
    auto feature = layer->newFeature(identity("Road"));
    auto prototype = owner->newAttribute();
    auto thin = owner->newObjectType(LayerSchema::AttributeKind);
    auto mixed = owner->newCompositionType(simfil::SchemaType::Composition::AnyOf);
    mixed.addAlternative(prototype);
    mixed.addAlternative(thin);
    auto names = owner->dictionary().size();
    CHECK_THROWS(layer->bindAttribute(thin, "Road", "Rules", "THIN"));
    CHECK_THROWS(layer->bindAttribute(mixed, "Road", "Rules", "MIXED"));
    CHECK(owner->dictionary().size() == names);
    CHECK_FALSE(feature.attributeLayers().field("Rules"));
    CHECK(layer->attributeOwners(thin.id()).empty());
    CHECK(layer->attributeOwners(mixed.id()).empty());

    auto manual = owner->newObjectType(LayerSchema::AttributeKind);
    manual.addField("validity", prototype.validities()).setRequired(false);
    manual.addField("_sourceData", prototype.sourceDataReferences()).setRequired(false);
    auto inner = owner->newCompositionType(simfil::SchemaType::Composition::AnyOf);
    inner.addAlternative(manual);
    inner.addAlternative(prototype);
    auto outer = owner->newCompositionType(simfil::SchemaType::Composition::OneOf);
    outer.addAlternative(inner);
    outer.addAlternative(inner);
    layer->bindAttribute(outer, "Road", "Rules", "COMPLETE");
    REQUIRE(outer.alternatives().size() == 2);
    CHECK(outer.alternatives()[0] == inner.id());
    CHECK(outer.alternatives()[1] == inner.id());
    CHECK_THROWS(manual.field("validity").setRequired(true));
    CHECK_THROWS(manual.addAlias("alternateSource", "_sourceData"));
    CHECK_THROWS(inner.addAlternative(thin));
    CHECK_THROWS(outer.addAlternative(thin));
    CHECK_THROWS(outer.asObject().addField("validity", prototype.validities()));
    manual.addField(
        "providerField",
        owner->schemaType(owner->primitive(simfil::SchemaType::Kind::Int)));

    owner->finalize();
    auto importedOwner = std::make_shared<MapSchema>();
    std::array<FeatureTypeInfo, 1> identities{identity("Road")};
    auto imported =
        importedOwner->importLayer("Road", LayerType::Features, layer->toJsonSchema(), identities);
    auto importedFeature =
        FeatureSchemaType(importedOwner->schemaType(imported->featureSchema("Road")));
    auto importedOuter =
        importedFeature.attributeLayers().field("Rules").type().field("COMPLETE").type();
    CHECK(importedOuter.composition() == simfil::SchemaType::Composition::OneOf);
    auto importedInner = importedOwner->schemaType(importedOuter.alternatives()[0]);
    auto importedManual = importedOwner->schemaType(importedInner.alternatives()[0]);
    CHECK_THROWS(importedManual.field("validity").setRequired(true));
    CHECK_THROWS(importedInner.asComposition().addAlternative(importedOwner->newObjectType()));
    CHECK_NOTHROW(importedOwner->finalize());
}

TEST_CASE(
    "Native binding rejects malformed physical Attribute envelopes",
    "[mapschema][native-views]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Road", LayerType::Features);
    auto feature = layer->newFeature(identity("Road"));
    auto prototype = owner->newAttribute();
    auto malformed = owner->newObjectType(LayerSchema::AttributeKind);
    malformed.addField("validity", prototype.validities()).setRequired(false);
    malformed.addField("_sourceData", prototype.sourceDataReferences()).setRequired(false);
    SECTION("Common members must be enumerable data")
    {
        malformed.field("validity").setRole(simfil::SchemaType::FieldRole::Context);
    }
    SECTION("Common members must not be multimaps")
    {
        malformed.field("_sourceData").setMultimap(true);
    }
    SECTION("Even duplicate identical member targets are not a physical envelope")
    {
        malformed.field("validity").addType(prototype.validities());
    }
    SECTION("A complete-looking wrapper cannot hide a thin alternative")
    {
        malformed.setComposition(simfil::SchemaType::Composition::AnyOf);
        malformed.asComposition().addAlternative(owner->newObjectType(LayerSchema::AttributeKind));
    }
    SECTION("Discarding the composition must not leave hidden alternatives")
    {
        malformed.setComposition(simfil::SchemaType::Composition::AnyOf);
        malformed.asComposition().addAlternative(prototype);
        malformed.setComposition(simfil::SchemaType::Composition::None);
    }
    CHECK_THROWS(AttributeSchemaType(malformed));
    auto names = owner->dictionary().size();
    CHECK_THROWS(layer->bindAttribute(malformed, "Road", "Malformed", "VALUE"));
    CHECK(owner->dictionary().size() == names);
    CHECK_FALSE(feature.attributeLayers().field("Malformed"));
    CHECK(layer->attributeOwners(malformed.id()).empty());
}

TEST_CASE(
    "Native semantic kinds remain independent of nullable affinities",
    "[mapschema][native-views]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Road", LayerType::Features);
    auto feature = layer->newFeature(identity("Road"));
    auto attribute = owner->newAttribute({}, "Speed");
    attribute.addField("speed", owner->schemaType(owner->primitive(simfil::SchemaType::Kind::Int)));
    auto optional = AttributeSchemaType(owner->schemaType(owner->nullable(attribute.id())));
    CHECK(optional.nullable() == true);
    CHECK(optional.hasAffinity(simfil::ValueType::Null));
    CHECK(optional.hasAffinity(simfil::ValueType::Object));
    layer->bindAttribute(optional, "Road", "Rules", "OPTIONAL_SPEED");
    CHECK(layer->attributeOwners(optional.id()).size() == 1);
    CHECK(layer->attributeOwners(attribute.id()).empty());
    CHECK_THROWS(optional.field("validity").setRole(simfil::SchemaType::FieldRole::Context));

    auto clone = owner->cloneType(feature);
    clone.setNullable(true);
    CHECK(FeatureSchemaType(clone).identity().name() == "Road");
    CHECK_THROWS(clone.asObject().addField("invented", optional));
    CHECK_FALSE(clone.field("invented"));
    owner->finalize();
    auto decoded = decode(*owner);
    CHECK(AttributeSchemaType(decoded->schemaType(optional.id())).nullable() == true);
    CHECK(FeatureSchemaType(decoded->schemaType(clone.id())).nullable() == true);

    auto importedOwner = std::make_shared<MapSchema>();
    std::array<FeatureTypeInfo, 1> identities{identity("Road")};
    auto imported =
        importedOwner->importLayer("Road", LayerType::Features, layer->toJsonSchema(), identities);
    auto importedFeature =
        FeatureSchemaType(importedOwner->schemaType(imported->featureSchema("Road")));
    auto importedAttribute = AttributeSchemaType(
        importedFeature.attributeLayers().field("Rules").type().field("OPTIONAL_SPEED").type());
    CHECK(importedAttribute.nullable() == true);
    CHECK_THROWS(importedAttribute.field("validity").setMultimap(true));
    CHECK_NOTHROW(importedOwner->finalize());
}

TEST_CASE(
    "Thin nullable legacy Feature roots retain attribute query discovery",
    "[mapschema][native-views]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Legacy", LayerType::Features);
    // This declaration is intentionally not a complete native Feature envelope.
    // Legacy query discovery must follow its semantic kind, independently of nullability.
    auto feature = owner->newObjectType(LayerSchema::FeatureKind);
    feature.setTypeName("Road");
    feature.setNullable(true);
    auto properties = owner->newObjectType();
    auto layers = owner->newObjectType();
    auto rules = owner->newObjectType();
    auto attribute = owner->newAttribute({}, "Speed");
    attribute.addField("speed", owner->schemaType(owner->primitive(simfil::SchemaType::Kind::Int)));
    feature.addField("properties", properties);
    properties.addField("layer", layers);
    layers.addField("Rules", rules).setMultimap(true);
    rules.addField("SPEED", attribute).setMultimap(true);
    layer->registerSchemaKey(LayerSchema::featureKey("Road"), feature.id());
    layer->setAttributeMetadata(attribute.id(), {"Road", "Rules", "SPEED", attribute.id()}, {});
    layer->setRootSchema(feature.id());
    CHECK_THROWS(FeatureSchemaType(feature));
    owner->finalize();
    for (auto candidate :
         {layer,
          decode(*owner)->layer("Legacy"),
          LayerSchema::fromJsonSchema(layer->toJsonSchema())})
    {
        auto root = candidate->rootType();
        CHECK(root.nullable() == true);
        auto payload =
            root.field("properties")
                .type()
                .field("layer")
                .type()
                .field("Rules")
                .type()
                .field("SPEED")
                .type();
        REQUIRE(payload);
        CHECK(candidate->attributeQuerySchema("Road", payload.id()) != simfil::NoTypeId);
        auto scopes = candidate->attributeScopes();
        REQUIRE(scopes.size() == 1);
        CHECK(scopes[0].featureType_ == "Road");
        CHECK(scopes[0].attributeName_ == "SPEED");
    }
}
