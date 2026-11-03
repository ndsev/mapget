#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "mapget/model/mapschema.h"
#include "mapget/model/simfilutil.h"
#include "simfil/model/json.h"
#include "simfil/model/schema-model.h"
#include "simfil/simfil.h"

#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>
#include <picosha2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <nlohmann/json-schema.hpp>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>

using namespace mapget;
using namespace std::chrono_literals;

namespace
{
FeatureTypeInfo identity(std::string name, std::string part = "id")
{
    return FeatureTypeInfo::fromJson(
        {{"name", std::move(name)},
         {"uniqueIdCompositions",
          nlohmann::json::array(
              {nlohmann::json::array({{{"partId", std::move(part)}, {"datatype", "U64"}}})})}});
}
std::vector<uint8_t> bytes(MapSchema const& schema)
{
    std::ostringstream output(std::ios::binary);
    auto result = schema.write(output);
    if (!result)
        throw std::runtime_error(result.error().message);
    auto value = output.str();
    return {value.begin(), value.end()};
}
MapSchema::Ptr simpleSchema()
{
    auto schema = std::make_shared<MapSchema>(MapFormat{"test", "1", {{"module", "2"}}});
    auto layer = schema->newLayer("Road", LayerType::Features);
    layer->addFeatureType(identity("Road"));
    schema->finalize();
    return schema;
}

TEST_CASE(
    "Layer schema search shares ranked representative discovery without transport policy",
    "[mapschema][schema-search]")
{
    auto owner = std::make_shared<MapSchema>(MapFormat{"search", "1", {}});
    auto layer = owner->newLayer("Road", LayerType::Features);
    auto root = layer->addSchema(LayerSchema::FeatureKind);
    layer->addFeatureType(identity("Road"), root);
    auto properties = layer->addSchema(simfil::Schema::Kind::Object);
    auto layers = layer->addSchema(simfil::Schema::Kind::Object);
    auto rules = layer->addSchema(simfil::Schema::Kind::Object);
    auto attribute = layer->addSchema(LayerSchema::AttributeKind);
    auto conditions = layer->addSchema(simfil::Schema::Kind::Array);
    auto direction = layer->addSchema(simfil::Schema::Kind::String);
    auto text = layer->addSchema(simfil::Schema::Kind::String);
    auto speed = layer->addSchema(simfil::Schema::Kind::Int);
    auto literal = layer->addSchema(simfil::Schema::Kind::Int);
    auto hidden = layer->addSchema(simfil::Schema::Kind::String);
    layer->addFieldSchema(root, "properties", properties);
    layer->addFieldSchema(properties, "layer", layers);
    layer->addFieldSchema(layers, "rules", rules);
    layer->addFieldSchema(rules, "SPEED_LIMIT", attribute);
    layer->setAttributeMetadata(
        attribute,
        {"Road", "rules", "SPEED_LIMIT", attribute},
        "SPEED_LIMIT");
    layer->addFieldSchema(attribute, "conditions", conditions);
    layer->addElementSchema(conditions, direction);
    layer->addFieldSchema(attribute, "speed", speed);
    layer->addNamedValue(
        speed,
        {"UNLIMITED",
         int64_t{255},
         simfil::Schema::NamedValue::Role::Special,
         simfil::Schema::NamedValue::Meaning::Unbounded});
    layer->addEnumValue(literal, INT64_MAX);
    layer->addFieldSchema(properties, "largeCode", literal);
    layer->addFieldSchema(properties, "cycle", properties);
    layer->addFieldSchema(properties, "otherDirection", direction);
    layer->addFieldSchema(properties, "thirdDirection", direction);
    layer->setTypeName(direction, "vehicle.Direction");
    for (int i = 0; i < 12; ++i) {
        layer->addFieldSchema(properties, "forwardField" + std::to_string(i), text);
        layer->addEnumSymbol(direction, "A_FORWARD_" + std::to_string(i));
    }
    layer->addEnumSymbol(direction, "FORWARD");
    layer->addFieldSchema(root, "$feature", hidden);
    layer->setFieldRole(root, "$feature", simfil::Schema::FieldRole::Context);
    layer->addEnumSymbol(hidden, "CONTEXT_SECRET");
    layer->addFieldSchema(root, "$ordinary", text);
    auto ramp = layer->addSchema(LayerSchema::FeatureKind);
    layer->addFeatureType(identity("Ramp"), ramp);
    layer->addFieldSchema(ramp, "direction", direction);
    owner->finalize();
    std::array roots{root};
    std::array types{std::string("Road")};
    simfil::EvaluationSummary summary;
    auto result = layer->find("FoR-wa_rd", roots, types, {.maxResults = 1}, 1024 * 1024, &summary);
    REQUIRE(result["items"].size() == 1);
    auto const& item = result["items"][0];
    CHECK(item["schemaId"] == direction);
    CHECK(item["enum"]["matchingSymbols"][0] == "FORWARD");
    CHECK(item["enum"]["matchingSymbolCount"] == 13);
    CHECK(item["enum"]["matchingSymbols"].size() == 8);
    CHECK(result["complete"] == false);
    CHECK(result["reason"] == "matches_omitted");
    CHECK(result["discovery"]["scanComplete"] == true);
    CHECK(result["discovery"]["matchedDomains"] == 2);
    CHECK(summary.reason == simfil::EvaluationSummary::Reason::ResultLimit);
    CHECK(summary.work > 0);
    REQUIRE(item["contexts"].size() == 2);
    CHECK(item["contextsOmitted"] == true);
    CHECK_FALSE(item.contains("expandArguments"));
    CHECK_FALSE(item["enum"].contains("valuesArguments"));
    auto contexts = item["contexts"];
    CHECK(std::ranges::any_of(
        contexts,
        [&](auto const& context)
        {
            return context.contains("attributeContext") &&
                context["attributeContext"]["name"] == "SPEED_LIMIT" &&
                context["path"].back() == "*" &&
                context["featureQuery"].template get<std::string>().ends_with("[\"conditions\"].*");
        }));
    auto named = layer->find("unlimited", roots, types, {}, 1024 * 1024);
    REQUIRE(named["items"].size() == 1);
    CHECK(named["items"][0]["namedValues"]["matchingNames"][0] == "UNLIMITED");
    auto exact = layer->find("large code", roots, {}, {}, 1024 * 1024);
    REQUIRE(exact["items"].size() == 1);
    CHECK(
        exact["items"][0]["enum"]["literalPreview"][0] ==
        nlohmann::json({{"$mapget", "int64"}, {"value", "9223372036854775807"}}));
    CHECK(exact["items"][0]["contexts"][0]["relativeToSchemaId"] == root);
    CHECK_FALSE(exact["items"][0]["contexts"][0].contains("featureQuery"));
    auto absent = layer->find("context secret", roots, {}, {}, 1024 * 1024);
    CHECK(absent["complete"] == true);
    CHECK(absent["items"].empty());
    CHECK_FALSE(layer->find("$ordinary", roots, {}, {}, 1024 * 1024)["items"].empty());
    std::array hiddenRoot{hidden};
    CHECK_FALSE(layer->find("context secret", hiddenRoot, {}, {}, 1024 * 1024)["items"].empty());
    CHECK_THROWS_AS(layer->find(" _- ", roots, {}, {}, 1024), std::invalid_argument);
    std::array allRoots{root, ramp};
    std::array allTypes{std::string("Road"), std::string("Ramp")};
    auto grouped = layer->find("forward", allRoots, allTypes, {}, 1024 * 1024);
    REQUIRE(grouped["rootMatches"].size() == 2);
    CHECK(grouped["rootMatches"][0]["featureType"] == "Road");
    CHECK(grouped["rootMatches"][0]["matchedDomains"] == 2);
    CHECK(grouped["rootMatches"][1]["featureType"] == "Ramp");
    CHECK(grouped["rootMatches"][1]["matchedDomains"] == 1);
}

TEST_CASE(
    "Layer schema search bounds candidate copies and reports interrupted discovery",
    "[mapschema][schema-search]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Raw", LayerType::SourceData);
    auto root = layer->addSchema(simfil::Schema::Kind::Object);
    auto text = layer->addSchema(simfil::Schema::Kind::String);
    layer->setRootSchema(root);
    layer->setTypeName(text, std::string(200000, 'x') + "needle");
    layer->addFieldSchema(root, "oversized", text);
    owner->finalize();
    std::array roots{root};
    simfil::EvaluationSummary summary;
    auto bytes = layer->find("needle", roots, {}, {}, 4096, &summary);
    CHECK(bytes["complete"] == false);
    CHECK(bytes["reason"] == "byte_limit");
    CHECK(bytes["discovery"]["scanComplete"] == false);
    CHECK(bytes["items"].empty());
    CHECK(bytes.dump().size() <= 4096);
    auto work = layer->find("absent", roots, {}, {.maxWork = 1}, 4096, &summary);
    CHECK(work["reason"] == "work_limit");
    CHECK(work["discovery"]["scanComplete"] == false);
    CHECK(summary.work <= 1);
    std::atomic_bool cancelled{true};
    auto cancelledResult = layer->find("needle", roots, {}, {.cancel = &cancelled}, 4096, &summary);
    CHECK(cancelledResult["reason"] == "cancelled");
    CHECK(summary.reason == simfil::EvaluationSummary::Reason::Canceled);
    auto timed = layer->find(
        "needle",
        roots,
        {},
        {.deadline = std::chrono::steady_clock::now()},
        4096,
        &summary);
    CHECK(timed["reason"] == "timeout");
    CHECK(timed["complete"] == false);
    auto depth = layer->find("needle", roots, {}, {.maxDepth = 0}, 4096);
    CHECK(depth["reason"] == "serialization_limit");
}

TEST_CASE(
    "Layer schema search bounds alternative recursion independently of data paths",
    "[mapschema][schema-search]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Raw", LayerType::SourceData);
    auto root = layer->addSchema(simfil::Schema::Kind::Union);
    layer->setRootSchema(root);
    auto tail = root;
    for (size_t i = 0; i < 32; ++i) {
        auto next = layer->addSchema(simfil::Schema::Kind::Union);
        layer->addAlternative(tail, next);
        tail = next;
    }
    auto value = layer->addSchema(simfil::Schema::Kind::String);
    layer->addEnumSymbol(value, "TARGET");
    layer->addAlternative(tail, root);
    layer->addAlternative(tail, value);
    std::array roots{root};
    CHECK_THROWS_AS(layer->find("target", roots, {}, {}, 65536), std::logic_error);
    owner->finalize();
    auto bounded = layer->find("target", roots, {}, {.maxDepth = 8}, 65536);
    CHECK(bounded["complete"] == false);
    CHECK(bounded["reason"] == "serialization_limit");
    CHECK(bounded["discovery"]["scanComplete"] == false);
    auto complete = layer->find("target", roots, {}, {.maxDepth = 64}, 65536);
    REQUIRE(complete["items"].size() == 1);
    CHECK(complete["complete"] == true);
    CHECK(complete["items"][0]["schemaId"] == value);
    CHECK(complete["items"][0]["contexts"][0]["path"].empty());
    CHECK_THROWS_AS(
        layer->find(std::string(129, 'x'), roots, {}, {}, 65536),
        std::invalid_argument);
}

/** Mutate the map metadata column in a checksum-valid codec-v2 payload, so
 * malformed-graph tests reach the structural reader rather than the digest check. */
std::vector<uint8_t>
mutateDomains(MapSchema const& schema, std::function<void(std::string&, size_t)> const& change)
{
    auto encoded = bytes(schema);
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> input(Adapter(encoded.begin(), encoded.end()));
    uint32_t magic = 0, version = 0;
    std::string identity, payload;
    input.value4b(magic);
    input.value4b(version);
    input.text1b(identity, 128);
    input.text1b(payload, 64 * 1024 * 1024);
    REQUIRE(version == 2);
    REQUIRE(input.adapter().isCompletedSuccessfully());
    std::vector<uint8_t> body(payload.begin(), payload.end());
    bitsery::Deserializer<Adapter> content(Adapter(body.begin(), body.end()));
    std::string name, release;
    content.text1b(name, body.size());
    content.text1b(release, body.size());
    std::vector<std::pair<std::string, std::string>> modules;
    content.container(
        modules,
        UINT16_MAX,
        [&](auto& reader, auto& pair)
        {
            reader.text1b(pair.first, body.size());
            reader.text1b(pair.second, body.size());
        });
    uint64_t length = 0;
    content.value8b(length);
    REQUIRE(length == (schema.domainCount() + 1) * 40);
    change(payload, content.adapter().currentReadPos());
    identity = "mapget-schema:sha256:" + picosha2::hash256_hex_string(payload);
    std::ostringstream stream(std::ios::binary);
    bitsery::Serializer<bitsery::OutputStreamAdapter> output(stream);
    output.value4b(magic);
    output.value4b(version);
    output.text1b(identity, 128);
    output.text1b(payload, 64 * 1024 * 1024);
    output.adapter().flush();
    auto result = stream.str();
    return {result.begin(), result.end()};
}

