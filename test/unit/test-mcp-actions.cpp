#include <catch2/catch_test_macros.hpp>

#include "../../libs/http-service/src/mcp-action-catalog.h"
#include "../../libs/http-service/src/mcp-viewer-relay.h"

#include <algorithm>
#include <fstream>

using namespace mapget::detail;
using Json = nlohmann::json;

namespace
{

/** Read the pinned cross-language contract without a dependency on an erdblick checkout. */
Json fixture(std::string const& name)
{
    std::ifstream file(std::filesystem::path(MAPGET_TEST_DATA_DIR) / "viewer-actions" / name);
    return Json::parse(file);
}

}  // namespace

TEST_CASE("MCP catalog agrees with the shared browser action fixtures", "[mcp-actions]")
{
    auto catalog = McpActionCatalog::load(
        std::filesystem::path(MAPGET_TEST_DATA_DIR) / "viewer-actions/web-mcp-actions.json");
    auto cases = fixture("fixtures.json");
    for (auto const& entry : cases.at("actions")) {
        INFO(entry.at("name"));
        CHECK(
            catalog
                .acceptsArguments(entry.at("action").get<std::string>(), entry.at("arguments")) ==
            entry.at("valid").get<bool>());
    }
    for (auto const& entry : cases.at("results")) {
        INFO(entry.at("name"));
        CHECK(
            catalog.acceptsResult(entry.at("action").get<std::string>(), entry.at("value")) ==
            entry.at("valid").get<bool>());
    }

    auto schemas = fixture("viewer-action-relay.schema.json");
    for (auto const* group : {"relay", "info"}) {
        for (auto const& entry : cases.at(group)) {
            INFO(entry.at("name"));
            bool isRelay = std::string_view(group) == "relay";
            auto schema = isRelay ? entry.at("direction").get<std::string>() : "info";
            nlohmann::json_schema::json_validator validator(schemas.at(schema));
            nlohmann::json_schema::basic_error_handler errors;
            validator.validate(entry.at(isRelay ? "message" : "value"), errors);
            CHECK(!errors == entry.at("valid").get<bool>());
            if (isRelay && schema == "client") {
                CHECK(
                    McpViewerRelay::acceptsMessage(entry.at("message")) ==
                    entry.at("valid").get<bool>());
            }
        }
    }
}

TEST_CASE(
    "MCP tool schemas add routing without changing application argument validation",
    "[mcp-actions]")
{
    auto manifest = fixture("web-mcp-actions.json");
    McpActionCatalog catalog(manifest);
    CHECK(catalog.id() == manifest.at("catalogId").get<std::string>());
    CHECK(catalog.tools(false, false).empty());
    for (bool control : {false, true}) {
        auto expected = std::count_if(
            manifest["actions"].begin(),
            manifest["actions"].end(),
            [control](auto const& action)
            { return action["permission"] == (control ? "viewer-control" : "viewer-read"); });
        CHECK(catalog.tools(!control, control).size() == expected);
    }
    CHECK_FALSE(catalog.contains("viewer_list_sessions"));
    CHECK_FALSE(catalog.contains("mapget_list_sources"));
    CHECK_FALSE(catalog.acceptsArguments("unknown", Json::object()));
    CHECK_FALSE(catalog.acceptsResult("unknown", Json::object()));
    CHECK(catalog.requiresControl("viewer_set_app_state"));
    CHECK(catalog.isMutation("viewer_set_app_state"));
    CHECK_FALSE(catalog.requiresControl("viewer_get_app_state"));
    CHECK_FALSE(catalog.isMutation("viewer_get_app_state"));

    auto cases = fixture("fixtures.json");
    for (auto const& tool : catalog.tools(true, true)) {
        auto name = tool.at("name").get<std::string>();
        nlohmann::json_schema::json_validator validator(tool.at("inputSchema"));
        for (auto const& entry : cases.at("actions")) {
            if (entry.at("action") != name || !entry.at("valid").get<bool>()) {
                continue;
            }
            INFO(entry.at("name"));
            auto args = entry.at("arguments");
            CHECK_THROWS(validator.validate(args));
            args["clientId"] = "b3e68f32-3b51-472d-8cab-14b597f7de91";
            CHECK_NOTHROW(validator.validate(args));
            CHECK_FALSE(catalog.acceptsArguments(name, args));
            args["clientId"] = 1;
            CHECK_THROWS(validator.validate(args));
        }
    }
}

