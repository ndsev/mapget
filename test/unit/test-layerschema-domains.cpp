#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json-schema.hpp>

#include "mapget/model/layerschema.h"
#include "mapget/model/simfilutil.h"
#include "simfil/model/json.h"
#include "simfil/model/schema-model.h"
#include "simfil/simfil.h"

using namespace mapget;
using Kind = simfil::Schema::Kind;
using Composition = simfil::Schema::Composition;

namespace
{

/** Bind the same typed graph used by completion to a bounded metadata descriptor. */
nlohmann::json describe(std::shared_ptr<LayerSchema const> registry, simfil::SchemaId root)
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
    simfil::SchemaId root,
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
    "Native schema transport preserves validation constraints and JSON projections",
    "[DataSourceInfo][schema-domain]")
{
    auto schema = std::make_shared<LayerSchema>();
    auto root = schema->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
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
    schema->addEnumSymbols(flags, std::vector<std::string>{"LEFT", "RIGHT"});
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
    auto restored = LayerSchema::fromJsonSchema(exported);
    // Discard the imported JSON cache: this must exercise export from the restored graph.
    restored->finalize();
    REQUIRE(restored->toJsonSchema() == exported);
    REQUIRE(describe(restored, root) == describe(schema, root));
    REQUIRE(restored->childSchema(root, "attribute") == attribute);
    REQUIRE(restored->kind(flags) == LayerSchema::BitmaskKind);
    REQUIRE(restored->kind(bytes) == Kind::Bytes);
    REQUIRE(
        std::ranges::find(restored->directFields(root), "_multimap") ==
        restored->directFields(root).end());
    REQUIRE(
        exported["definitions"][std::to_string(flags)]["x-mapget"]["bitmaskValues"].size() == 2);
    REQUIRE_FALSE(exported["definitions"][std::to_string(flags)].contains("enum"));

    nlohmann::json_schema::json_validator validator;
    REQUIRE_NOTHROW(validator.set_root_schema(exported));
    REQUIRE_NOTHROW(validator.validate(R"({"id":2,"attribute":{"label":"road"},"flags":"LEFT|RIGHT",
        "bytes":{"_bytes":true,"hex":"abcd","number":43981}})"_json));
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
    "Converter fragments share native domains but keep local references isolated",
    "[schema-domain]")
{
    auto schema = std::make_shared<LayerSchema>();
    auto shared = schema->addSchema(Kind::String, "shared");
    auto root = schema->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
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
    auto restored = LayerSchema::fromJsonSchema(exported);
    restored->finalize();
    REQUIRE(restored->toJsonSchema() == exported);
    nlohmann::json_schema::json_validator validator;
    REQUIRE_NOTHROW(validator.set_root_schema(exported));
    REQUIRE_NOTHROW(validator.validate(R"({"first":{"value":3},"second":{"value":"ON"}})"_json));
    REQUIRE_THROWS(validator.validate(R"({"first":{"value":8}})"_json));
}

TEST_CASE(
    "Schema snapshots preserve imported validation and finalization refreshes native export",
    "[schema-domain]")
{
    auto imported = LayerSchema::fromJsonSchema(R"({"type":"object","if":{"required":["mode"]},
        "then":{"required":["value"]},"properties":{"mode":{"type":"string"}}})"_json);
    REQUIRE(imported->detachedCopy()->toJsonSchema() == imported->toJsonSchema());
    auto native = std::make_shared<LayerSchema>();
    auto root = native->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
    native->finalize();
    auto snapshot = native->detachedCopy();
    auto before = native->toJsonSchema();
    native->addFieldSchema(root, "added", native->addSchema(Kind::Int));
    native->finalize();
    REQUIRE(native->toJsonSchema() != before);
    REQUIRE(snapshot->toJsonSchema() == before);
}

TEST_CASE(
    "LayerSchema retains typed JSON domains rather than flattening combiners",
    "[DataSourceInfo][schema-domain]")
{
    auto schema = LayerSchema::fromJsonSchema(R"({
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
    })"_json);
    auto root = schema->featureSchema("Thing");
    REQUIRE(root != simfil::NoSchemaId);
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
    auto roundtrip = LayerSchema::fromJsonSchema(schema->toJsonSchema());
    REQUIRE(roundtrip->featureSchema("Thing") == root);
    REQUIRE(describe(roundtrip, root) == json);
}

TEST_CASE(
    "Direct rich LayerSchema transport preserves IDs metadata and plural edges",
    "[DataSourceInfo][schema-domain]")
{
    auto schema = std::make_shared<LayerSchema>();
    auto root = schema->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
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
    auto roundtrip = LayerSchema::fromJsonSchema(json);
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
    REQUIRE_THROWS(LayerSchema::fromJsonSchema(invalid));
}

TEST_CASE(
    "Attribute query roots preserve payload aliases and exact runtime overlays",
    "[DataSourceInfo][schema-domain]")
{
    auto schema = std::make_shared<LayerSchema>();
    auto feature = schema->addSchema(Kind::Object, LayerSchema::featureKey("Carrier"), "Feature");
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
    REQUIRE(root != simfil::NoSchemaId);
    REQUIRE(root != attribute);
    REQUIRE(schema->childSchema(root, "$feature") == feature);
    auto transport = schema->toJsonSchema();
    schema->finalize();
    REQUIRE(schema->toJsonSchema() == transport);
    REQUIRE(schema->attributeQuerySchema("Carrier", attribute) == root);
    REQUIRE(schema->attributeQuerySchema("Missing", attribute) == simfil::NoSchemaId);
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
    auto restored = LayerSchema::fromJsonSchema(schema->toJsonSchema());
    REQUIRE(restored->attributeQuerySchema("Carrier", attribute) != simfil::NoSchemaId);
    REQUIRE(describe(restored, restored->attributeQuerySchema("Carrier", attribute)) == json);
}

TEST_CASE(
    "Recursive domains and multimap transport remain logical model views",
    "[DataSourceInfo][schema-domain]")
{
    auto schema = LayerSchema::fromJsonSchema(R"({
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
    })"_json);
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
