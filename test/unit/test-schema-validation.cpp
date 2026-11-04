#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json-schema.hpp>

#include "mapget/model/mapschema.h"

#include <initializer_list>
#include <span>
#include <sstream>
#include <string_view>

using namespace mapget;
using namespace nlohmann::literals;

namespace
{
using Json = nlohmann::json;

struct Sample
{
    Json value;
    bool accepted;
};

bool accepts(nlohmann::json_schema::json_validator& validator, Json const& value)
{
    nlohmann::json_schema::basic_error_handler errors;
    validator.validate(value, errors);
    return !errors;
}

/** Check actual validation, not equality between two already-lossy schema exports. */
void preservesAcceptance(Json const& original, std::initializer_list<Sample> samples)
{
    nlohmann::json_schema::json_validator before;
    REQUIRE_NOTHROW(before.set_root_schema(original));
    for (auto const& sample : samples) {
        INFO(sample.value.dump());
        REQUIRE(accepts(before, sample.value) == sample.accepted);
    }

    auto metadata = LayerInfo::fromJson(
        {{"type", "SourceData"}, {"featureModelSchema", original}},
        "Validation");
    auto native = metadata->layerSchema();
    REQUIRE(native);

    for (auto transport : {"legacy import", "native binary", "offset JSON import"}) {
        DYNAMIC_SECTION(transport)
        {
            auto layer = native;
            if (std::string_view(transport) == "native binary") {
                std::ostringstream stream(std::ios::binary);
                REQUIRE(metadata->schemaOwner()->write(stream));
                auto bytes = stream.str();
                auto owner = MapSchema::read(
                    std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()),
                    metadata->schemaOwner()->schemaId());
                layer = owner->layer("Validation");
            }
            else if (std::string_view(transport) == "offset JSON import") {
                auto owner = std::make_shared<MapSchema>();
                (void)owner->addSchema(simfil::Schema::Kind::String, "preexisting");
                layer =
                    owner->importLayer("Validation", LayerType::SourceData, native->toJsonSchema());
                owner->finalize();
            }
            REQUIRE(layer);
            auto exported = layer->toJsonSchema();
            nlohmann::json_schema::json_validator after;
            INFO(exported.dump());
            REQUIRE_NOTHROW(after.set_root_schema(exported));
            for (auto const& sample : samples) {
                INFO(sample.value.dump());
                CHECK(accepts(after, sample.value) == accepts(before, sample.value));
            }
        }
    }
}
}  // namespace

TEST_CASE("Legacy schema validation preserves if then else", "[schema-validation][schema-domain]")
{
    preservesAcceptance(
        R"({
        "type":"object",
        "properties":{"mode":{"type":"string"},"value":{"type":"integer"},"fallback":{"type":"boolean"}},
        "if":{"properties":{"mode":{"const":"strict"}},"required":["mode"]},
        "then":{"required":["value"]},"else":{"required":["fallback"]}
    })"_json,
        {{R"({"mode":"strict","value":7})"_json, true},
         {R"({"mode":"strict"})"_json, false},
         {R"({"mode":"other","fallback":false})"_json, true},
         {R"({"mode":"other"})"_json, false},
         {R"({"fallback":true})"_json, true}});
}

TEST_CASE(
    "Legacy schema validation preserves required-only properties",
    "[schema-validation][schema-domain]")
{
    preservesAcceptance(
        R"({"type":"object","required":["id"]})"_json,
        {{R"({"id":null})"_json, true},
         {R"({"id":1})"_json, true},
         {R"({})"_json, false},
         {R"({"other":1})"_json, false}});
}

TEST_CASE(
    "Legacy schema validation preserves schema-valued additional properties",
    "[schema-validation][schema-domain]")
{
    preservesAcceptance(
        R"({
        "type":"object","properties":{"known":{"type":"string"}},
        "additionalProperties":{"type":"integer","minimum":0}
    })"_json,
        {{R"({"known":"road","count":3})"_json, true},
         {R"({"count":"three"})"_json, false},
         {R"({"count":-1})"_json, false},
         {R"({})"_json, true}});
}

