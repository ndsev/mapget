#include "mcp-action-catalog.h"

#include <fstream>
#include <set>
#include <stdexcept>

namespace mapget::detail
{

McpActionCatalog McpActionCatalog::load(std::filesystem::path const& path)
{
    constexpr size_t maxBytes = 8 * 1024 * 1024;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::invalid_argument("Cannot open MCP action catalog.");
    }
    // Read at most the limit plus one byte, even if the file grows during startup.
    std::string bytes(maxBytes + 1, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(file.gcount()));
    if (file.bad() || bytes.size() > maxBytes) {
        throw std::invalid_argument("Cannot read MCP action catalog within the 8 MiB limit.");
    }
    auto manifest = nlohmann::json::parse(
        bytes,
        [](int depth, auto, auto&)
        {
            if (depth > 128) {
                throw std::invalid_argument("MCP action catalog exceeds the nesting limit.");
            }
            return true;
        });
    return McpActionCatalog(std::move(manifest));
}

McpActionCatalog::McpActionCatalog(nlohmann::json manifest)
{
    static auto const manifestSchema = nlohmann::json::parse(R"json({
      "type":"object", "additionalProperties":false,
      "required":["formatVersion","catalogId","actions","channels"],
      "properties":{
        "formatVersion":{"const":1},
        "catalogId":{"type":"string","pattern":"^sha256:[0-9a-f]{64}$"},
        "actions":{"type":"array","maxItems":64,"items":{
          "type":"object","additionalProperties":false,
          "required":["name","description","permission","mutation","inputSchema","outputSchema"],
          "properties":{
            "name":{"type":"string","pattern":"^viewer_[a-z][a-z0-9_]*$","maxLength":128},
            "description":{"type":"string","minLength":1,"maxLength":8192},
            "permission":{"enum":["viewer-read","viewer-control"]},
            "mutation":{"type":"boolean"},
            "inputSchema":{"type":"object"},"outputSchema":{"type":"object"}
          }
        }},
        "channels":{"type":"array","maxItems":64,"items":{
          "type":"object","additionalProperties":false,
          "required":["name","description","selectorSchema","valueSchema","readable","writable","persistence","synchronization"],
          "properties":{
            "name":{"type":"string","minLength":1,"maxLength":128},
            "description":{"type":"string","maxLength":8192},
            "selectorSchema":{"type":["object","boolean"]},
            "valueSchema":{"type":["object","boolean"]},
            "readable":{"type":"boolean"},"writable":{"type":"boolean"},
            "persistence":{"type":"string","maxLength":128},
            "synchronization":{"type":"string","maxLength":128}
          }
        }}
      }
    })json");
    nlohmann::json_schema::json_validator envelope(manifestSchema);
    envelope.validate(manifest);
    id_ = manifest.at("catalogId").get<std::string>();

    for (auto& definition : manifest.at("actions")) {
        auto name = definition.at("name").get<std::string>();
        if (name == "viewer_list_sessions") {
            throw std::invalid_argument("The MCP session-listing tool is server-owned.");
        }
        if (definition.at("mutation").get<bool>() &&
            definition.at("permission") != "viewer-control") {
            throw std::invalid_argument("Mutating MCP actions require viewer-control permission.");
        }
        auto const& input = definition.at("inputSchema");
        auto const& output = definition.at("outputSchema");
        // Routing is injected at the root; root combinators could accidentally reject
        // clientId in the advertised schema even though native argument validation succeeds.
        if (input.value("type", nlohmann::json{}) != "object" ||
            input.value("additionalProperties", nlohmann::json{}) != false ||
            output.value("type", nlohmann::json{}) != "object")
        {
            throw std::invalid_argument(
                "MCP actions need object results and closed object arguments.");
        }
        for (auto const* keyword :
             {"$ref",
              "allOf",
              "anyOf",
              "oneOf",
              "not",
              "if",
              "then",
              "else",
              "dependencies",
              "patternProperties",
              "propertyNames",
              "maxProperties",
              "minProperties",
              "enum",
              "const"})
        {
            if (input.contains(keyword)) {
                throw std::invalid_argument(
                    "MCP action argument root must allow UUID routing injection.");
            }
        }
        if (input.value("properties", nlohmann::json::object()).contains("clientId")) {
            throw std::invalid_argument(
                "clientId is an MCP routing argument, not an application field.");
        }
        for (auto const& required : input.value("required", nlohmann::json::array())) {
            if (required == "clientId") {
                throw std::invalid_argument(
                    "Application schemas cannot require the routing clientId.");
            }
        }
        auto [entry, inserted] = actions_.try_emplace(name);
        if (!inserted) {
            throw std::invalid_argument("Duplicate MCP action name.");
        }
        compileSchema(entry->second.arguments, input);
        compileSchema(entry->second.result, output);
        entry->second.definition = std::move(definition);
    }

    std::set<std::string> channels;
    for (auto const& channel : manifest.at("channels")) {
        if (!channels.insert(channel.at("name").get<std::string>()).second) {
            throw std::invalid_argument("Duplicate MCP state-channel name.");
        }
        // Discovery schemas are part of the trusted contract too, even though the
        // browser implements channel discovery and its actual getter/setter behavior.
        for (auto const* field : {"selectorSchema", "valueSchema"}) {
            nlohmann::json_schema::json_validator validator;
            compileSchema(validator, channel.at(field));
        }
    }
}

