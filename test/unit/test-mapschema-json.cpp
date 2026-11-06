#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "mapget/model/mapschema.h"
#include "simfil/model/schema-type.h"

#include <nlohmann/json-schema.hpp>

using namespace mapget;
using Json = nlohmann::json;
using Kind = simfil::SchemaType::Kind;

TEST_CASE(
    "JSON preserves unbased object and array field declaration order",
    "[mapschema][schema-json]")
{
    auto array = GENERATE(false, true);
    auto schema = std::make_shared<MapSchema>();
    auto layer = schema->newLayer("Data", LayerType::SourceData);
    auto type = schema->newType(array ? Kind::Array : Kind::Object);
    auto number = schema->newScalarType(Kind::Float);
    layer->setRootSchema(type.id());
    for (auto name : {"zeta", "alpha"}) {
        auto field = array ?
            type.asArray().addField(name, number) :
            type.asObject().addField(name, number);
        if (array)
            field.setRole(simfil::SchemaType::FieldRole::Context);
    }
    if (array)
        type.asArray().addElement(number);
    schema->finalize();
    auto json = layer->toJsonSchema();
    CHECK(
        json["definitions"][std::to_string(type.id())]["x-mapget"]["directFields"] ==
        Json::array({"zeta", "alpha"}));
    auto copy = LayerSchema::fromJsonSchema(json);
    CHECK(copy->toJsonSchema() == json);
    auto restored = copy->mapSchema()->schemaType(copy->rootSchema());
    std::vector<std::string> names;
    restored.forEachDirectField([&](auto const& field) { names.emplace_back(field.name()); });
    CHECK(names == std::vector<std::string>{"zeta", "alpha"});
    CHECK_FALSE(restored.baseType());
}