/** Mutate the embedded generic graph, retaining valid enclosing lengths/checksum. */
std::vector<uint8_t> mutateGenericDomains(
    MapSchema const& schema,
    std::function<void(std::string&, size_t)> const& change)
{
    std::ostringstream genericStream(std::ios::binary);
    REQUIRE(schema.simfil::SchemaPool::write(genericStream));
    auto generic = genericStream.str();
    std::ostringstream tailStream(std::ios::binary);
    bitsery::Serializer<bitsery::OutputStreamAdapter> tailWriter(tailStream);
    tailWriter.text1b(generic, UINT32_MAX);
    tailWriter.adapter().flush();
    auto tail = tailStream.str();

    using Adapter = bitsery::InputBufferAdapter<std::string>;
    bitsery::Deserializer<Adapter> input(Adapter(generic.begin(), generic.end()));
    uint32_t version = 0;
    uint64_t length = 0;
    input.value4b(version);
    input.value8b(length);
    REQUIRE(version == 1);
    REQUIRE(length == (schema.domainCount() + 1) * 76);
    change(generic, input.adapter().currentReadPos());

    return mutateDomains(
        schema,
        [&](std::string& payload, size_t)
        {
            REQUIRE(payload.ends_with(tail));
            payload.resize(payload.size() - tail.size());
            std::ostringstream replacement(std::ios::binary);
            bitsery::Serializer<bitsery::OutputStreamAdapter> writer(replacement);
            writer.text1b(generic, UINT32_MAX);
            writer.adapter().flush();
            payload += replacement.str();
        });
}
}  // namespace

TEST_CASE("MapSchema owns domains shared by layer views, not per-layer graphs", "[mapschema]")
{
    auto schema = std::make_shared<MapSchema>();
    auto left = schema->newLayer("A", LayerType::Features);
    auto right = schema->newLayer("B", LayerType::Features);
    auto shared = schema->addSchema(simfil::Schema::Kind::Int, "shared.speed");
    auto a = left->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey("Road"), "Feature");
    auto b = right->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey("Road"), "Feature");
    left->addFieldSchema(a, "speed", shared);
    right->addFieldSchema(b, "speed", shared);
    left->addFeatureType(identity("Road", "a"), a);
    right->addFeatureType(identity("Road", "b"), b);
    right->addFeatureType(identity("Reference"), 0, false);
    schema->finalize();
    CHECK(left->mapSchema() == right->mapSchema());
    CHECK(left->featureSchema("Road") != right->featureSchema("Road"));
    CHECK(left->childSchema(a, "speed") == right->childSchema(b, "speed"));
    CHECK(left->getTypeInfo("Road")->uniqueIdCompositions_[0][0].idPartLabel_ == "a");
    CHECK(right->getTypeInfo("Road")->uniqueIdCompositions_[0][0].idPartLabel_ == "b");
    CHECK_FALSE(right->producesFeatureType("Reference"));
    CHECK(right->featureTypes() == std::vector<std::string>{"Road"});
    CHECK_NOTHROW(left->finalize());
    CHECK_THROWS(left->setOpen(a, true));
    CHECK_THROWS(schema->addSchema(simfil::Schema::Kind::Bool));
    CHECK_THROWS(static_cast<simfil::ModelPool&>(*schema).newValue(int64_t{4}));
    CHECK_THROWS(static_cast<simfil::ModelPool&>(*schema).addRoot(left));
    CHECK_FALSE(schema->strings()->emplace("not-in-the-frozen-dictionary"));
    auto binding = schema->binding();
    std::weak_ptr<MapSchema> weak = schema;
    schema.reset();
    left = {};
    right = {};
    CHECK_FALSE(weak.expired());
    CHECK(binding->lookup(shared)->kind() == simfil::Schema::Kind::Int);
    binding.reset();
    CHECK(weak.expired());
}

TEST_CASE(
    "Layer JSON roundtrip retains local aliases and no projection-only domains",
    "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto a = graph->newLayer("A", LayerType::Features);
    auto b = graph->newLayer("B", LayerType::Features);
    a->addFeatureType(identity("Road"));
    b->addFeatureType(identity("Road"));
    auto boolean = b->addJsonSchema({{"const", true}});
    auto never = b->addSchema(simfil::Schema::Kind::Never);
    b->addFieldSchema(b->featureSchema("Road"), "enabled", boolean);
    b->addFieldSchema(b->featureSchema("Road"), "impossible", never);
    graph->finalize();
    auto exported = b->toJsonSchema();
    auto restored = std::make_shared<MapSchema>();
    auto copy = restored->importLayer("B", LayerType::Features, exported, b->featureTypeInfos());
    restored->finalize();
    CHECK(copy->featureSchema("Road") == b->featureSchema("Road"));
    CHECK(copy->featureSchema("Road") != a->featureSchema("Road"));
    CHECK(restored->domainCount() == graph->domainCount());
    CHECK(copy->toJsonSchema() == exported);
    CHECK(copy->enumValues(boolean).size() == 1);
    CHECK(copy->kind(never) == simfil::Schema::Kind::Never);
}

TEST_CASE("Const and enum restrictions intersect without duplicating literals", "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->importLayer(
        "Constants",
        LayerType::SourceData,
        {{"type", "object"},
         {"properties",
          {{"enabled", {{"enum", {true, false}}, {"const", true}}},
           {"impossible", {{"enum", {true}}, {"const", false}}}}}});
    graph->finalize();
    auto enabled = layer->childSchema(layer->rootSchema(), "enabled");
    auto impossible = layer->childSchema(layer->rootSchema(), "impossible");
    REQUIRE(layer->enumValues(enabled).size() == 1);
    CHECK(std::get<bool>(layer->enumValues(enabled).front()));
    CHECK(layer->kind(impossible) == simfil::Schema::Kind::Never);
    auto exported = layer->toJsonSchema();
    auto restored = LayerSchema::fromJsonSchema(exported);
    CHECK(restored->toJsonSchema() == exported);
}

TEST_CASE(
    "Named Attributes share physical payloads without stealing the generic branch alias",
    "[mapschema][schema.attribute]")
{
    auto schema = nlohmann::json::parse(R"json({
        "type":"object", "x-mapget":{"metaType":"Feature", "featureType":"Poi"},
        "properties":{"properties":{"type":"object", "properties":{
            "layer":{"type":"object", "x-mapget":{"metaType":"AttributeLayerMap"},
                "properties":{"POI":{"type":"object", "x-mapget":{"metaType":"AttributeContainer"},
                    "properties":{
                        "ONE":{"x-mapget":{"metaType":"Attribute", "attributeTypeCode":"ONE"},
                            "anyOf":[{"$ref":"#/definitions/physical"}]},
                        "TWO":{"x-mapget":{"metaType":"Attribute", "attributeTypeCode":"TWO"},
                            "anyOf":[{"$ref":"#/definitions/physical"}]}
                    }}}
            }}}},
        "definitions":{"physical":{"type":"object", "additionalProperties":false,
            "x-mapget":{"metaType":"Attribute", "attributeType":"physical"},
            "properties":{"payload":{"type":"object", "properties":{"value":{"type":"integer"}}}}}}
    })json");
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->importLayer("Poi", LayerType::Features, schema);
    owner->finalize();
    auto aKey = LayerSchema::attributeKey("Poi", "POI", "ONE", "/definitions/physical");
    auto bKey = LayerSchema::attributeKey("Poi", "POI", "TWO", "/definitions/physical");
    auto native = MapSchema::read(bytes(*owner));
    auto json = LayerSchema::fromJsonSchema(layer->toJsonSchema());
    for (auto view : {layer, native->layer("Poi"), json}) {
        auto generic = view->schemaId("/definitions/physical");
        auto a = view->schemaId(aKey), b = view->schemaId(bKey);
        REQUIRE(generic);
        REQUIRE(a);
        REQUIRE(b);
        CHECK(a != b);
        CHECK(a != generic);
        CHECK(b != generic);
        CHECK(view->childSchema(a, "payload") == view->childSchema(b, "payload"));
        CHECK(view->childSchema(a, "payload") == view->childSchema(generic, "payload"));
        CHECK(view->attributeTypeCode(a) == "ONE");
        CHECK(view->attributeTypeCode(b) == "TWO");
        CHECK(view->attributeTypeCode(generic).empty());
    }
}

TEST_CASE("Feature property aliases survive native and JSON schema transport", "[mapschema]")
{
    bool declaredAttributes = false;
    SECTION("Implicit attributes alias is not an enumerated field") {}
    SECTION("A declared attributes field retains its independent domain")
    {
        declaredAttributes = true;
    }
    auto input = nlohmann::json::parse(R"({
        "oneOf":[{"$ref":"#/$defs/RoadFeature"}],"$defs":{"RoadFeature":{
            "type":"object", "additionalProperties":false,
            "x-mapget":{"metaType":"Feature","featureType":"Road"},
            "properties":{"properties":{"type":"object","additionalProperties":false,
                "properties":{"numbers":{"type":"array","items":{"type":"integer"}}}}}
        }}
    })");
    if (declaredAttributes)
        input["$defs"]["RoadFeature"]["properties"]["attributes"] = {{"type", "string"}};
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->importLayer("Road", LayerType::Features, input);
    owner->finalize();
    auto native = MapSchema::read(bytes(*owner));
    auto json = LayerSchema::fromJsonSchema(layer->toJsonSchema());
    size_t variant = 0;
    for (auto view : {layer, native->layer("Road"), json}) {
        CAPTURE(declaredAttributes, variant);
        ++variant;
        auto graph = view->mapSchema();
        auto feature = graph->domain(view->featureSchema("Road"));
        REQUIRE(feature);
        REQUIRE(feature->canHaveField(StringPool::AttributesStr));
        CHECK(
            feature->canonicalField(StringPool::AttributesStr) ==
            (declaredAttributes ? StringPool::AttributesStr : StringPool::PropertiesStr));
        auto attributes = feature->fieldSchema(StringPool::AttributesStr);
        auto properties = feature->fieldSchema(StringPool::PropertiesStr);
        REQUIRE(attributes != simfil::NoSchemaId);
        REQUIRE(properties != simfil::NoSchemaId);
        CHECK((attributes == properties) == !declaredAttributes);
        CHECK(
            std::ranges::count(feature->directFields(), StringPool::AttributesStr) ==
            (declaredAttributes ? 1 : 0));
        CHECK(std::ranges::count(feature->directFields(), StringPool::PropertiesStr) == 1);
        auto root = graph->domain(view->rootSchema());
        REQUIRE(root);
        CHECK(root->canHaveField(StringPool::AttributesStr));
    }
}

TEST_CASE(
    "Context edge roles survive native and JSON schema transport",
    "[mapschema][schema.context]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Context", LayerType::Features);
    auto root = layer->addSchema(simfil::Schema::Kind::Object);
    auto hidden = layer->addSchema(simfil::Schema::Kind::Object);
    auto scalar = layer->addSchema(simfil::Schema::Kind::Int);
    layer->addFieldSchema(root, "$data", scalar);
    layer->addFieldSchema(root, "context", hidden);
    layer->setFieldRole(root, "context", simfil::Schema::FieldRole::Context);
    layer->addFieldSchema(hidden, "secret", scalar);
    layer->setOpen(hidden, true);
    layer->setRootSchema(root);
    owner->finalize();
    auto restored = MapSchema::read(bytes(*owner));
    auto json = LayerSchema::fromJsonSchema(layer->toJsonSchema());
    for (auto view : {layer, restored->layer("Context"), json}) {
        CHECK(view->fieldRole(root, "context") == simfil::Schema::FieldRole::Context);
        CHECK(view->fieldRole(root, "$data") == simfil::Schema::FieldRole::Data);
        CHECK(view->reachabilityComplete(root));
        CHECK_FALSE(view->canHaveField(root, "secret"));
        CHECK(view->canHaveField(root, "context"));
        CHECK(view->childSchema(root, "context") == hidden);
        CHECK(view->childSchema(hidden, "secret") == scalar);
        auto node = view->mapSchema()->domainNode(root)->toJson();
        CHECK(node["fields"]["context"]["role"] == "context");
        CHECK(node["fields"]["context"]["$ref"] == hidden);
        CHECK_FALSE(node["fields"]["$data"].contains("role"));
    }
}