TEST_CASE("MCP screenshot schemas describe metadata rather than image bytes", "[mcp-actions]")
{
    auto manifest = fixture("web-mcp-actions.json");
    Json metadata{
        {"type", "object"},
        {"properties", {{"width", {{"type", "integer"}, {"minimum", 1}}}}},
        {"required", {"width"}},
        {"additionalProperties", false}};
    manifest["actions"] = Json::array(
        {{{"name", "viewer_screenshot"},
          {"description", "Capture the visible application viewport."},
          {"permission", "viewer-read"},
          {"mutation", false},
          {"inputSchema", {{"type", "object"}, {"additionalProperties", false}}},
          {"outputSchema",
           {{"type", "object"},
            {"properties", {{"image", {{"type", "object"}}}, {"metadata", metadata}}},
            {"required", {"image", "metadata"}},
            {"additionalProperties", false}}}}});
    McpActionCatalog catalog(manifest);
    auto tools = catalog.tools(true, false);
    REQUIRE(tools.size() == 1);
    CHECK(tools[0]["outputSchema"] == metadata);
    Json result{{"width", 640}};
    nlohmann::json_schema::json_validator output(tools[0]["outputSchema"]);
    CHECK_NOTHROW(output.validate(result));
    CHECK_FALSE(catalog.acceptsResult("viewer_screenshot", result));
    CHECK(catalog.acceptsResult(
        "viewer_screenshot",
        {{"image", Json::object()}, {"metadata", result}}));
    // Browser validation stays strict even though its image is projected out of structuredContent.
    CHECK_FALSE(catalog.acceptsResult(
        "viewer_screenshot",
        {{"image", Json::object()}, {"metadata", {{"width", "640"}}}}));
    manifest["actions"][0]["outputSchema"]["properties"]["metadata"] = {{"type", "string"}};
    CHECK_THROWS(McpActionCatalog(manifest));
}

TEST_CASE("MCP catalogs fail closed before publishing invalid metadata", "[mcp-actions]")
{
    auto manifest = fixture("web-mcp-actions.json");
    SECTION("Unknown manifest version")
    {
        manifest["formatVersion"] = 2;
    }
    SECTION("Digest is not an arbitrary browser label")
    {
        manifest["catalogId"] = "latest";
    }
    SECTION("No executable field")
    {
        manifest["actions"][0]["code"] = "alert(1)";
    }
    SECTION("Duplicate names")
    {
        manifest["actions"].push_back(manifest["actions"][0]);
    }
    SECTION("Native tool collision")
    {
        manifest["actions"][0]["name"] = "viewer_list_sessions";
    }
    SECTION("Native namespace collision")
    {
        manifest["actions"][0]["name"] = "mapget_list_sources";
    }
    SECTION("Read permission cannot mutate")
    {
        manifest["actions"][0]["mutation"] = true;
    }
    SECTION("Unknown permission")
    {
        manifest["actions"][0]["permission"] = "admin";
    }
    SECTION("Application cannot take routing field")
    {
        manifest["actions"][0]["inputSchema"]["properties"]["clientId"] = {{"type", "string"}};
    }
    SECTION("Arguments need a closed root")
    {
        manifest["actions"][0]["inputSchema"]["additionalProperties"] = true;
    }
    SECTION("Root constraints cannot reject the inserted routing field")
    {
        manifest["actions"][0]["inputSchema"]["maxProperties"] = 1;
    }
    SECTION("Object result is mandatory")
    {
        manifest["actions"][0]["outputSchema"]["type"] = "array";
    }
    SECTION("Duplicate channels")
    {
        manifest["channels"].push_back(manifest["channels"][0]);
    }
    SECTION("Remote references are never fetched")
    {
        manifest["actions"][0]["outputSchema"]["properties"]["remote"] = {
            {"$ref", "https://untrusted.invalid/schema"}};
    }
    SECTION("New dialect must not be silently weakened")
    {
        manifest["actions"][0]["outputSchema"]["$schema"] =
            "https://json-schema.org/draft/2020-12/schema";
    }
    SECTION("Unsupported validation keyword")
    {
        manifest["actions"][0]["outputSchema"]["unevaluatedProperties"] = false;
    }
    SECTION("Unsupported validation within a channel")
    {
        manifest["channels"][0]["selectorSchema"]["unevaluatedProperties"] = false;
    }
    SECTION("Invalid schema must not compile")
    {
        manifest["actions"][0]["inputSchema"]["properties"]["bad"] = {{"minLength", -1}};
    }
    SECTION("Recursive references are bounded")
    {
        manifest["actions"][0]["outputSchema"]["properties"]["recursive"] = {{"$ref", "#"}};
    }
    SECTION("Dangling reference")
    {
        manifest["actions"][0]["outputSchema"]["properties"]["bad"] = {
            {"$ref", "#/definitions/missing"}};
    }
    CHECK_THROWS(McpActionCatalog(std::move(manifest)));
}