TEST_CASE(
    "Legacy schema validation preserves pattern properties",
    "[schema-validation][schema-domain]")
{
    preservesAcceptance(
        R"({
        "type":"object","additionalProperties":false,
        "patternProperties":{"^i_":{"type":"integer"},"^s_":{"type":"string"}}
    })"_json,
        {{R"({"i_count":3,"s_name":"road"})"_json, true},
         {R"({"i_count":"3"})"_json, false},
         {R"({"unknown":3})"_json, false},
         {R"({})"_json, true}});
}

TEST_CASE("Legacy schema validation preserves contains", "[schema-validation][schema-domain]")
{
    preservesAcceptance(
        R"({"type":"array","items":{"type":"integer"},"contains":{"minimum":5}})"_json,
        {{R"([1,5])"_json, true},
         {R"([1,2])"_json, false},
         {R"([])"_json, false},
         {R"([5,"wrong"])"_json, false}});
}

TEST_CASE("Legacy schema validation preserves negation", "[schema-validation][schema-domain]")
{
    preservesAcceptance(
        R"({"type":"object","not":{"required":["blocked"]}})"_json,
        {{R"({})"_json, true},
         {R"({"allowed":1})"_json, true},
         {R"({"blocked":false})"_json, false}});
}

TEST_CASE(
    "Legacy schema validation preserves property and schema dependencies",
    "[schema-validation][schema-domain]")
{
    preservesAcceptance(
        R"({
        "type":"object","dependencies":{
            "credit_card":["billing_address"],
            "mode":{"required":["value"],"properties":{"value":{"type":"integer","minimum":1}}}
        }
    })"_json,
        {{R"({"credit_card":"card","billing_address":"address"})"_json, true},
         {R"({"credit_card":"card"})"_json, false},
         {R"({"mode":"active","value":1})"_json, true},
         {R"({"mode":"active","value":0})"_json, false},
         {R"({"mode":"active"})"_json, false}});
}

TEST_CASE("Legacy schema validation preserves property names", "[schema-validation][schema-domain]")
{
    preservesAcceptance(
        R"({"type":"object","propertyNames":{"pattern":"^[a-z]+$"}})"_json,
        {{R"({})"_json, true}, {R"({"road":1})"_json, true}, {R"({"Road":1})"_json, false}});
}

TEST_CASE(
    "Legacy schema validation preserves compound constants",
    "[schema-validation][schema-domain]")
{
    preservesAcceptance(
        R"({"type":"object","const":{"mode":"strict","count":2}})"_json,
        {{R"({"mode":"strict","count":2})"_json, true},
         {R"({"mode":"strict","count":3})"_json, false},
         {R"({"mode":"strict","count":2,"extra":true})"_json, false},
         {R"({})"_json, false}});
}

TEST_CASE(
    "Legacy schema validation preserves compound enum values",
    "[schema-validation][schema-domain]")
{
    preservesAcceptance(
        R"({"type":"array","enum":[[1,2],[3]]})"_json,
        {{R"([1,2])"_json, true},
         {R"([3])"_json, true},
         {R"([1])"_json, false},
         {R"([])"_json, false}});
}

TEST_CASE(
    "Legacy validation-only references retain local definitions",
    "[schema-validation][schema-domain]")
{
    preservesAcceptance(
        R"({
        "type":"object","additionalProperties":false,
        "definitions":{"Positive":{"type":"integer","minimum":1}},
        "patternProperties":{"^v_":{"$ref":"#/definitions/Positive"}}
    })"_json,
        {{R"({"v_count":1})"_json, true},
         {R"({"v_count":0})"_json, false},
         {R"({"v_count":"one"})"_json, false},
         {R"({"other":1})"_json, false}});
}

TEST_CASE(
    "Legacy conditional references survive domain remapping",
    "[schema-validation][schema-domain]")
{
    preservesAcceptance(
        R"({
            "type":"object","properties":{"kind":{"type":"string"}},
            "definitions":{"Positive":{"type":"integer","minimum":1}},
            "if":{"properties":{"kind":{"const":"numeric"}},"required":["kind"]},
            "then":{"required":["value"],"properties":{"value":{"$ref":"#/definitions/Positive"}}},
            "else":{"properties":{"value":{"type":"string"}}}
        })"_json,
        {{R"({"kind":"numeric","value":1})"_json, true},
         {R"({"kind":"numeric","value":0})"_json, false},
         {R"({"kind":"numeric"})"_json, false},
         {R"({"kind":"text","value":"road"})"_json, true},
         {R"({"kind":"text","value":1})"_json, false}});
}