TEST_CASE("Historical packed JSON kinds migrate by the legacy-v1 layout", "[mapschema]")
{
    // Literal pre-v6 export values, deliberately not computed from current enums.
    // In particular, old array=983552 now collides with the native object kind.
    auto legacy = nlohmann::json::parse(R"json({
        "x-mapget":{"domainDefinitions":true},
        "$ref":"#/definitions/1",
        "definitions":{
            "1":{"type":"object","additionalProperties":false,"properties":{
                "object":{"$ref":"#/definitions/2"},
                "items":{"$ref":"#/definitions/3"},
                "bytes":{"$ref":"#/definitions/4"},
                "flags":{"$ref":"#/definitions/5"},
                "attribute":{"$ref":"#/definitions/6"},
                "unknown":{"$ref":"#/definitions/7"}
            },"x-mapget":{"schemaId":1,"kind":8126720,"schemaKey":"Feature:Road",
                          "metaType":"Feature","typename":"Road"}},
            "2":{"type":"object","additionalProperties":false,
                 "x-mapget":{"schemaId":2,"kind":917760}},
            "3":{"type":"array","items":{"$ref":"#/definitions/2"},
                 "x-mapget":{"schemaId":3,"kind":983552}},
            "4":{"type":"object","required":["_bytes","hex","number"],"properties":{
                    "_bytes":{"const":true},"hex":{"type":"string"},
                    "number":{"type":["integer","null"]}},
                 "x-mapget":{"schemaId":4,"kind":852032,"enumBytes":["abcd"]}},
            "5":{"type":"string","x-mapget":{
                    "schemaId":5,"kind":8585248,"bitmaskValues":["LEFT","RIGHT"],
                    "sourceType":"legacy.Flags"}},
            "6":{"type":"object","properties":{"flags":{"$ref":"#/definitions/5"}},
                 "x-mapget":{"schemaId":6,"kind":8192256,"metaType":"Attribute"}},
            "7":{"x-mapget":{"schemaId":7,"kind":263166}}
        }
    })json");
    auto layer = LayerSchema::fromJsonSchema(legacy);
    using Kind = simfil::Schema::Kind;
    REQUIRE(layer->rootSchema() == 1);
    CHECK(layer->featureSchema("Road") == 1);
    CHECK(layer->kind(1) == LayerSchema::FeatureKind);
    CHECK(layer->kind(2) == Kind::Object);
    CHECK(layer->kind(3) == Kind::Array);
    std::vector<simfil::SchemaId> elements;
    layer->mapSchema()->domain(3)->forEachElementSchema([&](auto id) { elements.push_back(id); });
    CHECK(elements == std::vector<simfil::SchemaId>{2});
    CHECK(layer->kind(4) == Kind::Bytes);
    CHECK(layer->directFields(4).empty());
    REQUIRE(layer->enumValues(4).size() == 1);
    CHECK(std::get<simfil::ByteArray>(layer->enumValues(4).front()).toHex(false) == "abcd");
    CHECK(layer->kind(6) == LayerSchema::AttributeKind);
    auto flags = layer->mapSchema()->domain(5);
    CHECK(
        simfil::Schema::kindNameId(flags->kind()) ==
        simfil::Schema::kindNameId(LayerSchema::BitmaskKind));
    CHECK(flags->hasAffinity(simfil::ValueType::String));
    CHECK_FALSE(flags->hasAffinity(simfil::ValueType::Bitmask));
    CHECK_FALSE(flags->isBitmask());
    CHECK_FALSE(flags->namedValue("LEFT"));
    CHECK(flags->bitWidth() == 0);
    CHECK(
        std::ranges::equal(layer->directEnumSymbols(5), std::vector<std::string>{"LEFT", "RIGHT"}));
    CHECK_FALSE(layer->reachabilityComplete(7));

    auto canonical = layer->toJsonSchema();
    auto const& definitions = canonical.at("definitions");
    CHECK(
        definitions["1"]["x-mapget"]["kind"] ==
        nlohmann::json{{"name", "Feature"}, {"affinities", {"object"}}});
    CHECK(
        definitions["3"]["x-mapget"]["kind"] ==
        nlohmann::json{{"name", "array"}, {"affinities", {"array"}}});
    CHECK(
        definitions["5"]["x-mapget"]["kind"] ==
        nlohmann::json{{"name", "bitmask"}, {"affinities", {"string"}}});
    CHECK(
        definitions["5"]["x-mapget"]["bitmaskValues"] == nlohmann::json::array({"LEFT", "RIGHT"}));
    CHECK_FALSE(definitions["5"]["x-mapget"].contains("namedValues"));
    CHECK_FALSE(definitions["5"]["x-mapget"].contains("bitWidth"));
    CHECK_FALSE(definitions["5"].contains("enum"));
    CHECK_FALSE(definitions["7"].contains("type"));
    for (auto const& definition : definitions)
        CHECK(definition.at("x-mapget").at("kind").is_object());
    auto restored = LayerSchema::fromJsonSchema(canonical);
    CHECK(restored->toJsonSchema() == canonical);
    auto binary = MapSchema::read(bytes(*layer->mapSchema()));
    CHECK(binary->layer("legacy")->toJsonSchema() == canonical);
}

TEST_CASE(
    "Every legacy kind name and affinity maps independently of current ordinals",
    "[mapschema]")
{
    using Kind = simfil::Schema::Kind;
    // Legacy scalar/all sets deliberately exclude the newly introduced raw-mask value type.
    auto all = simfil::Schema::AllAffinities &
        ~simfil::valueTypeAffinity(simfil::ValueType::Bitmask);
    auto scalar = simfil::Schema::ScalarAffinities &
        ~simfil::valueTypeAffinity(simfil::ValueType::Bitmask);
    auto withBits = [](Kind kind, uint16_t bits)
    {
        return simfil::Schema::makeKind(simfil::Schema::kindNameId(kind), bits);
    };
    for (
        auto [packed, expected] : std::vector<std::pair<uint32_t, Kind>>{
            {263166, withBits(Kind::Unknown, all)},
            {328702, withBits(Kind::Any, all)},
            {393216, Kind::Never},
            {458878, withBits(Kind::Value, scalar)},
            {524290, Kind::Null},
            {589828, Kind::Bool},
            {655368, Kind::Int},
            {720912, Kind::Float},
            {786464, Kind::String},
            {852032, Kind::Bytes},
            {917760, Kind::Object},
            {983552, Kind::Array},
            {1049598, withBits(Kind::Union, all)},
            {1115134, withBits(Kind::OneOf, all)},
            {1180670, withBits(Kind::Intersection, all)},
            {8126720, LayerSchema::FeatureKind},
            {8192256, LayerSchema::AttributeKind},
            {8585248,
             withBits(
                 LayerSchema::BitmaskKind,
                 simfil::valueTypeAffinity(simfil::ValueType::String))},
            {655370,
             withBits(
                 Kind::Int,
                 simfil::valueTypeAffinity(simfil::ValueType::Int) |
                     simfil::valueTypeAffinity(simfil::ValueType::Null))},
            {720920,
             withBits(
                 Kind::Float,
                 simfil::valueTypeAffinity(simfil::ValueType::Float) |
                     simfil::valueTypeAffinity(simfil::ValueType::Int))}})
    {
        CAPTURE(packed);
        auto layer = LayerSchema::fromJsonSchema({{"x-mapget", {{"kind", packed}}}});
        CHECK(layer->kind(layer->rootSchema()) == expected);
        auto roundtrip = LayerSchema::fromJsonSchema(layer->toJsonSchema());
        CHECK(roundtrip->kind(roundtrip->rootSchema()) == expected);
    }
}

TEST_CASE(
    "JSON kind projections reject unknown encodings but retain semantic annotations",
    "[mapschema]")
{
    for (auto const& invalid : nlohmann::json::array(
             {-1,
              uint64_t(UINT32_MAX) + 1,
              8126721,
              983040 + 1024,
              200u << 16,
              1.5,
              nullptr,
              true,
              nlohmann::json{{"name", "object"}},
              nlohmann::json{{"name", "unregistered"}, {"affinities", {"object"}}},
              nlohmann::json{{"name", "object"}, {"affinities", {"futureType"}}},
              nlohmann::json{{"name", "object"}, {"affinities", {"object", "object"}}},
              nlohmann::json{{"name", "object"}, {"affinities", {9}}},
              nlohmann::json{{"name", "object"}, {"affinities", {"object"}}, {"version", 99}}}))
    {
        CAPTURE(invalid);
        CHECK_THROWS(LayerSchema::fromJsonSchema({{"x-mapget", {{"kind", invalid}}}}));
    }
    for (auto annotation : {"attribute", "region", "relation"}) {
        auto layer = LayerSchema::fromJsonSchema(
            {{"type", {"object", "null"}},
             {"x-mapget", {{"kind", annotation}}},
             {"properties", {{"label", {{"type", "string"}}}}}});
        auto graph = layer->mapSchema();
        auto root = graph->domain(layer->rootSchema());
        REQUIRE(root);
        CHECK(root->hasAffinity(simfil::ValueType::Object));
        CHECK(root->hasAffinity(simfil::ValueType::Null));
        CHECK(root->composition() == simfil::Schema::Composition::AnyOf);
        auto canonical = layer->toJsonSchema();
        CHECK(LayerSchema::fromJsonSchema(canonical)->toJsonSchema() == canonical);
    }
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Masks", LayerType::SourceData);
    auto mask = layer->addSchema(LayerSchema::BitmaskKind);
    layer->setRootSchema(mask);
    layer->setBitWidth(mask, 8);
    layer->addNamedValue(
        mask,
        {.name = "LEFT", .value = uint64_t{1}, .role = simfil::Schema::NamedValue::Role::Flag});
    graph->finalize();
    auto canonical = layer->toJsonSchema();
    CHECK(
        canonical["definitions"][std::to_string(mask)]["x-mapget"]["kind"] ==
        nlohmann::json{{"name", "bitmask"}, {"affinities", {"bitmask"}}});
    auto restored = LayerSchema::fromJsonSchema(canonical);
    auto domain = restored->mapSchema()->domain(restored->rootSchema());
    CHECK(domain->isBitmask());
    CHECK(domain->bitWidth() == 8);
    REQUIRE(domain->namedValue("LEFT"));
    CHECK(std::get<uint64_t>(domain->namedValue("LEFT")->value) == 1);
}

TEST_CASE("MapSchema binary roundtrip validates identity and pins exact definitions", "[mapschema]")
{
    auto source = simpleSchema();
    auto id = std::string(source->schemaId());
    auto encoded = bytes(*source);
    // Independently allocated graphs must never encode addresses or uninitialized padding.
    CHECK(bytes(*simpleSchema()) == encoded);
    CHECK(simpleSchema()->schemaId() == id);
    auto restored = MapSchema::read(encoded, id);
    CHECK(MapSchema::read(encoded, id) == restored);
    CHECK(restored->format() == source->format());
    CHECK(restored->schemaId() == id);
    CHECK(bytes(*restored) == encoded);
    CHECK(restored->layer("Road")->getTypeInfo("Road")->name_ == "Road");
    CHECK_THROWS(MapSchema::read(encoded, "wrong-schema"));
    CHECK_THROWS(MapSchema::read(encoded, id, 8));
    auto truncated = encoded;
    truncated.pop_back();
    CHECK_THROWS(MapSchema::read(truncated));
    auto corrupt = encoded;
    corrupt[corrupt.size() / 2] ^= 1;
    CHECK_THROWS(MapSchema::read(corrupt));
    auto trailing = encoded;
    trailing.push_back(0);
    CHECK_THROWS(MapSchema::read(trailing));
    source.reset();
    CHECK(restored->layer("Road")->mapSchema() == restored);
}