void McpActionCatalog::checkSchema(
    nlohmann::json const& schema,
    nlohmann::json const& root,
    size_t depth,
    size_t& remainingNodes)
{
    if (depth > 64 || remainingNodes == 0) {
        throw std::invalid_argument("MCP schemas must be bounded and nonrecursive.");
    }
    --remainingNodes;
    if (schema.is_boolean()) {
        return;
    }
    if (!schema.is_object()) {
        throw std::invalid_argument("An MCP schema must be an object or boolean.");
    }
    // Bound traversal before invoking the recursive meta-schema validator. Unknown
    // keywords must not silently weaken newer-dialect validation in a Draft-07 engine.
    static std::set<std::string_view> const keywords{
        "$schema",
        "$ref",
        "$comment",
        "title",
        "description",
        "default",
        "examples",
        "readOnly",
        "writeOnly",
        "type",
        "enum",
        "const",
        "multipleOf",
        "maximum",
        "exclusiveMaximum",
        "minimum",
        "exclusiveMinimum",
        "maxLength",
        "minLength",
        "pattern",
        "maxItems",
        "minItems",
        "uniqueItems",
        "maxProperties",
        "minProperties",
        "required",
        "properties",
        "patternProperties",
        "definitions",
        "dependencies",
        "items",
        "additionalItems",
        "additionalProperties",
        "contains",
        "propertyNames",
        "allOf",
        "anyOf",
        "oneOf",
        "not",
        "if",
        "then",
        "else"};
    for (auto const& [keyword, value] : schema.items()) {
        if (!keywords.contains(keyword)) {
            throw std::invalid_argument("Unsupported MCP schema keyword: " + keyword);
        }
        if (keyword == "$schema" && value != "http://json-schema.org/draft-07/schema#") {
            throw std::invalid_argument("Only Draft-07 MCP action schemas are supported.");
        }
        if (keyword == "$ref") {
            auto ref = value.get<std::string>();
            if (ref != "#" && !ref.starts_with("#/")) {
                throw std::invalid_argument(
                    "MCP action schemas may only reference local JSON pointers.");
            }
            checkSchema(
                root.at(nlohmann::json::json_pointer(ref.substr(1))),
                root,
                depth + 1,
                remainingNodes);
        }
        else if (
            keyword == "properties" || keyword == "patternProperties" || keyword == "definitions" ||
            keyword == "dependencies")
        {
            for (auto const& child : value) {
                // Draft-07 property dependencies are arrays of names, not schemas.
                if (keyword != "dependencies" || !child.is_array()) {
                    checkSchema(child, root, depth + 1, remainingNodes);
                }
            }
        }
        else if (
            keyword == "allOf" || keyword == "anyOf" || keyword == "oneOf" ||
            (keyword == "items" && value.is_array()))
        {
            for (auto const& child : value) {
                checkSchema(child, root, depth + 1, remainingNodes);
            }
        }
        else if (
            keyword == "items" || keyword == "additionalItems" ||
            keyword == "additionalProperties" || keyword == "contains" ||
            keyword == "propertyNames" || keyword == "not" || keyword == "if" ||
            keyword == "then" || keyword == "else")
        {
            checkSchema(value, root, depth + 1, remainingNodes);
        }
    }
}

void McpActionCatalog::compileSchema(
    nlohmann::json_schema::json_validator& validator,
    nlohmann::json const& schema)
{
    nlohmann::json_schema::json_validator meta(
        nlohmann::json_schema::draft7_schema_builtin,
        nullptr,
        [](std::string const& format, std::string const& value)
        {
            // The validator does not implement uri-reference. Our contract only
            // permits local JSON pointers, checked/resolved again in checkSchema.
            if (format == "uri-reference" && (value == "#" || value.starts_with("#/"))) {
                return;
            }
            nlohmann::json_schema::default_string_format_check(format, value);
        });
    size_t remainingNodes = 100000;
    checkSchema(schema, schema, 0, remainingNodes);
    meta.validate(schema);
    validator.set_root_schema(schema);
}

std::string const& McpActionCatalog::id() const
{
    return id_;
}

bool McpActionCatalog::contains(std::string_view action) const
{
    return actions_.contains(action);
}

bool McpActionCatalog::requiresControl(std::string_view action) const
{
    return actions_.at(std::string(action)).definition.at("permission") == "viewer-control";
}

bool McpActionCatalog::isMutation(std::string_view action) const
{
    return actions_.at(std::string(action)).definition.at("mutation").get<bool>();
}

nlohmann::json McpActionCatalog::tools(bool allowRead, bool allowControl) const
{
    auto result = nlohmann::json::array();
    for (auto const& [name, action] : actions_) {
        if (requiresControl(name) ? !allowControl : !allowRead) {
            continue;
        }
        auto input = action.definition.at("inputSchema");
        input["properties"]["clientId"] = {
            {"type", "string"},
            {"description", "Exact live viewer session returned by viewer_list_sessions."},
            {"pattern", "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"}};
        if (!input.contains("required")) {
            input["required"] = nlohmann::json::array();
        }
        input["required"].push_back("clientId");
        result.push_back(
            {{"name", name},
             {"description", action.definition.at("description")},
             {"inputSchema", std::move(input)},
             {"outputSchema", action.definition.at("outputSchema")},
             {"annotations", {{"readOnlyHint", !isMutation(name)}}}});
    }
    return result;
}

bool McpActionCatalog::acceptsArguments(std::string_view action, nlohmann::json const& value) const
{
    auto found = actions_.find(action);
    if (found == actions_.end()) {
        return false;
    }
    nlohmann::json_schema::basic_error_handler errors;
    found->second.arguments.validate(value, errors);
    return !errors;
}

bool McpActionCatalog::acceptsResult(std::string_view action, nlohmann::json const& value) const
{
    auto found = actions_.find(action);
    if (found == actions_.end()) {
        return false;
    }
    nlohmann::json_schema::basic_error_handler errors;
    found->second.result.validate(value, errors);
    return !errors;
}

}  // namespace mapget::detail
