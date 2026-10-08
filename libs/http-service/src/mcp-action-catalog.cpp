#include "mcp-action-catalog.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>

namespace mapget::detail
{

/** Per-validation bounded diagnostics, derived only from trusted schema names and constraints. */
class McpActionCatalog::ArgumentErrors final : public nlohmann::json_schema::error_handler
{
public:
    /** Collect only trusted property names for redacting instance-derived JSON pointers. */
    explicit ArgumentErrors(nlohmann::json const& schema) : schema_(schema)
    {
        std::vector<nlohmann::json const*> pending{&schema};
        while (!pending.empty()) {
            auto const* node = pending.back();
            pending.pop_back();
            if (node->is_object() && node->contains("properties"))
                for (auto const& [key, _] : node->at("properties").items())
                    names_.insert(key);
            if (node->is_structured())
                for (auto const& child : *node)
                    if (child.is_structured())
                        pending.push_back(&child);
        }
    }

    /** Explain rejected root combinations using schema-owned names, never caller values. */
    void explainConstraints(nlohmann::json const& instance)
    {
        if (!instance.is_object())
            return;
        std::vector<nlohmann::json const*> pending{&schema_};
        while (!pending.empty() && issues.size() < 8) {
            auto const& branch = *pending.back();
            pending.pop_back();
            if (branch.contains("allOf"))
                for (auto const& child : branch["allOf"])
                    pending.push_back(&child);
            if (branch.contains("not")) {
                auto fields = presentFields(branch["not"], instance);
                if (!fields.empty())
                    append(
                        {{"path", ""},
                         {"message",
                          "These fields cannot be supplied together; remove at least one."},
                         {"conflictingFields", fields}});
            }
            if (!branch.contains("if") || !branch.contains("then"))
                continue;
            auto fields = presentFields(branch["if"], instance);
            if (fields.empty())
                continue;
            auto const& consequent = branch["then"];
            if (consequent.contains("required")) {
                for (auto const& field : consequent["required"]) {
                    auto const name = field.get<std::string>();
                    if (names_.contains(name) && !instance.contains(name))
                        append(
                            {{"path", ""},
                             {"message", "The supplied selector requires this field."},
                             {"triggerFields", fields},
                             {"missingField", name}});
                }
            }
            if (consequent.contains("properties")) {
                for (auto const& [name, expected] : consequent["properties"].items()) {
                    if (!names_.contains(name) || !expected.contains("const") ||
                        expected["const"].dump().size() > 1024 ||
                        (instance.contains(name) && instance[name] == expected["const"]))
                        continue;
                    // An optional absent property does not violate a properties-only constraint.
                    auto const required = consequent.value("required", nlohmann::json::array());
                    if (!instance.contains(name) &&
                        std::find(required.begin(), required.end(), name) == required.end())
                        continue;
                    append(
                        {{"path", "/" + name},
                         {"message", "The supplied selector requires this value."},
                         {"triggerFields", fields},
                         {"expected", {{"const", expected["const"]}}}});
                }
            }
        }
    }