TEST_CASE(
    "Sealed native shapes accept atomic descriptive annotations before publication",
    "[mapschema][schema-json][schema-native]")
{
    auto schema = std::make_shared<MapSchema>();
    auto layer = schema->newLayer("Roads", LayerType::Features);
    auto feature = layer->newFeature(FeatureTypeInfo::fromJson(
        {{"name", "Road"},
         {"uniqueIdCompositions", {{{{"partId", "roadId"}, {"datatype", "U32"}}}}}}));
    schema->setJsonSchemaAnnotations(
        feature.id(),
        {{"title", "Road"}, {"description", "A native sealed feature."}});
    auto before = layer->toJsonSchema();
    auto const key = std::to_string(feature.id());
    CHECK(before["definitions"][key]["title"] == "Road");
    CHECK(before["definitions"][key]["description"] == "A native sealed feature.");
    CHECK_THROWS(feature.addField("forbidden", feature.properties()));

    auto count = schema->typeCount();
    auto strings = schema->strings()->size();
    CHECK_THROWS(schema->setJsonSchemaAnnotations(feature.id(), Json::parse(R"({
        "description":"not published",
        "if":{"type":"object", "properties":{"transientMember":{"type":"string"}}},
        "then":{"$ref":"https://invalid.example/schema"}
    })")));
    CHECK(schema->typeCount() == count);
    CHECK(schema->strings()->size() == strings);
    CHECK(layer->toJsonSchema() == before);

    auto integer = schema->primitive(Kind::Int);
    schema->setJsonSchemaAnnotations(integer, {{"description", "A sealed scalar."}});
    CHECK_THROWS(schema->setJsonSchemaAnnotations(integer, {{"minimum", 1}}));
    CHECK_FALSE(schema->schemaType(integer).numericDomain());
    schema->finalize();
    CHECK_THROWS(schema->setJsonSchemaAnnotations(feature.id(), {{"title", "Too late"}}));
    auto exported = layer->toJsonSchema();
    CHECK(exported["definitions"][key]["title"] == "Road");
    auto restored = LayerSchema::fromJsonSchema(exported);
    CHECK(restored->toJsonSchema() == exported);
}

TEST_CASE("JSON scalar annotations preserve absent native semantics", "[mapschema][schema-json]")
{
    auto schema = std::make_shared<MapSchema>();
    auto layer = schema->newLayer("Values", LayerType::SourceData);
    auto id = layer->addSchema(Kind::Int, "Value");
    layer->setRootSchema(id);
    schema->schemaType(id).asScalar().setNumericDomain(
        {.encoding = "uint64", .unit = "m", .minimum = uint64_t{0}, .maximum = UINT64_MAX});
    schema->schemaType(id).asScalar().addNamedValue(
        {.name = "unknown",
         .value = uint64_t{UINT64_MAX},
         .role = simfil::SchemaType::NamedValue::Role::Special,
         .meaning = simfil::SchemaType::NamedValue::Meaning::Unknown});
    schema->setJsonSchemaAnnotations(id, {{"description", "Preserve native numeric metadata."}});
    auto numeric = schema->schemaType(id).numericDomain();
    REQUIRE(numeric);
    CHECK(numeric->unit == "m");
    CHECK(std::get<uint64_t>(*numeric->maximum) == UINT64_MAX);
    REQUIRE(schema->schemaType(id).namedValue("unknown"));
    Json annotation{
        {"x-mapget",
         {{"namedValues",
           Json::array(
               {{{"name", "unknown"},
                 {"value", UINT64_MAX},
                 {"role", "special"},
                 {"meaning", "unknown"}}})}}}};
    CHECK_NOTHROW(schema->setJsonSchemaAnnotations(id, annotation));
    auto duplicate = annotation;
    duplicate["x-mapget"]["namedValues"].push_back(duplicate["x-mapget"]["namedValues"][0]);
    CHECK_THROWS(schema->setJsonSchemaAnnotations(id, duplicate));
    auto conflict = annotation;
    conflict["x-mapget"]["namedValues"][0]["value"] = uint64_t{2};
    CHECK_THROWS(schema->setJsonSchemaAnnotations(id, conflict));
    CHECK_THROWS(
        schema->setJsonSchemaAnnotations(id, {{"x-mapget", {{"numericDomain", nullptr}}}}));
    schema->finalize();
    auto restored = LayerSchema::fromJsonSchema(layer->toJsonSchema());
    auto readback = restored->mapSchema()->schemaType(restored->rootSchema()).numericDomain();
    REQUIRE(readback);
    CHECK(std::get<uint64_t>(*readback->maximum) == UINT64_MAX);
    CHECK(readback->unit == "m");
}

TEST_CASE(
    "JSON annotation failure rolls back nested types and scalar replacement",
    "[mapschema][schema-json]")
{
    auto schema = std::make_shared<MapSchema>();
    auto layer = schema->newLayer("Values", LayerType::SourceData);
    auto id = layer->addSchema(Kind::Int, "Value");
    layer->setRootSchema(id);
    schema->setJsonSchemaAnnotations(id, {{"description", "original"}, {"minimum", 1}});
    auto before = layer->toJsonSchema();
    auto count = schema->typeCount();
    auto strings = schema->strings()->size();
    auto broken = Json::parse(R"({
        "description":"not published",
        "minimum":12,
        "if":{"type":"object", "properties":{"transientMember":{"type":"string"}}},
        "then":{"$ref":"https://invalid.example/schema"}
    })");
    CHECK_THROWS(schema->setJsonSchemaAnnotations(id, broken));
    CHECK(schema->typeCount() == count);
    CHECK(schema->strings()->size() == strings);
    CHECK(layer->toJsonSchema() == before);
    CHECK(schema->findSchema("#/import/2") == simfil::NoTypeId);
    // Reusing the rolled-back text and alias must not retrieve a retired model address.
    auto added = schema->addJsonSchema(
        {{"type", "object"}, {"properties", {{"transientMember", {{"type", "string"}}}}}});
    REQUIRE(schema->schemaType(added).field("transientMember"));
    CHECK_NOTHROW(schema->finalize());
}

TEST_CASE("JSON bit-width updates are explicit and atomic", "[mapschema][schema-json]")
{
    auto schema = std::make_shared<MapSchema>();
    auto layer = schema->newLayer("Masks", LayerType::SourceData);
    auto id = layer->addSchema(LayerSchema::BitmaskKind, "Mask");
    layer->setRootSchema(id);
    auto mask = schema->schemaType(id).asBitmask();
    mask.setBitWidth(8);
    mask.addFlag("HIGH", 128);
    schema->setJsonSchemaAnnotations(id, {{"description", "Still an eight-bit mask."}});
    CHECK(mask.bitWidth() == 8);
    for (auto width : Json::array({0, -1, 65, 1.5, 7})) {
        CHECK_THROWS(schema->setJsonSchemaAnnotations(id, {{"x-mapget", {{"bitWidth", width}}}}));
        CHECK(mask.bitWidth() == 8);
        REQUIRE(mask.namedValue("HIGH"));
    }
    schema->setJsonSchemaAnnotations(id, {{"x-mapget", {{"bitWidth", 16}}}});
    CHECK(mask.bitWidth() == 16);
    schema->finalize();
    auto restored = LayerSchema::fromJsonSchema(layer->toJsonSchema());
    CHECK(restored->mapSchema()->schemaType(restored->rootSchema()).bitWidth() == 16);
}