TEST_CASE(
    "Union members retain direct runtime domains across schema transport",
    "[mapschema][schema-lookup]")
{
    using Kind = simfil::Schema::Kind;
    using Composition = simfil::Schema::Composition;
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Members", LayerType::SourceData);
    auto choice = layer->addSchema(Kind::OneOf);
    layer->setComposition(choice, Composition::OneOf);
    layer->setRootSchema(choice);
    auto speed = layer->addSchema(Kind::Int);
    auto count = layer->addSchema(Kind::Int);
    layer->addNamedValue(speed, {.name = "UNLIMITED", .value = int64_t{255}});
    for (auto [name, domain] : {std::pair{"speed", speed}, {"count", count}}) {
        auto branch = layer->addSchema(Kind::Object);
        layer->addFieldSchema(branch, name, domain);
        layer->addAlternative(choice, branch);
    }
    auto array = layer->addSchema(Kind::Array);
    layer->addElementSchema(array, speed);
    auto emptyMember = layer->addSchema(Kind::Object);
    layer->addFieldSchema(emptyMember, "", count);
    graph->finalize();
    auto encoded = bytes(*graph);
    auto restored = MapSchema::read(encoded);
    for (auto const& owner : {std::shared_ptr<MapSchema const>(graph), restored}) {
        auto definition = owner->domain(choice);
        REQUIRE(definition);
        CHECK(definition->composition() == Composition::OneOf);
        CHECK(definition->alternatives().size() == 2);
        CHECK(definition->directFields().empty());
        CHECK(definition->fieldSchema(owner->strings()->get("speed")) == speed);
        CHECK(definition->fieldSchema(owner->strings()->get("count")) == count);
        CHECK(owner->domain(array)->itemSchema() == speed);

        // Runtime and schema dictionaries deliberately assign different IDs.
        auto strings = std::make_shared<simfil::StringPool>();
        REQUIRE(strings->emplace("runtime-padding"));
        auto model = std::make_shared<simfil::ModelPool>(strings);
        model->setSchemaBinding(owner->binding());
        auto object = model->newObject();
        REQUIRE(object->setSchema(choice));
        REQUIRE(object->addField("speed", int64_t{255}));
        auto field = strings->get("speed");
        auto child = object->get(field);
        REQUIRE(child);
        auto value = simfil::Value::field(*object, *child, field);
        CHECK(value.schemaReference().id == speed);
        REQUIRE(value.namedValue("UNLIMITED"));
        CHECK(value.namedValue("UNLIMITED")->as<simfil::ValueType::Int>() == 255);
        auto items = model->newArray();
        REQUIRE(items->setSchema(array));
        items->append(int64_t{255});
        CHECK(simfil::Value::at(*items, *items->at(0), 0).schemaReference().id == speed);

        auto completionStrings = std::make_shared<StringPool>("MemberLookup");
        REQUIRE(completionStrings->emplace("different-padding"));
        auto env = makeEnvironment(completionStrings);
        installCompletionLayerSchema(*env, owner->layer("Members"), completionStrings);
        CHECK(env->querySchema(choice)->fieldSchema(completionStrings->get("speed")) == speed);
        CHECK(env->querySchema(array)->itemSchema() == speed);
        REQUIRE(completionStrings->emplace("not-in-graph"));
        CHECK(env->querySchema(emptyMember)->fieldSchema(completionStrings->get("")) == count);
        CHECK(
            env->querySchema(emptyMember)->fieldSchema(completionStrings->get("not-in-graph")) ==
            simfil::NoSchemaId);
    }
    CHECK(bytes(*restored) == encoded);
    CHECK(restored->layer("Members")->toJsonSchema() == layer->toJsonSchema());
}

TEST_CASE("MapSchema preflights inner dictionary and model lengths before allocation", "[mapschema]")
{
    auto source = simpleSchema();
    std::ostringstream dictionaryStream(std::ios::binary), modelStream(std::ios::binary);
    REQUIRE(source->strings()->simfil::StringPool::write(dictionaryStream));
    REQUIRE(source->ModelPool::write(modelStream));
    auto dictionary = dictionaryStream.str(), model = modelStream.str();
    std::ostringstream tailStream(std::ios::binary);
    bitsery::Serializer<bitsery::OutputStreamAdapter> tailWriter(tailStream);
    tailWriter.text1b(dictionary, UINT32_MAX);
    tailWriter.text1b(model, UINT32_MAX);
    tailWriter.adapter().flush();
    auto tail = tailStream.str();

    bool corruptModel = false;
    SECTION("Dictionary length") {}
    SECTION("Model length") { corruptModel = true; }

    // The one-byte case exercises actual-input preflight, not just the memory
    // budget. The large length must likewise fail without allocating its payload.
    for (size_t length : {size_t{1}, size_t{64 * 1024 * 1024}}) {
        auto malformed = mutateGenericDomains(
            *source,
            [&](std::string& payload, size_t)
            {
                REQUIRE(payload.ends_with(tail));
                payload.resize(payload.size() - tail.size());
                std::ostringstream replacement(std::ios::binary);
                bitsery::Serializer<bitsery::OutputStreamAdapter> writer(replacement);
                if (corruptModel)
                    writer.text1b(dictionary, UINT32_MAX);
                bitsery::details::writeSize(writer.adapter(), length);
                writer.adapter().flush();
                payload += replacement.str();
            });
        REQUIRE(malformed.size() < 64 * 1024);
        CHECK_THROWS_AS(MapSchema::read(malformed), std::length_error);
    }
}