    /** Retain bounded repair hints without copying caller values into diagnostics. */
    void error(
        nlohmann::json::json_pointer const& location,
        nlohmann::json const&,
        std::string const& message) override
    {
        invalid = true;
        if (issues.size() >= 8)
            return;
        std::vector<std::string> parts;
        for (auto cursor = location; !cursor.empty(); cursor.pop_back()) {
            auto token = cursor.back();
            auto index = !token.empty() && token.size() <= 10 &&
                token.find_first_not_of("0123456789") == std::string::npos;
            parts.push_back(names_.contains(token) || index ? token : "*");
        }
        nlohmann::json::json_pointer safe;
        for (auto it = parts.rbegin(); it != parts.rend(); ++it)
            safe /= *it;
        auto text = message;
        auto const prefix = std::string{
            "at least one subschema has failed, but all of them are required to validate - "};
        while (text.starts_with(prefix))
            text.erase(0, prefix.size());
        // Format/content checker exceptions and additional-property errors may contain input.
        if (text.starts_with("validation failed for additional property"))
            text = "Unknown argument field; use only fields in this tool's input schema.";
        else if (!(text.starts_with("required property '") || text == "unexpected instance type" ||
                   text.starts_with("instance exceeds") || text.starts_with("instance is below") ||
                   text == "instance not found in required enum" || text == "instance not const" ||
                   text == "array has too many items" || text == "array has too few items" ||
                   text == "too many properties" || text == "too few properties"))
            text = "Value does not match an allowed input schema branch or constraint.";
        nlohmann::json issue{{"path", safe.to_string()}, {"message", text.substr(0, 512)}};
        if (location.empty() && schema_.contains("required")) {
            issue["required"] = schema_["required"];
            for (auto const& field : schema_["required"]) {
                auto const name = field.get<std::string>();
                if (!text.starts_with("required property '" + name + "' ") ||
                    !schema_.contains("properties") || !schema_["properties"].contains(name))
                    continue;
                issue["missingField"] = name;
                auto const& expected = schema_["properties"][name];
                for (auto key : {"type", "enum", "const", "description"})
                    if (expected.contains(key) && expected[key].dump().size() <= 1024)
                        issue["expected"][key] = expected[key];
            }
        }
        if (location.empty()) {
            if (text.starts_with("Unknown argument field") && schema_.contains("properties")) {
                issue["allowedFields"] = nlohmann::json::array();
                for (auto const& [name, _] : schema_["properties"].items()) {
                    if (issue["allowedFields"].size() >= 32)
                        break;
                    issue["allowedFields"].push_back(name);
                }
            }
            for (auto combination : {"anyOf", "oneOf"}) {
                if (!schema_.contains(combination))
                    continue;
                for (auto const& branch : schema_[combination])
                    if (branch.contains("required") && issue["acceptedShapes"].size() < 8)
                        issue["acceptedShapes"].push_back({{"required", branch["required"]}});
            }
        }
        // Follow only trusted, unambiguous schema properties/items. Nested errors
        // such as featureIds[0] must explain the expected string, not merely its path.
        auto const* expected = &schema_;
        for (auto it = parts.rbegin(); it != parts.rend() && expected; ++it) {
            if (expected->contains("properties") && expected->at("properties").contains(*it))
                expected = &expected->at("properties").at(*it);
            else if (
                expected->contains("items") && expected->at("items").is_object() && !it->empty() &&
                it->find_first_not_of("0123456789") == std::string::npos)
                expected = &expected->at("items");
            else
                expected = nullptr;
        }
        if (!parts.empty() && expected) {
            for (auto key :
                 {"type",
                  "enum",
                  "const",
                  "minimum",
                  "maximum",
                  "minItems",
                  "maxItems",
                  "minLength",
                  "maxLength",
                  "description"})
                if (expected->contains(key) && expected->at(key).dump().size() <= 1024)
                    issue["expected"][key] = expected->at(key);
        }
        append(std::move(issue));
    }

    bool invalid = false;
    nlohmann::json issues = nlohmann::json::array();

private:
    /** Recognize only pure presence conditions; other schema logic keeps generic diagnostics. */
    nlohmann::json
    presentFields(nlohmann::json const& condition, nlohmann::json const& instance) const
    {
        auto fields = nlohmann::json::array();
        if (!condition.is_object() || condition.size() != 1)
            return fields;
        if (condition.contains("anyOf")) {
            for (auto const& branch : condition["anyOf"]) {
                fields = presentFields(branch, instance);
                if (!fields.empty())
                    return fields;
            }
        }
        else if (condition.contains("required")) {
            for (auto const& field : condition["required"]) {
                auto const name = field.get<std::string>();
                if (!names_.contains(name) || !instance.contains(name))
                    return nlohmann::json::array();
                fields.push_back(name);
            }
        }
        return fields;
    }

    /** Preserve the shared issue count, byte bound and deduplication policy. */
    void append(nlohmann::json issue)
    {
        if (issues.size() < 8 && issue.dump().size() <= 4096 &&
            std::find(issues.begin(), issues.end(), issue) == issues.end())
            issues.push_back(std::move(issue));
    }