TEST_CASE(
    "JSON preserves unknown edges presence aliases and canonical bases",
    "[mapschema][schema-json]")
{
    auto schema = std::make_shared<MapSchema>();
    auto layer = schema->newLayer("Objects", LayerType::SourceData);
    auto base = layer->addSchema(Kind::Object, "Base");
    auto object = layer->addSchema(Kind::Object, "Derived");
    auto scalar = layer->addSchema(Kind::Int, "Value");
    auto array = layer->addSchema(Kind::Array, "Array");
    layer->setRootSchema(object);
    schema->schemaType(base).asObject().declareField("unknown").setRequired(std::nullopt);
    schema->schemaType(base).asObject().addField("value", schema->schemaType(scalar));
    schema->schemaType(object).asObject().setBase(schema->schemaType(base).asObject());
    schema->schemaType(object).asObject().addAlias("alias", "value");
    schema->schemaType(object).asObject().addField("mixed");
    schema->schemaType(object).field("mixed").addType(schema->schemaType(scalar));
    schema->schemaType(object).asObject().addField("unknownTarget");
    schema->schemaType(object).asObject().declareField("emptyMultimap").setRequired(std::nullopt);
    schema->schemaType(object).field("emptyMultimap").setMultimap(true);
    schema->schemaType(object).asObject().addField("items", schema->schemaType(array));
    schema->schemaType(array).asArray().addElement({});
    schema->finalize();
    auto exported = layer->toJsonSchema();
    auto copy = LayerSchema::fromJsonSchema(exported);
    auto root = copy->mapSchema()->schemaType(copy->rootSchema());
    REQUIRE(root.baseType());
    std::vector<std::string> directFields;
    root.forEachDirectField([&](auto field) { directFields.emplace_back(field.name()); });
    CHECK(
        directFields ==
        std::vector<std::string>{"mixed", "unknownTarget", "emptyMultimap", "items"});
    std::vector<std::string> baseFields;
    root.baseType().forEachDirectField([&](auto field) { baseFields.emplace_back(field.name()); });
    CHECK(baseFields == std::vector<std::string>{"unknown", "value"});
    REQUIRE(root.field("unknown"));
    CHECK_FALSE(root.field("unknown").required().has_value());
    CHECK(root.field("unknown").types().empty());
    CHECK(root.field("alias").type() == root.field("value").type());
    auto mixed = root.field("mixed").types();
    REQUIRE(mixed.size() == 2);
    CHECK(mixed.front() == simfil::NoTypeId);
    CHECK(mixed[1] == root.field("value").type().id());
    REQUIRE(root.field("unknownTarget").types().size() == 1);
    CHECK(root.field("unknownTarget").types().front() == simfil::NoTypeId);
    REQUIRE(root.field("items").type().elements().size() == 1);
    CHECK(root.field("items").type().elements().front() == simfil::NoTypeId);
    REQUIRE(root.field("emptyMultimap").types().empty());
    CHECK(root.field("emptyMultimap").multimap());
    CHECK(copy->toJsonSchema() == exported);
    for (auto const& json : {exported, copy->toJsonSchema()}) {
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(json);
        CHECK_NOTHROW(validator.validate({{"value", 3}}));
        CHECK_THROWS(validator.validate({{"value", "not numeric"}}));
        CHECK_THROWS(validator.validate({{"unexpected", 3}}));
    }
    for (auto invalid : Json::array(
             {"mixed", Json::array({1}), Json::array({"absent"}), Json::array({"mixed", "mixed"})}))
    {
        auto malformed = exported;
        malformed["definitions"][std::to_string(object)]["x-mapget"]["directFields"] = invalid;
        CHECK_THROWS(LayerSchema::fromJsonSchema(malformed));
    }
}