TEST_CASE("MapSchema codec v2 deliberately rejects the committed v1 artifact", "[mapschema]")
{
    // V2 replaces monolithic domain records with map metadata and a generic
    // SchemaPool payload. The incompatible V1 artifact must never be reinterpreted.
    std::ifstream input(MAPGET_TEST_DATA_DIR "/mapschema-v1.bin", std::ios::binary);
    REQUIRE(input.is_open());
    std::vector<uint8_t>
        legacy(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    REQUIRE(legacy.size() == 3048);
    REQUIRE(
        picosha2::hash256_hex_string(legacy) ==
        "707951a44c9504e2c6a2bc60e8966d0a0d077ea7847cb1020af885880bf08daa");
    CHECK_THROWS_WITH(MapSchema::read(legacy), "Unsupported MapSchema binary format.");
}

TEST_CASE("MapSchema codec v2 matches the committed binary golden fixture", "[mapschema]")
{
    // This fixes codec V2, including generic SchemaPool V1 and the empty provenance
    // list. Replacing these bytes requires an explicit codec/cache-identity decision,
    // not routine snapshot regeneration. The V1 fixture above guards rejection.
    constexpr auto expectedId =
        "mapget-schema:sha256:1c54b7cff7e4731babb03de06bef2980cddfe77eb4e7c026a585bc972552a292";
    constexpr auto expectedSha = "95f42f060e6d5c856fba7142838a5c53d0e3b54496fdf5f2420b4a846bca3d5c";
    std::ifstream input(MAPGET_TEST_DATA_DIR "/mapschema-v2.bin", std::ios::binary);
    REQUIRE(input.is_open());
    std::vector<uint8_t>
        golden(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    REQUIRE(golden.size() == 3124);
    REQUIRE(picosha2::hash256_hex_string(golden) == expectedSha);

    // Decode the fixed artifact before constructing any graph with today's writer.
    auto decoded = MapSchema::read(golden, expectedId);
    CHECK(decoded->schemaId() == expectedId);
    CHECK(decoded->domainCount() == 4);
    MapFormat format{"mapget-codec-fixture", "1", {{"values", "1"}}};
    CHECK(decoded->format() == format);
    auto layer = decoded->layer("Values");
    REQUIRE(layer);
    CHECK(layer->layerType() == LayerType::SourceData);
    CHECK(layer->rootSchema() == 1);
    CHECK(layer->childSchema(1, "count") == 2);
    CHECK(layer->childSchema(1, "flags") == 3);
    CHECK(layer->childSchema(1, "speed") == 4);
    CHECK(layer->kind(2) == simfil::Schema::Kind::Int);
    CHECK(layer->kind(4) == simfil::Schema::Kind::Int);
    CHECK(layer->bitWidth(2) == 0);
    CHECK(layer->bitWidth(4) == 0);
    auto numeric = decoded->numericDomain(4);
    REQUIRE(numeric);
    CHECK(numeric->encoding == "uint8");
    CHECK(numeric->unit == "km/h");
    REQUIRE(numeric->minimum);
    REQUIRE(numeric->maximum);
    CHECK(std::get<uint64_t>(*numeric->minimum) == 0);
    CHECK(std::get<uint64_t>(*numeric->maximum) == 255);
    REQUIRE(numeric->ordinaryRanges.size() == 1);
    CHECK(std::get<uint64_t>(numeric->ordinaryRanges[0].min) == 1);
    CHECK(std::get<uint64_t>(numeric->ordinaryRanges[0].max) == 253);
    CHECK(numeric->scale == 1);
    CHECK(numeric->offset == 0);
    CHECK(numeric->complete);
    auto special = decoded->domain(4)->namedValue("UNLIMITED");
    REQUIRE(special);
    CHECK(std::get<uint64_t>(special->value) == 255);
    CHECK(special->role == simfil::Schema::NamedValue::Role::Special);
    CHECK(special->meaning == simfil::Schema::NamedValue::Meaning::Unbounded);
    CHECK(decoded->domain(4)->enumValues().empty());
    CHECK(layer->directEnumSymbols(4).empty());
    REQUIRE(decoded->domain(3)->isBitmask());
    CHECK(layer->bitWidth(3) == 8);
    auto flag = decoded->domain(3)->namedValue("LEFT");
    REQUIRE(flag);
    CHECK(std::get<uint64_t>(flag->value) == 1);
    CHECK(flag->role == simfil::Schema::NamedValue::Role::Flag);
    CHECK(flag->meaning == simfil::Schema::NamedValue::Meaning::Ordinary);
    CHECK(bytes(*decoded) == golden);

    auto schema = std::make_shared<MapSchema>(format);
    (void)schema->importLayer("Values", LayerType::SourceData, nlohmann::json::parse(R"json({
        "type": "object",
        "properties": {
            "count": {"type": "integer"},
            "speed": {"type": "integer", "x-mapget": {
                "numericDomain": {
                    "encoding": "uint8", "unit": "km/h", "minimum": 0, "maximum": 255,
                    "ordinaryRanges": [{"minimum": 1, "maximum": 253}], "complete": true
                },
                "namedValues": [{
                    "name": "UNLIMITED", "value": 255, "role": "special", "meaning": "unbounded"
                }]
            }},
            "flags": {"x-mapget": {
                "kind": {"name": "bitmask", "affinities": ["bitmask"]}, "bitWidth": 8,
                "namedValues": [{
                    "name": "LEFT", "value": 1, "role": "flag", "meaning": "ordinary"
                }]
            }}
        }
    })json"));
    schema->finalize();
    CHECK(schema->schemaId() == expectedId);
    CHECK(bytes(*schema) == golden);
}

TEST_CASE("Attribute source aliases preserve definition and owner context", "[mapschema]")
{
    auto attribute = [](nlohmann::json value)
    {
        return nlohmann::json{
            {"type", "object"},
            {"x-mapget", {{"metaType", "Attribute"}, {"attributeTypeCode", "LIMIT"}}},
            {"properties", {{"value", std::move(value)}}}};
    };
    auto json = nlohmann::json::object();
    json["definitions"]["numeric"] =
        attribute({{"type", "integer"}, {"minimum", 0}, {"maximum", 255}});
    json["definitions"]["mask"] = attribute(nlohmann::json::parse(R"json({
        "type":"string","x-mapget":{
            "kind":{"name":"bitmask","affinities":["bitmask"]},
            "bitWidth":8,"namedValues":[{"name":"LEFT","value":1,"role":"flag"}]
        }
    })json"));
    for (auto name : {"Link", "GeoLine"}) {
        auto container = nlohmann::json{
            {"type", "object"},
            {"x-mapget", {{"metaType", "AttributeContainer"}}},
            {"properties",
             {{"LIMIT",
               {{"anyOf",
                 {{{"$ref", "#/definitions/numeric"}}, {{"$ref", "#/definitions/mask"}}}}}}}}};
        auto layers = nlohmann::json{
            {"type", "object"},
            {"x-mapget", {{"metaType", "AttributeLayerMap"}}},
            {"properties", {{"rules", std::move(container)}}}};
        json["anyOf"].push_back(
            {{"type", "object"},
             {"x-mapget", {{"metaType", "Feature"}, {"featureType", name}}},
             {"properties", {{"layer", std::move(layers)}}}});
    }
    auto layer = LayerSchema::fromJsonSchema(json);
    auto canonical = layer->toJsonSchema();
    auto restored = LayerSchema::fromJsonSchema(canonical);
    auto binary = MapSchema::read(bytes(*layer->mapSchema()))->layer("legacy");
    std::set<simfil::SchemaId> distinct;
    for (auto feature : {"Link", "GeoLine"})
        for (auto definition : {"numeric", "mask"}) {
            auto key = LayerSchema::attributeKey(
                feature,
                "rules",
                "LIMIT",
                "/definitions/" + std::string(definition));
            auto id = layer->schemaId(key);
            REQUIRE(id);
            CHECK(distinct.insert(id).second);
            for (auto current : {layer, restored, binary}) {
                CHECK(current->schemaId(key) == id);
                auto owner = current->ownerForPath(feature, id, {});
                CHECK(owner.kind_ == LayerSchema::PathOwnerKind::Attribute);
                CHECK(owner.attribute_.featureType_ == feature);
                CHECK(current->attributeQuerySchema(feature, id));
                CHECK_FALSE(current->attributeQuerySchema(
                    feature == std::string_view("Link") ? "GeoLine" : "Link",
                    id));
                auto value = current->mapSchema()->domain(current->childSchema(id, "value"));
                REQUIRE(value);
                CHECK(value->isBitmask() == (definition == std::string_view("mask")));
                CHECK(
                    value->hasAffinity(simfil::ValueType::Int) ==
                    (definition == std::string_view("numeric")));
            }
        }
    CHECK(restored->toJsonSchema() == canonical);
    CHECK(binary->toJsonSchema() == canonical);
    CHECK(
        LayerSchema::attributeKey("A:B", "C", "D", "/definitions/E") !=
        LayerSchema::attributeKey("A", "B:C", "D", "/definitions/E"));
    CHECK_THROWS(LayerSchema::attributeKey("", "rules", "LIMIT", "/definitions/numeric"));
}

TEST_CASE("Unannotated schema kinds retain valid default bit widths", "[mapschema]")
{
    using Kind = simfil::Schema::Kind;
    for (auto kind :
         {Kind::Unknown,
          Kind::Any,
          Kind::Never,
          Kind::Value,
          Kind::Null,
          Kind::Bool,
          Kind::Int,
          Kind::Float,
          Kind::String,
          Kind::Bytes,
          Kind::Object,
          Kind::Array,
          Kind::Union,
          Kind::OneOf,
          Kind::Intersection,
          LayerSchema::FeatureKind,
          LayerSchema::AttributeKind,
          LayerSchema::BitmaskKind})
    {
        CAPTURE(uint32_t(kind));
        auto graph = std::make_shared<MapSchema>();
        auto layer = graph->newLayer("Bare", LayerType::SourceData);
        auto root = layer->addSchema(kind);
        layer->setRootSchema(root);
        REQUIRE_NOTHROW(graph->finalize());
        auto expectedWidth = kind == LayerSchema::BitmaskKind ? 64 : 0;
        CHECK(graph->bitWidth(root) == expectedWidth);
        auto restored = MapSchema::read(bytes(*graph));
        CHECK(restored->bitWidth(root) == expectedWidth);
    }
}

TEST_CASE("Raw bitmask JSON schemas validate native tags without inventing fields", "[mapschema]")
{
    using Named = simfil::Schema::NamedValue;
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Masks", LayerType::SourceData);
    auto mask = layer->addSchema(LayerSchema::BitmaskKind, "Mask");
    auto nullable = layer->addSchema(LayerSchema::BitmaskKind, "NullableMask");
    layer->setBitWidth(mask, 57);
    layer->setBitWidth(nullable, 64);
    layer->setNullable(nullable, true);
    for (auto id : {mask, nullable}) {
        layer
            ->addNamedValue(id, {.name = "KNOWN", .value = uint64_t{2}, .role = Named::Role::Flag});
        layer->addNamedValue(id, {.name = "NONE", .value = uint64_t{0}, .role = Named::Role::Flag});
    }
    auto legacy = layer->addSchema(
        simfil::Schema::makeKind(
            simfil::Schema::kindNameId(LayerSchema::BitmaskKind),
            simfil::valueTypeAffinity(simfil::ValueType::String)),
        "LegacyMask");
    layer->addEnumSymbol(legacy, "LEFT");
    layer->addEnumSymbol(legacy, "RIGHT");
    layer->setRootSchema(mask);
    graph->finalize();
    auto exported = layer->toJsonSchema();
    auto imported = LayerSchema::fromJsonSchema(exported);
    auto binary = MapSchema::read(bytes(*graph))->layer("Masks");
    for (auto current : {layer, binary, imported}) {
        REQUIRE(current->mapSchema()->domainCount() == 3);
        CHECK(current->schemaId("Mask") == mask);
        CHECK(current->schemaId("NullableMask") == nullable);
        CHECK(current->schemaId("LegacyMask") == legacy);
        CHECK(current->toJsonSchema() == exported);
        for (auto id : {mask, nullable}) {
            auto owner = current->mapSchema();
            REQUIRE(owner->domain(id)->isBitmask());
            auto width = id == mask ? 57 : 64;
            CHECK(current->bitWidth(id) == width);
            CHECK(current->directFields(id).empty());
            CHECK(current->nestedFields(id).empty());
            CHECK_FALSE(current->fieldRequired(id, "_bitmask"));
            CHECK(current->directEnumSymbols(id).empty());
            auto definition = exported;
            definition["$ref"] = "#/definitions/" + std::to_string(id);
            nlohmann::json_schema::json_validator validator;
            validator.set_root_schema(definition);
            auto binding = owner->binding();
            for (uint64_t bits : {uint64_t{0}, uint64_t{2}, uint64_t{1} << (width - 1)}) {
                simfil::Bitmask value{bits, {binding, id}};
                auto known = value.hasFlag("KNOWN");
                REQUIRE(known);
                CHECK(*known == bool(bits & 2));
                auto zero = value.hasFlag("NONE");
                REQUIRE(zero);
                CHECK(*zero == (bits == 0));
                auto tagged = value.toJson();
                REQUIRE_NOTHROW(validator.validate(tagged));
                auto model = std::make_shared<simfil::ModelPool>();
                model->setSchemaBinding(binding);
                auto parsed = simfil::json::buildModelNode(tagged, *model);
                REQUIRE(parsed);
                CHECK((*parsed)->toJson() == tagged);
                model->addRoot(*parsed);
                std::ostringstream stream(std::ios::binary);
                REQUIRE(model->write(stream));
                auto payload = stream.str();
                auto restored = std::make_shared<simfil::ModelPool>();
                restored->setSchemaBinding(binding);
                REQUIRE(restored->read(std::vector<uint8_t>(payload.begin(), payload.end())));
                auto root = restored->root(0);
                REQUIRE(root);
                CHECK((*root)->toJson() == tagged);
            }
            auto tagged = simfil::Bitmask{2, {binding, id}}.toJson();
            auto invalidFields =
                nlohmann::json{{"_bitmask", false}, {"bits", 2}, {"domain", "1"}, {"schema", ""}};
            for (auto const& [field, value] : invalidFields.items()) {
                auto invalid = tagged;
                invalid[field] = value;
                CHECK_THROWS(validator.validate(invalid));
            }
            for (auto const& field : {"_bitmask", "bits", "schema", "domain"}) {
                auto invalid = tagged;
                invalid.erase(field);
                CHECK_THROWS(validator.validate(invalid));
            }
            auto extra = tagged;
            extra["other"] = true;
            CHECK_THROWS(validator.validate(extra));
            CHECK_THROWS(validator.validate("KNOWN"));
            if (id == nullable)
                CHECK_NOTHROW(validator.validate(nullptr));
            else
                CHECK_THROWS(validator.validate(nullptr));
        }
        CHECK_FALSE(current->mapSchema()->domain(legacy)->isBitmask());
        CHECK(current->bitWidth(legacy) == 0);
        auto definition = exported;
        definition["$ref"] = "#/definitions/" + std::to_string(legacy);
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(definition);
        CHECK_NOTHROW(validator.validate("LEFT | RIGHT"));
        CHECK_THROWS(validator.validate(
            simfil::Bitmask{2, {current->mapSchema()->binding(), mask}}.toJson()));
    }
}

TEST_CASE("Only concrete raw mask domains carry a bit width", "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Values", LayerType::SourceData);
    auto number = layer->addSchema(simfil::Schema::Kind::Int);
    layer->setJsonSchemaAnnotations(number, {{"x-mapget", {{"sourceType", "example.Count"}}}});
    auto mask = layer->addSchema(LayerSchema::BitmaskKind);
    auto nullable = layer->addSchema(LayerSchema::BitmaskKind);
    layer->setNullable(nullable, true);
    layer->setBitWidth(nullable, 8);
    layer->setRootSchema(number);
    graph->finalize();
    for (auto current : {MapSchema::ConstPtr(graph), MapSchema::read(bytes(*graph))}) {
        CHECK(current->bitWidth(number) == 0);
        CHECK(current->bitWidth(mask) == 64);
        CHECK(current->bitWidth(nullable) == 8);
    }
    auto roundtrip = LayerSchema::fromJsonSchema(layer->toJsonSchema());
    CHECK(roundtrip->bitWidth(number) == 0);
    CHECK(roundtrip->bitWidth(mask) == 64);
    CHECK(roundtrip->bitWidth(nullable) == 8);
    CHECK_THROWS(
        LayerSchema::fromJsonSchema({{"type", "integer"}, {"x-mapget", {{"bitWidth", 8}}}}));
    for (auto [id, width] : {std::pair{number, 8u}, std::pair{mask, 0u}, std::pair{mask, 65u}}) {
        auto invalid = mutateGenericDomains(
            *graph,
            [id = id, width = width](auto& payload, size_t start)
            {
                uint32_t flags = 0;
                // Generic codec-v1 rows are 76 bytes; flags follow the kind.
                auto offset = start + size_t(id) * 76 + sizeof(uint32_t);
                std::memcpy(&flags, payload.data() + offset, sizeof(flags));
                flags = (flags & ~0xff00u) | (width << 8);
                std::memcpy(payload.data() + offset, &flags, sizeof(flags));
            });
        CHECK_THROWS(MapSchema::read(invalid));
    }
}

TEST_CASE(
    "MapSchema rejects recursive overlay payloads and oversized native columns",
    "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Relations", LayerType::Features);
    auto attribute = layer->addSchema(LayerSchema::AttributeKind, "SharedAttribute", "Attribute");
    for (auto name : {"A", "B"}) {
        layer->addFeatureType(identity(name));
        layer->addFieldSchema(layer->featureSchema(name), "attribute", attribute);
    }
    graph->finalize();
    auto first = layer->attributeQuerySchema("A", attribute),
         second = layer->attributeQuerySchema("B", attribute);
    REQUIRE(first);
    REQUIRE(second > first);
    CHECK(
        MapSchema::read(mutateDomains(*graph, [](auto&, auto) {}))->schemaId() ==
        graph->schemaId());
    auto chained = mutateGenericDomains(
        *graph,
        [&](auto& payload, size_t start)
        {
            // Generic codec-v1 stores the inherited base at byte 16 of each row.
            uint32_t base = first;
            std::memcpy(payload.data() + start + size_t(second) * 76 + 16, &base, sizeof(base));
        });
    CHECK_THROWS_AS(MapSchema::read(chained), std::invalid_argument);
    auto orphan = mutateGenericDomains(
        *graph,
        [&](auto& payload, size_t start)
        {
            uint32_t noBase = 0;
            std::memcpy(payload.data() + start + size_t(second) * 76 + 16, &noBase, sizeof(noBase));
        });
    CHECK_THROWS_AS(MapSchema::read(orphan), std::invalid_argument);
    auto invalidMapFlags = mutateDomains(
        *graph,
        [&](auto& payload, size_t start)
        {
            uint32_t flags = 2;
            std::memcpy(payload.data() + start + size_t(attribute) * 40, &flags, sizeof(flags));
        });
    CHECK_THROWS_AS(MapSchema::read(invalidMapFlags), std::invalid_argument);
    auto oversized = mutateDomains(
        *graph,
        [](auto& payload, size_t start)
        {
            uint64_t length = (uint64_t{1} << 24) * 40;
            std::memcpy(payload.data() + start - sizeof(length), &length, sizeof(length));
        });
    CHECK_THROWS_AS(MapSchema::read(oversized), std::length_error);
    auto oversizedGeneric = mutateGenericDomains(
        *graph,
        [](auto& payload, size_t start)
        {
            uint64_t length = (uint64_t{1} << 24) * 76;
            std::memcpy(payload.data() + start - sizeof(length), &length, sizeof(length));
        });
    CHECK_THROWS_AS(MapSchema::read(oversizedGeneric), std::length_error);
}

TEST_CASE(
    "MapSchema validates embedded generic domain flags, inheritance and version",
    "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Values", LayerType::SourceData);
    auto object = layer->addSchema(simfil::Schema::Kind::Object);
    layer->setRootSchema(object);
    graph->finalize();
    CHECK(
        MapSchema::read(mutateGenericDomains(*graph, [](auto&, auto) {}))->schemaId() ==
        graph->schemaId());

    SECTION("Reserved generic flags")
    {
        auto invalid = mutateGenericDomains(
            *graph,
            [&](auto& payload, size_t start)
            {
                uint32_t flags = 8;
                std::memcpy(
                    payload.data() + start + size_t(object) * 76 + 4,
                    &flags,
                    sizeof(flags));
            });
        CHECK_THROWS_AS(MapSchema::read(invalid), std::invalid_argument);
    }
    SECTION("Cyclic inherited domain")
    {
        auto invalid = mutateGenericDomains(
            *graph,
            [&](auto& payload, size_t start)
            {
                uint32_t base = object;
                std::memcpy(payload.data() + start + size_t(object) * 76 + 16, &base, sizeof(base));
            });
        CHECK_THROWS_AS(MapSchema::read(invalid), std::invalid_argument);
    }
    SECTION("Unknown inherited domain")
    {
        auto invalid = mutateGenericDomains(
            *graph,
            [&](auto& payload, size_t start)
            {
                uint32_t base = uint32_t(graph->domainCount() + 1);
                std::memcpy(payload.data() + start + size_t(object) * 76 + 16, &base, sizeof(base));
            });
        CHECK_THROWS_AS(MapSchema::read(invalid), std::invalid_argument);
    }
    SECTION("Unsupported generic payload version")
    {
        auto invalid = mutateGenericDomains(
            *graph,
            [](auto& payload, size_t)
            {
                uint32_t version = 2;
                std::memcpy(payload.data(), &version, sizeof(version));
            });
        CHECK_THROWS_AS(MapSchema::read(invalid), std::invalid_argument);
    }
}