    nlohmann::json const& schema_;
    std::set<std::string> names_;
};

bool McpActionCatalog::validateArguments(
    nlohmann::json_schema::json_validator const& validator,
    nlohmann::json const& schema,
    nlohmann::json const& value,
    nlohmann::json* issues)
{
    nlohmann::json_schema::basic_error_handler validation;
    validator.validate(value, validation);
    if (issues)
        *issues = nlohmann::json::array();
    if (!validation || !issues)
        return !validation;
    // Build human-readable guidance only for rejected calls, keeping the common path cheap.
    ArgumentErrors errors(schema);
    errors.explainConstraints(value);
    validator.validate(value, errors);
    *issues = std::move(errors.issues);
    return !errors.invalid;
}

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
        // Routing is injected at the root. Only conditionals which cannot observe clientId
        // may constrain the same object as the application arguments.
        if (input.value("type", nlohmann::json{}) != "object" ||
            input.value("additionalProperties", nlohmann::json{}) != false ||
            output.value("type", nlohmann::json{}) != "object")
        {
            throw std::invalid_argument(
                "MCP actions need object results and closed object arguments.");
        }
        for (auto const* keyword :
             {"$ref",
              "anyOf",
              "oneOf",
              "not",
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
        for (auto const* keyword : {"allOf", "if", "then", "else"}) {
            if (!input.contains(keyword))
                continue;
            if (std::string_view(keyword) == "allOf") {
                for (auto const& branch : input.at(keyword))
                    checkRoutingBranch(branch, input.at("properties"));
            }
            else
                checkRoutingBranch(input.at(keyword), input.at("properties"));
        }
        auto [entry, inserted] = actions_.try_emplace(name);
        if (!inserted) {
            throw std::invalid_argument("Duplicate MCP action name.");
        }
        compileSchema(entry->second.arguments, input);
        compileSchema(entry->second.result, output);
        if (name == "viewer_screenshot") {
            // The browser returns {image, metadata}; MCP publishes the image as ImageContent.
            // Its structured output schema must describe metadata alone and be self-contained.
            auto const& metadata = output.at("properties").at("metadata");
            if (metadata.value("type", nlohmann::json{}) != "object")
                throw std::invalid_argument("MCP screenshot metadata must have an object schema.");
            nlohmann::json_schema::json_validator projected;
            compileSchema(projected, metadata);
        }
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

void McpActionCatalog::checkRoutingBranch(
    nlohmann::json const& branch,
    nlohmann::json const& fields,
    size_t depth)
{
    if (depth > 64)
        throw std::invalid_argument("MCP routing conditionals exceed nesting limit.");
    if (branch.is_boolean())
        return;
    if (!branch.is_object())
        throw std::invalid_argument("Invalid MCP routing conditional.");
    for (auto const& [keyword, value] : branch.items()) {
        if (keyword == "properties") {
            for (auto const& [field, _] : value.items())
                if (field == "clientId" || !fields.contains(field))
                    throw std::invalid_argument(
                        "MCP conditionals may inspect only declared application fields.");
        }
        else if (keyword == "required") {
            for (auto const& field : value)
                if (!field.is_string() || field == "clientId" ||
                    !fields.contains(field.get<std::string>()))
                    throw std::invalid_argument(
                        "MCP conditionals may require only declared application fields.");
        }
        else if (keyword == "allOf") {
            for (auto const& child : value)
                checkRoutingBranch(child, fields, depth + 1);
        }
        else if (keyword == "if" || keyword == "then" || keyword == "else") {
            checkRoutingBranch(value, fields, depth + 1);
        }
        else if (
            keyword != "type" && keyword != "description" && keyword != "title" &&
            keyword != "$comment")
        {
            // A root closure, reference, field-count or object const can treat routing as data.
            throw std::invalid_argument(
                "MCP conditional is not invariant under UUID routing injection.");
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
             {"outputSchema",
              name == "viewer_screenshot" ?
                  action.definition.at("outputSchema").at("properties").at("metadata") :
                  action.definition.at("outputSchema")},
             {"annotations", {{"readOnlyHint", !isMutation(name)}}}});
    }
    return result;
}

bool McpActionCatalog::acceptsArguments(
    std::string_view action,
    nlohmann::json const& value,
    nlohmann::json* issues) const
{
    auto found = actions_.find(action);
    if (found == actions_.end()) {
        return false;
    }
    return validateArguments(
        found->second.arguments,
        found->second.definition.at("inputSchema"),
        value,
        issues);
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