TEST_CASE(
    "Direct generic factories remain exportable through MapSchema",
    "[mapschema][schema-json]")
{
    auto schema = std::make_shared<MapSchema>();
    auto layer = schema->newLayer("Direct", LayerType::SourceData);
    auto object = schema->newObjectType();
    auto number = schema->newScalarType(Kind::Int);
    object.addField("number", number);
    layer->setRootSchema(object.id());
    CHECK_NOTHROW(layer->toJsonSchema());
    schema->finalize();
    auto json = layer->toJsonSchema();
    auto copy = LayerSchema::fromJsonSchema(json);
    CHECK(copy->toJsonSchema() == json);
    CHECK(
        copy->mapSchema()->schemaType(copy->rootSchema()).field("number").type().kind() ==
        Kind::Int);
}

TEST_CASE(
    "Imported and sealed Attribute payloads accept additional use sites",
    "[mapschema][schema-json]")
{
    auto schema = std::make_shared<MapSchema>();
    auto layer = schema->importLayer("Attributes", LayerType::SourceData, Json::parse(R"({
        "type":"object", "x-mapget":{
            "metaType":"Attribute", "schemaKey":"PhysicalAttribute",
            "attributeOwners":[{"featureType":"Road", "layer":"rules", "name":"FIRST"}]
        }, "properties":{"value":{"type":"integer"}}
    })"));
    auto root = layer->rootSchema();
    REQUIRE(layer->attributeOwners(root).size() == 1);
    // Sealing the physical root must not freeze the separate use-site inventory.
    auto nullable = schema->nullable(root);
    REQUIRE(nullable != root);
    // A distinct nullable declaration inherits shape, not its source's named uses.
    REQUIRE(layer->attributeOwners(nullable).empty());
    CHECK_NOTHROW(layer->setAttributeMetadata(root, {"Lane", "otherRules", "SECOND", root}, {}));
    CHECK_NOTHROW(
        layer->setAttributeMetadata(nullable, {"Lane", "otherRules", "OPTIONAL", nullable}, {}));
    CHECK_THROWS(schema->schemaType(root).asObject().addField("forbidden", schema->schemaType(0)));
    REQUIRE(layer->attributeOwners(root).size() == 2);
    REQUIRE(layer->attributeOwners(nullable).size() == 1);
    CHECK(layer->attributeOwners(nullable).front().attributeName_ == "OPTIONAL");
    schema->finalize();
    auto restored = std::make_shared<MapSchema>();
    auto copy = restored->importLayer("Attributes", LayerType::SourceData, layer->toJsonSchema());
    restored->finalize();
    CHECK(copy->attributeOwners(copy->rootSchema()).size() == 2);
    CHECK(copy->toJsonSchema() == layer->toJsonSchema());
    auto renamed = LayerSchema::fromJsonSchema(layer->toJsonSchema());
    REQUIRE(renamed->attributeOwners(renamed->rootSchema()).size() == 2);
    for (auto const& site : renamed->attributeOwners(renamed->rootSchema()))
        CHECK(site.featureLayer_ == "legacy");
}

TEST_CASE(
    "JSON boundary accepts registered application kinds without another kind registry",
    "[mapschema][schema-json]")
{
    constexpr simfil::StringId APPLICATION_KIND = 240;
    auto source = std::make_shared<MapSchema>();
    source->strings()->addStaticKey(APPLICATION_KIND, "ApplicationRecord");
    auto layer = source->newLayer("Records", LayerType::SourceData);
    auto kind = simfil::SchemaType::makeKind(
        APPLICATION_KIND,
        simfil::valueTypeAffinity(simfil::ValueType::Object));
    auto root = layer->addSchema(kind);
    layer->setRootSchema(root);
    source->finalize();
    auto json = layer->toJsonSchema();
    CHECK(
        json["definitions"][std::to_string(root)]["x-mapget"]["kind"]["name"] ==
        "ApplicationRecord");
    auto destination = std::make_shared<MapSchema>();
    destination->strings()->addStaticKey(APPLICATION_KIND, "ApplicationRecord");
    auto copied = destination->importLayer("Records", LayerType::SourceData, json);
    destination->finalize();
    CHECK(destination->schemaType(copied->rootSchema()).kind() == kind);
    CHECK(copied->toJsonSchema() == json);
    CHECK_THROWS(LayerSchema::fromJsonSchema(json));
}