TEST_CASE("MapSchema rejects aliased numeric storage and invalid flag declarations", "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Values", LayerType::SourceData);
    auto first = layer->addSchema(simfil::Schema::Kind::Int);
    auto second = layer->addSchema(simfil::Schema::Kind::Int);
    auto mask = layer->addSchema(LayerSchema::BitmaskKind);
    std::array<simfil::Schema::NumericRange, 1> ranges{{{int64_t{0}, int64_t{10}}}};
    simfil::Schema::NumericDomain numeric;
    numeric.ordinaryRanges = ranges;
    layer->setNumericDomain(first, numeric);
    layer->setNumericDomain(second, numeric);
    layer->setBitWidth(mask, 8);
    layer->addNamedValue(mask, {"BIT", uint64_t{1}, simfil::Schema::NamedValue::Role::Flag});
    layer->setRootSchema(first);
    graph->finalize();
    CHECK(MapSchema::read(bytes(*graph))->schemaId() == graph->schemaId());

    // Locate a generic codec-v1 column by its serialized counts, not a fixed
    // payload offset or a compiler-dependent sizeof of the native records.
    auto column = [&](std::string& payload, size_t domains, size_t index)
    {
        constexpr std::array<size_t, 8> widths{12, 2, 2, 4, 16, 16, 72, 4};
        REQUIRE(index < widths.size());
        using Adapter = bitsery::InputBufferAdapter<std::string>;
        bitsery::Deserializer<Adapter> input(Adapter(payload.begin(), payload.end()));
        input.adapter().currentReadPos(domains + (graph->domainCount() + 1) * 76);
        for (size_t i = 0; i < index; ++i) {
            size_t count = 0;
            bitsery::details::readSize(input.adapter(), count, 4 * 1024 * 1024, std::true_type{});
            input.adapter().currentReadPos(input.adapter().currentReadPos() + count * widths[i]);
        }
        size_t count = 0;
        bitsery::details::readSize(input.adapter(), count, 4 * 1024 * 1024, std::true_type{});
        REQUIRE(input.adapter().error() == bitsery::ReaderError::NoError);
        return std::pair{input.adapter().currentReadPos(), count};
    };
    SECTION("Two domains must not expand the same numeric row")
    {
        auto invalid = mutateGenericDomains(
            *graph,
            [&](auto& payload, size_t start)
            {
                uint32_t numericRow = 0;
                std::memcpy(&numericRow, payload.data() + start + size_t(first) * 76 + 12, 4);
                REQUIRE(numericRow == 1);
                std::memcpy(payload.data() + start + size_t(second) * 76 + 12, &numericRow, 4);
            });
        CHECK_THROWS_WITH(
            MapSchema::read(invalid),
            "Numeric rows must have one canonical domain owner");
    }
    SECTION("Numeric rows must not expand overlapping interval slices")
    {
        auto invalid = mutateGenericDomains(
            *graph,
            [&](auto& payload, size_t start)
            {
                auto [offset, count] = column(payload, start, 6);
                REQUIRE(count == 2);
                // NumericData's interval range follows the two bounds and unit transform.
                uint32_t firstRange = 0, secondRange = 0;
                std::memcpy(&firstRange, payload.data() + offset + 56, 4);
                std::memcpy(&secondRange, payload.data() + offset + 72 + 56, 4);
                REQUIRE(firstRange == 0);
                REQUIRE(secondRange == 2);
                std::memcpy(payload.data() + offset + 72 + 56, &firstRange, 4);
            });
        CHECK_THROWS_WITH(MapSchema::read(invalid), "Schema slices must be disjoint and canonical");
    }
    SECTION("Flag masks must be nonnegative integers within the declared width")
    {
        for (auto [type, bits, message] :
             {std::tuple<uint8_t, uint64_t, char const*>{
                  4,
                  0,
                  "Flag masks must be nonnegative integers"},
              {0, 1, "Flag masks must be nonnegative integers"},
              {1, UINT64_MAX, "Flag masks must be nonnegative integers"},
              {2, 256, "Flag declaration exceeds its encoding width"}})
        {
            auto invalid = mutateGenericDomains(
                *graph,
                [&](auto& payload, size_t start)
                {
                    auto [offset, count] = column(payload, start, 4);
                    REQUIRE(count == 1);
                    std::memcpy(payload.data() + offset, &bits, sizeof(bits));
                    payload[offset + 14] = char(type);
                });
            CHECK_THROWS_WITH(MapSchema::read(invalid), message);
        }
    }
}

TEST_CASE("MapSchema bounds repeated byte enum materialization before indexing", "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Bytes", LayerType::SourceData);
    auto value = layer->addSchema(simfil::Schema::Kind::Bytes);
    layer->setRootSchema(value);
    layer->addEnumValue(value, simfil::ByteArray(std::string(1024, 'x')));
    for (size_t i = 1; i < 64; ++i)
        layer->addEnumValue(value, std::monostate{});
    graph->finalize();
    auto repeated = mutateGenericDomains(
        *graph,
        [&](auto& payload, size_t domains)
        {
            using Adapter = bitsery::InputBufferAdapter<std::string>;
            bitsery::Deserializer<Adapter> input(Adapter(payload.begin(), payload.end()));
            input.adapter().currentReadPos(domains + (graph->domainCount() + 1) * 76);
            for (auto width : {12u, 2u, 2u}) {
                size_t count = 0;
                bitsery::details::readSize(
                    input.adapter(),
                    count,
                    4 * 1024 * 1024,
                    std::true_type{});
                input.adapter().currentReadPos(input.adapter().currentReadPos() + count * width);
            }
            size_t count = 0;
            bitsery::details::readSize(input.adapter(), count, 4 * 1024 * 1024, std::true_type{});
            REQUIRE(count == 64);
            auto first = input.adapter().currentReadPos();
            for (size_t i = 1; i < count; ++i)
                std::memcpy(payload.data() + first + i * 4, payload.data() + first, 4);
        });
    REQUIRE(repeated.size() < 64 * 1024);
    CHECK_THROWS_WITH(
        MapSchema::read(repeated, {}, repeated.size()),
        "Schema literal materialization exceeds its byte budget");
}

TEST_CASE("MapSchema remaps legacy IDs and rejects conflicting imports atomically", "[mapschema]")
{
    nlohmann::json domain = {
        {"type", "integer"},
        {"x-mapget", {{"schemaId", 1}, {"schemaKey", "legacy.value"}}}};
    nlohmann::json legacy = {
        {"definitions", {{"1", domain}}},
        {"x-mapget", {{"domainDefinitions", true}}}};
    auto schema = std::make_shared<MapSchema>();
    auto global = schema->addSchema(simfil::Schema::Kind::String, "global");
    auto a = schema->importLayer("A", LayerType::SourceData, legacy);
    auto b = schema->importLayer("B", LayerType::SourceData, legacy);
    CHECK(a->schemaId("legacy.value") != global);
    CHECK(a->schemaId("legacy.value") != b->schemaId("legacy.value"));
    CHECK(schema->importLayer("A", LayerType::SourceData, legacy)->addr() == a->addr());
    auto before = schema->domainCount();
    legacy["definitions"]["1"]["type"] = "string";
    CHECK_THROWS(schema->importLayer("A", LayerType::SourceData, legacy));
    CHECK(schema->domainCount() == before);
    std::vector<FeatureTypeInfo> duplicate{identity("Road"), identity("Road", "different")};
    CHECK_THROWS(schema->importLayer("Broken", LayerType::Features, nullptr, duplicate));
    CHECK_FALSE(schema->layer("Broken"));
    CHECK(schema->domainCount() == before);
    schema->finalize();
    CHECK(schema->layerNames().size() == 2);
}

TEST_CASE(
    "MapSchema cycles share compact reachability and preserve conservative unknowns",
    "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Cycle", LayerType::SourceData);
    auto a = graph->addSchema(simfil::Schema::Kind::Object),
         b = graph->addSchema(simfil::Schema::Kind::Object);
    graph->addFieldSchema(a, "next", b);
    graph->addFieldSchema(b, "previous", a);
    layer->setRootSchema(a);
    graph->finalize();
    CHECK(layer->reachabilityComplete(a));
    CHECK(layer->canHaveField(a, "previous"));
    CHECK_FALSE(layer->canHaveField(a, "absent"));
    CHECK(layer->nestedFields(a).data() == layer->nestedFields(b).data());
    auto descriptor = graph->domainNode(a);
    REQUIRE(descriptor);
    CHECK(descriptor->type() == simfil::ValueType::Object);
    auto unknown = std::make_shared<MapSchema>();
    auto open = unknown->newLayer("Open", LayerType::SourceData);
    auto root = unknown->addSchema(simfil::Schema::Kind::Object);
    unknown->setOpen(root, true);
    open->setRootSchema(root);
    unknown->finalize();
    CHECK_FALSE(open->reachabilityComplete(root));
    CHECK(open->canHaveField(root, "absent"));
}

TEST_CASE("Nested schema names are lexical while direct declarations retain order", "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Order", LayerType::SourceData);
    auto root = graph->addSchema(simfil::Schema::Kind::Object);
    auto value = graph->addSchema(simfil::Schema::Kind::String);
    graph->addFieldSchema(root, "z", value);
    graph->addFieldSchema(root, "a", value);
    graph->addEnumSymbol(value, "Z");
    graph->addEnumSymbol(value, "A");
    layer->setRootSchema(root);
    graph->finalize();
    CHECK(std::ranges::equal(layer->directFields(root), std::vector<std::string>{"z", "a"}));
    CHECK(std::ranges::equal(layer->nestedFields(root), std::vector<std::string>{"a", "z"}));
    CHECK(std::ranges::equal(layer->directEnumSymbols(value), std::vector<std::string>{"Z", "A"}));
    CHECK(std::ranges::equal(layer->nestedEnumSymbols(value), std::vector<std::string>{"A", "Z"}));
    CHECK(layer->canHaveField(root, "a"));
    CHECK(layer->canHaveEnumSymbol(root, "Z"));
    auto decoded = MapSchema::read(bytes(*graph))->layer("Order");
    CHECK(std::ranges::equal(decoded->nestedFields(root), layer->nestedFields(root)));
    CHECK(std::ranges::equal(decoded->directEnumSymbols(value), layer->directEnumSymbols(value)));
}

TEST_CASE(
    "MapSchema stores named specials and full-width masks without label ordinals",
    "[mapschema]")
{
    auto schema = std::make_shared<MapSchema>();
    auto layer = schema->newLayer("Values", LayerType::SourceData);
    auto speed = schema->addSchema(simfil::Schema::Kind::Int, "Speed");
    std::vector<simfil::Schema::NumericRange> ranges{{int64_t{1}, int64_t{253}}};
    schema->setNumericDomain(
        speed,
        {.encoding = "uint8",
         .unit = "km/h",
         .minimum = int64_t{0},
         .maximum = int64_t{255},
         .ordinaryRanges = ranges,
         .complete = true});
    schema->addNamedValue(
        speed,
        {.name = "UNLIMITED",
         .value = int64_t{255},
         .role = simfil::Schema::NamedValue::Role::Special,
         .meaning = simfil::Schema::NamedValue::Meaning::Unbounded});
    auto flags = schema->addSchema(LayerSchema::BitmaskKind, "Flags");
    schema->setBitWidth(flags, 64);
    schema->addNamedValue(
        flags,
        {.name = "TOP",
         .value = uint64_t{1} << 63,
         .role = simfil::Schema::NamedValue::Role::Flag});
    layer->setRootSchema(speed);
    schema->finalize();
    auto restored = MapSchema::read(bytes(*schema), schema->schemaId());
    auto numeric = restored->numericDomain(speed);
    REQUIRE(numeric);
    CHECK(numeric->unit == "km/h");
    CHECK(numeric->ordinaryRanges.size() == 1);
    CHECK(std::get<int64_t>(numeric->ordinaryRanges[0].max) == 253);
    auto special = restored->domain(speed)->namedValue("UNLIMITED");
    REQUIRE(special);
    CHECK(std::get<int64_t>(special->value) == 255);
    auto mask = restored->domain(flags)->namedValue("TOP");
    REQUIRE(mask);
    CHECK(std::get<uint64_t>(mask->value) == uint64_t{1} << 63);
    CHECK(restored->domain(speed)->enumValues().empty());
}