TEST_CASE(
    "MCP application conditionals preserve routing and cross-field validation",
    "[mcp-actions]")
{
    auto manifest = fixture("web-mcp-actions.json");
    auto name = manifest["actions"][0]["name"].get<std::string>();
    auto& input = manifest["actions"][0]["inputSchema"];
    input = Json::parse(R"({"type":"object","additionalProperties":false,
      "required":["mode","value"],"properties":{"mode":{"enum":["count","text"]},"value":{}},
      "allOf":[{"if":{"properties":{"mode":{"const":"count"}},"required":["mode"]},
                "then":{"properties":{"value":{"type":"integer"}}},
                "else":{"properties":{"value":{"type":"string"}}}}]})");
    McpActionCatalog catalog(manifest);
    auto tools = catalog.tools(true, true);
    for (auto const& tool : tools) {
        if (tool["name"] != name)
            continue;
        nlohmann::json_schema::json_validator validator(tool.at("inputSchema"));
        for (auto value : {Json(7), Json("wrong")}) {
            auto arguments = Json{{"mode", "count"}, {"value", value}};
            CHECK(catalog.acceptsArguments(name, arguments) == value.is_number_integer());
            arguments["clientId"] = "b3e68f32-3b51-472d-8cab-14b597f7de91";
            nlohmann::json_schema::basic_error_handler errors;
            validator.validate(arguments, errors);
            CHECK(!errors == value.is_number_integer());
        }
    }
    for (auto restriction :
         {Json{{"additionalProperties", false}},
          Json{{"maxProperties", 2}},
          Json{{"properties", {{"clientId", {{"type", "integer"}}}}}}})
    {
        input["allOf"][0]["then"] = restriction;
        CHECK_THROWS(McpActionCatalog(manifest));
    }
}

TEST_CASE(
    "MCP schema validation supports local definitions without changing defaults",
    "[mcp-actions]")
{
    auto manifest = fixture("web-mcp-actions.json");
    manifest["actions"][0]["inputSchema"] = {
        {"type", "object"},
        {"additionalProperties", false},
        {"definitions",
         {{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}, {"default", 3}}}}},
        {"properties", {{"limit", {{"$ref", "#/definitions/limit"}}}}}};
    McpActionCatalog catalog(std::move(manifest));
    auto args = Json::object();
    CHECK(catalog.acceptsArguments("viewer_describe_app_state", args));
    CHECK(args.empty());
    CHECK(catalog.acceptsArguments("viewer_describe_app_state", {{"limit", 5}}));
    CHECK_FALSE(catalog.acceptsArguments("viewer_describe_app_state", {{"limit", 11}}));
    CHECK_FALSE(catalog.acceptsArguments("viewer_describe_app_state", {{"limit", "5"}}));
}
