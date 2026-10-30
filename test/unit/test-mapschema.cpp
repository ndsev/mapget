#include <catch2/catch_test_macros.hpp>

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

/** Mutate a checksum-valid codec payload, so malformed-graph tests exercise the
 * structural reader rather than merely the envelope digest check. */
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
        auto malformed = mutateDomains(
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

TEST_CASE("MapSchema codec v1 matches the committed binary golden fixture", "[mapschema]")
{
    // This is a compatibility contract, not a regeneratable snapshot. Replacing the
    // fixture requires a deliberate codec-version/cache-identity compatibility decision.
    constexpr auto expectedId =
        "mapget-schema:sha256:b94b69a8bb5e07be61ffa298e7596427cac1fb3d6bdcf0fd3495bfabebf7c5ee";
    constexpr auto expectedSha = "707951a44c9504e2c6a2bc60e8966d0a0d077ea7847cb1020af885880bf08daa";
    std::ifstream input(MAPGET_TEST_DATA_DIR "/mapschema-v1.bin", std::ios::binary);
    REQUIRE(input.is_open());
    std::vector<uint8_t>
        golden(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    REQUIRE(golden.size() == 3048);
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
    schema->importLayer("Values", LayerType::SourceData, nlohmann::json::parse(R"json({
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
        auto invalid = mutateDomains(
            *graph,
            [&](auto& payload, size_t start)
            {
                uint32_t flags = 0;
                auto offset = start + size_t(id) * 104 + sizeof(uint32_t);
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
    auto chained = mutateDomains(
        *graph,
        [&](auto& payload, size_t start)
        {
            // Codec v1 DomainData is 104 bytes; its final uint32 is the payload base.
            uint32_t base = first;
            std::memcpy(payload.data() + start + size_t(second) * 104 + 100, &base, sizeof(base));
        });
    CHECK_THROWS_AS(MapSchema::read(chained), std::invalid_argument);
    auto orphan = mutateDomains(
        *graph,
        [&](auto& payload, size_t start)
        {
            uint32_t noBase = 0;
            std::memcpy(
                payload.data() + start + size_t(second) * 104 + 100,
                &noBase,
                sizeof(noBase));
        });
    CHECK_THROWS_AS(MapSchema::read(orphan), std::invalid_argument);
    auto oversized = mutateDomains(
        *graph,
        [](auto& payload, size_t start)
        {
            uint64_t length = (uint64_t{1} << 24) * 104;
            std::memcpy(payload.data() + start - sizeof(length), &length, sizeof(length));
        });
    CHECK_THROWS_AS(MapSchema::read(oversized), std::length_error);
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