TEST_CASE(
    "Standard JSON bounds populate native numeric domains without overriding semantics",
    "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->importLayer(
        "Bounds",
        LayerType::SourceData,
        {{"type", "object"},
         {"properties",
          {{"count", {{"type", "integer"}, {"minimum", 0}, {"maximum", uint64_t(UINT64_MAX)}}},
           {"distance", {{"type", "number"}, {"minimum", -1.5}, {"maximum", 12.75}}},
           {"exclusive", {{"type", "number"}, {"exclusiveMinimum", 0}}},
           {"label", {{"type", "string"}, {"minimum", 0}}},
           {"unknown", {{"minimum", 0}, {"maximum", 255}}},
           {"speed",
            {{"type", "integer"},
             {"minimum", 0},
             {"maximum", 250},
             {"x-mapget",
              {{"numericDomain",
                {{"encoding", "uint8"},
                 {"unit", "km/h"},
                 {"minimum", 0},
                 {"maximum", 255},
                 {"ordinaryRanges", {{{"minimum", 1}, {"maximum", 253}}}},
                 {"complete", true}}}}}}}}}});
    graph->finalize();
    auto count = layer->childSchema(layer->rootSchema(), "count");
    auto bounds = layer->numericDomain(count);
    REQUIRE(bounds);
    REQUIRE(bounds->minimum);
    REQUIRE(bounds->maximum);
    CHECK(std::get<int64_t>(*bounds->minimum) == 0);
    CHECK(std::get<uint64_t>(*bounds->maximum) == UINT64_MAX);
    CHECK(bounds->encoding.empty());
    CHECK(bounds->unit.empty());
    CHECK(bounds->ordinaryRanges.empty());
    CHECK_FALSE(bounds->complete);
    CHECK(graph->domainNode(count)->toJson()["numericDomain"]["minimum"] == 0);
    auto distance = layer->childSchema(layer->rootSchema(), "distance");
    auto metres = layer->numericDomain(distance);
    REQUIRE(metres);
    CHECK(std::get<double>(*metres->minimum) == -1.5);
    CHECK(std::get<double>(*metres->maximum) == 12.75);
    for (auto name : {"exclusive", "label", "unknown"})
        CHECK_FALSE(layer->numericDomain(layer->childSchema(layer->rootSchema(), name)));
    auto speed = layer->childSchema(layer->rootSchema(), "speed");
    auto explicitDomain = layer->numericDomain(speed);
    REQUIRE(explicitDomain);
    CHECK(std::get<int64_t>(*explicitDomain->maximum) == 255);
    CHECK(explicitDomain->unit == "km/h");
    REQUIRE(explicitDomain->ordinaryRanges.size() == 1);
    CHECK(std::get<int64_t>(explicitDomain->ordinaryRanges[0].max) == 253);
    CHECK(explicitDomain->complete);
    auto restored = MapSchema::read(bytes(*graph))->layer("Bounds");
    CHECK(std::get<uint64_t>(*restored->numericDomain(count)->maximum) == UINT64_MAX);
    CHECK(std::get<double>(*restored->numericDomain(distance)->minimum) == -1.5);
    auto exported = layer->toJsonSchema();
    CHECK(exported["definitions"][std::to_string(speed)]["maximum"] == 250);
    CHECK(LayerSchema::fromJsonSchema(exported)->toJsonSchema() == exported);
}

TEST_CASE("Named numeric values never become exhaustive JSON enums", "[mapschema]")
{
    // Mirrors the Classic converter contract: ordinary numeric measurements and
    // a few named constants coexist; only the string field is a closed enum.
    auto layer = LayerSchema::fromJsonSchema(nlohmann::json::parse(R"json({
        "type":"object","additionalProperties":false,
        "properties":{
            "speed":{"type":"integer","minimum":0,"maximum":255,"x-mapget":{
                "namedValues":[
                    {"name":"SPEED_UNLIMITED","value":255,"role":"special","meaning":"unbounded"},
                    {"name":"SPEED_VARIABLE","value":254,"role":"special","meaning":"variable"}
                ],
                "numericDomain":{"encoding":"uint8","minimum":0,"maximum":255,
                    "ordinaryRanges":[{"minimum":1,"maximum":253}],"unit":"km/h","complete":true}
            }},
            "baseSpeed":{"type":"integer","minimum":1,"maximum":255,"x-mapget":{
                "namedValues":[{"name":"MIN_AVERAGE_SPEED","value":1,"role":"constant"}]
            }},
            "phase":{"type":"string","enum":["ACTIVE","WAITING"],"x-mapget":{
                "namedValues":[{"name":"ACTIVE","value":"ACTIVE","role":"constant"}]
            }}
        }
    })json"));
    auto binary = MapSchema::read(bytes(*layer->mapSchema()));
    for (auto current : {layer, binary->layer("legacy")}) {
        auto exported = current->toJsonSchema();
        auto speed = current->childSchema(current->rootSchema(), "speed");
        auto baseSpeed = current->childSchema(current->rootSchema(), "baseSpeed");
        auto phase = current->childSchema(current->rootSchema(), "phase");
        CHECK(current->directEnumSymbols(speed).empty());
        CHECK(current->nestedEnumSymbols(speed).empty());
        CHECK(std::ranges::equal(
            current->nestedEnumSymbols(current->rootSchema()),
            std::vector<std::string>{"ACTIVE", "WAITING"}));
        auto descriptor = current->mapSchema()->domainNode(speed)->toJson();
        CHECK_FALSE(descriptor.contains("enum"));
        CHECK(descriptor["namedValues"].size() == 2);
        CHECK_FALSE(exported["definitions"][std::to_string(speed)].contains("enum"));
        CHECK_FALSE(exported["definitions"][std::to_string(baseSpeed)].contains("enum"));
        CHECK(
            exported["definitions"][std::to_string(phase)]["enum"] ==
            nlohmann::json::array({"ACTIVE", "WAITING"}));
        CHECK(
            exported["definitions"][std::to_string(speed)]["x-mapget"]["namedValues"].size() == 2);
        auto roundtrip = LayerSchema::fromJsonSchema(exported);
        CHECK(roundtrip->toJsonSchema() == exported);
        nlohmann::json_schema::json_validator validator;
        REQUIRE_NOTHROW(validator.set_root_schema(exported));
        CHECK_NOTHROW(validator.validate({{"speed", 30}, {"baseSpeed", 1}, {"phase", "ACTIVE"}}));
        CHECK_NOTHROW(
            validator.validate({{"speed", 255}, {"baseSpeed", 42}, {"phase", "WAITING"}}));
        CHECK_THROWS(validator.validate({{"speed", 256}}));
        CHECK_THROWS(validator.validate({{"speed", "SPEED_UNLIMITED"}}));
        CHECK_THROWS(validator.validate({{"phase", "UNKNOWN"}}));

        auto strings = std::make_shared<StringPool>("NamedValueNormalization");
        auto env = makeEnvironment(strings);
        installCompletionLayerSchema(*env, current, strings);
        auto data = std::make_shared<simfil::ModelPool>(strings);
        data->setSchemaBinding(current->mapSchema()->binding());
        auto root = data->newObject();
        REQUIRE(root->setSchema(current->rootSchema()));
        root->addField("speed", data->newSmallValue(int16_t{255}));
        auto compiled = simfil::compile(
            *env,
            "speed = SPEED_UNLIMITED",
            {.any = false,
             .rewriteMode = simfil::RewriteMode::Schema,
             .rootSchema = current->rootSchema()});
        REQUIRE(compiled);
        auto values = simfil::eval(*env, **compiled, *root, nullptr);
        REQUIRE(values);
        REQUIRE(values->size() == 1);
        CHECK(values->front().as<simfil::ValueType::Bool>());
        auto arithmetic = simfil::compile(
            *env,
            "SPEED_UNLIMITED + 1",
            {.any = false,
             .rewriteMode = simfil::RewriteMode::Schema,
             .rootSchema = current->rootSchema()});
        REQUIRE(arithmetic);
        CHECK((*arithmetic)->expr().toString().find("\"SPEED_UNLIMITED\"") == std::string::npos);
    }
}

TEST_CASE("Only owned attribute query roots rewrite type-code symbols", "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Road", LayerType::Features);
    layer->addFeatureType(identity("Road"));
    auto feature = layer->featureSchema("Road");
    auto attribute = layer->addSchema(LayerSchema::AttributeKind, {}, "Attribute");
    layer->setAttributeMetadata(attribute, {"Road", "rules", "SPEED", attribute}, "Speed");
    layer->addFieldSchema(feature, "speed", attribute);
    layer->addFieldSchema(attribute, "limit", layer->addSchema(simfil::Schema::Kind::Int));
    graph->finalize();
    auto strings = std::make_shared<StringPool>("SymbolRewriteRegression");
    auto env = makeEnvironment(strings);
    installCompletionLayerSchema(*env, layer, strings);
    auto code = strings->emplace("SPEED").value();
    auto const& lookup = env->querySchemaCallback;
    CHECK(lookup(feature)->symbolEqualityPaths(simfil::StringPool::Empty, lookup).empty());
    CHECK(lookup(attribute)->symbolEqualityPaths(code, lookup).empty());
    CHECK(layer->scalarFieldPathsForAttribute(feature, "").empty());
    auto query = layer->attributeQuerySchema("Road", attribute);
    REQUIRE(query);
    auto limit = layer->childSchema(attribute, "limit");
    // Point lookup includes both inherited payload fields and explicit context
    // fields; their recursive enumeration policy is a separate concern.
    CHECK(lookup(query)->fieldSchema(strings->get("limit")) == limit);
    CHECK(lookup(query)->fieldSchema(strings->get("$feature")) == feature);
    CHECK(graph->domain(query)->fieldSchema(graph->strings()->get("limit")) == limit);
    auto paths = lookup(query)->symbolEqualityPaths(code, lookup);
    REQUIRE(paths.size() == 1);
    REQUIRE(paths[0].size() == 1);
    CHECK(paths[0][0].field == strings->get("$name"));
    auto ast = simfil::compile(
        *env,
        "''",
        {.any = false, .rewriteMode = simfil::RewriteMode::Schema, .rootSchema = feature});
    REQUIRE(ast);
    auto data = std::make_shared<simfil::ModelPool>(strings);
    auto value = data->newObject();
    auto result = simfil::eval(*env, **ast, *value, nullptr);
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK(result->front().as<simfil::ValueType::String>().empty());
}

TEST_CASE(
    "Attribute shorthand prunes shared DAGs without losing alias or cyclic paths",
    "[mapschema]")
{
    using Kind = simfil::Schema::Kind;
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Road", LayerType::Features);
    layer->addFeatureType(identity("Road"));
    auto root = layer->featureSchema("Road");
    auto number = layer->addSchema(Kind::Int);
    auto sharedDag = [&](simfil::SchemaId leaf)
    {
        // Only 18 nodes, but more than 100k distinct path prefixes without pruning.
        for (size_t i = 0; i < 18; ++i) {
            auto parent = layer->addSchema(Kind::Object);
            layer->addFieldSchema(parent, "left", leaf);
            layer->addFieldSchema(parent, "right", leaf);
            leaf = parent;
        }
        return leaf;
    };
    auto unrelated = sharedDag(number);
    auto emptyPayload = sharedDag(layer->addSchema(Kind::Object));
    auto attribute = layer->addSchema(LayerSchema::AttributeKind, {}, "Attribute");
    layer->setAttributeMetadata(attribute, {"Road", "rules", "SPEED", attribute}, "Speed");
    layer->addFieldSchema(attribute, "$name", layer->addSchema(Kind::String));
    layer->addFieldSchema(attribute, "validity", number);
    layer->addFieldSchema(attribute, "empty", emptyPayload);
    layer->addFieldSchema(attribute, "value", number);
    layer->addFieldSchema(attribute, "extraValue", number);
    auto cycleA = layer->addSchema(Kind::Object);
    auto cycleB = layer->addSchema(Kind::Object);
    layer->addFieldSchema(cycleA, "cycle", cycleB);
    layer->addFieldSchema(cycleA, "target", attribute);
    layer->addFieldSchema(cycleB, "cycle", cycleA);
    layer->addFieldSchema(root, "unrelated", unrelated);
    layer->addFieldSchema(root, "roadId", number);
    layer->addFieldSchema(root, "primary", attribute);
    layer->addFieldSchema(root, "secondary", attribute);
    layer->addFieldSchema(root, "firstCycle", cycleA);
    layer->addFieldSchema(root, "secondCycle", cycleB);
    graph->finalize();
    auto binary = MapSchema::read(bytes(*graph))->layer("Road");
    for (auto current : {layer, binary}) {
        // Real Live regression: roadId is a field, not an attribute shorthand.
        CHECK(current->scalarFieldPathsForAttribute(root, "roadId").empty());
        CHECK(current->scalarFieldPathsForAttribute(root, "absent").empty());
        CHECK(current->scalarFieldPathsForAttribute(unrelated, "SPEED").empty());
        auto overlay = current->attributeQuerySchema("Road", attribute);
        REQUIRE(overlay);
        auto payloadPaths = current->scalarFieldPathsForAttribute(overlay, "SPEED");
        REQUIRE(payloadPaths.size() == 1);
        REQUIRE(payloadPaths.front().size() == 1);
        CHECK(payloadPaths.front().front().kind_ == simfil::SchemaPathSegment::Kind::Field);
        CHECK(payloadPaths.front().front().field_ == "value");
        std::vector<std::vector<std::string>> paths;
        for (auto const& path : current->scalarFieldPathsForAttribute(root, "SPEED")) {
            auto& names = paths.emplace_back();
            for (auto const& segment : path) {
                REQUIRE(segment.kind_ == simfil::SchemaPathSegment::Kind::Field);
                names.push_back(segment.field_);
            }
        }
        std::vector<std::vector<std::string>> expected{
            {"primary", "value"},
            {"secondary", "value"},
            {"firstCycle", "target", "value"},
            {"secondCycle", "cycle", "target", "value"}};
        std::ranges::sort(expected);
        CHECK(paths == expected);

        auto strings = std::make_shared<StringPool>("SharedDagShorthand");
        auto env = makeEnvironment(strings);
        installCompletionLayerSchema(*env, current, strings);
        auto model = std::make_shared<simfil::ModelPool>(strings);
        model->setSchemaBinding(current->mapSchema()->binding());
        auto feature = model->newObject();
        REQUIRE(feature->setSchema(root));
        auto payload = model->newObject();
        REQUIRE(payload->setSchema(attribute));
        payload->addField("value", model->newSmallValue(int16_t{20}));
        payload->addField("validity", model->newSmallValue(int16_t{1000}));
        payload->addField("extraValue", model->newSmallValue(int16_t{1000}));
        auto first = model->newObject();
        auto second = model->newObject();
        first->addField("cycle", second);
        first->addField("target", payload);
        second->addField("cycle", first);
        feature->addField("roadId", model->newSmallValue(int16_t{2}));
        feature->addField("primary", payload);
        feature->addField("secondary", payload);
        feature->addField("firstCycle", first);
        feature->addField("secondCycle", second);
        for (auto [query, expected, count] :
             {std::tuple{"roadId == 2", true, 1u},
              {"SPEED > 10", true, 4u},
              {"SPEED > 100", false, 4u}})
        {
            INFO(query);
            auto compiled = simfil::compile(
                *env,
                query,
                {.any = false, .rewriteMode = simfil::RewriteMode::Schema, .rootSchema = root});
            REQUIRE(compiled);
            auto values = simfil::eval(*env, **compiled, *feature, nullptr);
            REQUIRE(values);
            // With any=false, each matching alias contributes its own comparison result.
            REQUIRE(values->size() == count);
            for (auto const& value : *values)
                CHECK(value.as<simfil::ValueType::Bool>() == expected);
        }
    }
}

TEST_CASE(
    "Attribute shorthand keeps its budget for genuinely numerous matching paths",
    "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Values", LayerType::SourceData);
    auto root = layer->addSchema(simfil::Schema::Kind::Object);
    auto shared = layer->addSchema(simfil::Schema::Kind::Object);
    auto value = layer->addSchema(simfil::Schema::Kind::Int);
    layer->setAttributeMetadata(value, {"Road", "rules", "SPEED", value}, "Speed");
    for (size_t i = 0; i < 1000; ++i) {
        auto name = "field" + std::to_string(i);
        layer->addFieldSchema(root, name, shared);
        layer->addFieldSchema(shared, name, value);
    }
    layer->setRootSchema(root);
    graph->finalize();
    CHECK_THROWS_AS(layer->scalarFieldPathsForAttribute(root, "SPEED"), std::length_error);
}

TEST_CASE("MapSchema incoming candidates derive only from produced forward owners", "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Road", LayerType::Features);
    layer->addFeatureType(identity("Road"));
    layer->addFeatureType(identity("ReferenceOnly"), 0, false);
    auto root = layer->featureSchema("Road");
    layer->setJsonSchemaAnnotations(
        root,
        {{"x-mapget",
          {{"relationDeclarations",
            nlohmann::json::array(
                {{{"ownerType", "Road"},
                  {"name", "next"},
                  {"targetTypes", {"Road"}},
                  {"incoming", false},
                  {"coverage", "complete"},
                  {"locality", "may-cross-container"}},
                 {{"ownerType", "ReferenceOnly"},
                  {"name", "notProduced"},
                  {"targetTypes", {"Road"}}}})}}}});
    graph->finalize();
    CHECK(layer->hasRelationDeclarations());
    auto incoming = layer->relationsTo("Road");
    REQUIRE(incoming.size() == 1);
    CHECK(incoming[0].name_ == "next");
    CHECK(incoming[0].permitsIncoming("Road", "next", "Road"));
    CHECK(incoming[0].complete_);
    CHECK(incoming[0].locality_ == LayerSchema::RelationDeclaration::Locality::MayCrossContainer);
    auto restored = MapSchema::read(bytes(*graph));
    CHECK(restored->layer("Road")->relationsTo("Road")[0].locality_ == incoming[0].locality_);
    auto json = layer->toJsonSchema().dump();
    CHECK(json.find("\"coverage\"") == std::string::npos);
    CHECK(json.find("\"incoming\"") == std::string::npos);
}

TEST_CASE(
    "Legacy schema roots distinguish produced features from referenced ID definitions",
    "[mapschema]")
{
    auto feature = [](std::string name, bool produced = true)
    {
        return nlohmann::json{
            {"type", "object"},
            {"x-mapget", {{"metaType", "Feature"}, {"featureType", name}, {"produced", produced}}}};
    };
    nlohmann::json rich{
        {"oneOf", {{{"$ref", "#/$defs/Carrier"}}, {{"$ref", "#/$defs/Hidden"}}}},
        {"$defs", {{"Carrier", feature("Carrier")}, {"Hidden", feature("Hidden", false)}}}};
    std::vector<FeatureTypeInfo> ids{
        identity("Carrier"),
        identity("ReferenceOnly"),
        identity("Hidden")};
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->importLayer("Road", LayerType::Features, rich, ids);
    graph->finalize();
    CHECK(layer->featureTypeInfos().size() == 3);
    CHECK(layer->featureTypes() == std::vector<std::string>{"Carrier"});
    CHECK_FALSE(layer->producesFeatureType("ReferenceOnly"));
    CHECK_FALSE(layer->producesFeatureType("Hidden"));
    auto standalone = LayerSchema::fromJsonSchema(rich);
    CHECK(standalone->featureTypeInfos().empty());
    CHECK(standalone->featureTypes() == std::vector<std::string>{"Carrier"});
    auto open = std::make_shared<MapSchema>();
    auto legacy = open->importLayer("Road", LayerType::Features, nullptr, ids);
    open->finalize();
    CHECK(legacy->featureTypes().size() == 3);
}

TEST_CASE(
    "Schema-only non-feature roots and null/empty enum values survive transport",
    "[mapschema]")
{
    auto schema = LayerSchema::fromJsonSchema(
        {{"type", "object"},
         {"additionalProperties", false},
         {"properties", {{"value", {{"enum", {nullptr, "", 7}}}}}}});
    auto roundtrip = LayerSchema::fromJsonSchema(schema->toJsonSchema());
    REQUIRE(roundtrip->rootSchema());
    CHECK(roundtrip->childSchema(roundtrip->rootSchema(), "value"));
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Values", LayerType::SourceData);
    auto value = graph->addSchema(simfil::Schema::Kind::Value);
    graph->addEnumValue(value, std::monostate{});
    graph->addEnumSymbol(value, "");
    layer->setRootSchema(value);
    graph->finalize();
    auto restored = MapSchema::read(bytes(*graph));
    auto values = restored->domain(value)->enumValues();
    REQUIRE(values.size() == 1);
    CHECK(std::holds_alternative<std::monostate>(values[0]));
    auto symbols = restored->domain(value)->directEnumSymbols();
    REQUIRE(symbols.size() == 1);
    CHECK(restored->strings()->resolve(symbols[0]).value().empty());
}

TEST_CASE("Nested network/schema decoding shares workers without admission deadlock", "[mapschema]")
{
    auto barrier = std::make_shared<std::barrier<>>(4);
    std::vector<std::future<MapSchema::ConstPtr>> tasks;
    for (int i = 0; i < 4; ++i) {
        auto graph = std::make_shared<MapSchema>(MapFormat{"Nested", std::to_string(i), {}});
        graph->newLayer("Road", LayerType::Features)->addFeatureType(identity("Road"));
        graph->finalize();
        auto encoded = bytes(*graph);
        auto id = std::string(graph->schemaId());
        tasks.push_back(std::async(
            std::launch::async,
            [barrier, encoded, id]
            {
                return MapSchema::getOrBuild(
                    "network:" + id,
                    [barrier, encoded, id]
                    {
                        barrier->arrive_and_wait();
                        return MapSchema::read(encoded, id);
                    });
            }));
    }
    for (auto& task : tasks) {
        REQUIRE(task.wait_for(5s) == std::future_status::ready);
        CHECK(task.get()->finalized());
    }
    CHECK_THROWS(MapSchema::getOrBuild(
        "recursive-test",
        [] { return MapSchema::getOrBuild("recursive-test", [] { return simpleSchema(); }); }));
}

TEST_CASE("Fresh schema workers retain nesting state through failure and retry", "[mapschema]")
{
    auto schema = simpleSchema();
    auto caller = std::this_thread::get_id();
    std::thread::id worker, nested, retried;
    bool failed = false, recursionRejected = false;
    auto result = MapSchema::getOrBuild(
        "mapschema-test-tls-outer",
        [&]
        {
            worker = std::this_thread::get_id();
            try {
                MapSchema::getOrBuild(
                    "mapschema-test-tls-inner",
                    [&]() -> MapSchema::ConstPtr
                    {
                        nested = std::this_thread::get_id();
                        throw std::runtime_error("nested builder failed");
                    });
            }
            catch (std::runtime_error const& error) {
                failed = std::string_view(error.what()) == "nested builder failed";
            }
            // The failed nested key must have been removed on this same worker.
            auto recovered = MapSchema::getOrBuild(
                "mapschema-test-tls-inner",
                [&]
                {
                    retried = std::this_thread::get_id();
                    return schema;
                });
            try {
                MapSchema::getOrBuild(
                    "mapschema-test-tls-outer",
                    [schema] { return schema; },
                    [deadline = std::chrono::steady_clock::now() + 2s]
                    { return std::chrono::steady_clock::now() >= deadline; });
            }
            catch (std::logic_error const&) {
                recursionRejected = true;
            }
            // Successful nested completion must also pop its key before a cache hit.
            return MapSchema::getOrBuild(
                "mapschema-test-tls-inner",
                [recovered] { return recovered; });
        });
    CHECK(result == schema);
    CHECK(worker != caller);
    CHECK(nested == worker);
    CHECK(retried == worker);
    CHECK(failed);
    CHECK(recursionRejected);
}

TEST_CASE(
    "A known forward inventory does not hide another produced owner's unknowns",
    "[mapschema]")
{
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Mixed", LayerType::Features);
    layer->addFeatureType(identity("Known"));
    layer->addFeatureType(identity("Unknown"));
    layer->setJsonSchemaAnnotations(
        layer->featureSchema("Known"),
        {{"x-mapget", {{"relationDeclarations", nlohmann::json::array()}}}});
    graph->finalize();
    CHECK_FALSE(layer->hasRelationDeclarations());
    CHECK_FALSE(MapSchema::read(bytes(*graph))->layer("Mixed")->hasRelationDeclarations());
}

TEST_CASE(
    "MapSchema build coalescing retains owned inputs after cancelling the first waiter",
    "[mapschema]")
{
    auto input = std::make_shared<int>(7);
    std::weak_ptr<int> weak = input;
    auto entered = std::make_shared<std::promise<void>>();
    auto enteredFuture = entered->get_future();
    auto finish = std::make_shared<std::promise<void>>();
    auto gate = finish->get_future().share();
    auto result = std::make_shared<std::promise<MapSchema::ConstPtr>>();
    auto retained = result->get_future();
    std::atomic<bool> cancel = false;
    std::atomic<int> builds = 0;
    auto first = std::async(
        std::launch::async,
        [input, entered, gate, result, &cancel, &builds]
        {
            return MapSchema::getOrBuild(
                "mapschema-test-cancellation",
                [input, entered, gate, result, &builds]
                {
                    ++builds;
                    entered->set_value();
                    gate.wait();
                    auto schema = simpleSchema();
                    result->set_value(schema);
                    return schema;
                },
                [&cancel] { return cancel.load(); });
        });
    REQUIRE(enteredFuture.wait_for(2s) == std::future_status::ready);
    input.reset();
    cancel = true;
    CHECK(first.wait_for(2s) == std::future_status::ready);
    CHECK_THROWS(first.get());
    CHECK_FALSE(weak.expired());
    auto second = std::async(
        std::launch::async,
        [&builds]
        {
            return MapSchema::getOrBuild(
                "mapschema-test-cancellation",
                [&builds]
                {
                    ++builds;
                    return simpleSchema();
                });
        });
    finish->set_value();
    auto schema = second.get();
    CHECK(schema->finalized());
    CHECK(builds == 1);
    for (int i = 0; i < 100 && !weak.expired(); ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(weak.expired());
}
