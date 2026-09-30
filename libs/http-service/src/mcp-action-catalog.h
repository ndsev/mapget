#pragma once

#include <nlohmann/json-schema.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace mapget::detail
{

/**
 * Immutable, deployment-trusted browser action metadata and compiled validators.
 * Browser registrations may select names from this catalog, never redefine them.
 */
class McpActionCatalog
{
public:
    /** Load a bounded local build artifact; no browser-selected URLs or schema fetches. */
    static McpActionCatalog load(std::filesystem::path const& path);

    /** Validate the entire artifact before publishing any of its actions. */
    explicit McpActionCatalog(nlohmann::json manifest);

    /** Return the build-exported opaque identity, not a separately canonicalized C++ hash. */
    [[nodiscard]] std::string const& id() const;

    /** Test whether an application action belongs to this trusted catalog. */
    [[nodiscard]] bool contains(std::string_view action) const;

    /** Return whether a known action needs the independent viewer-control permission. */
    [[nodiscard]] bool requiresControl(std::string_view action) const;

    /** Return whether a known action occupies the tab's single mutation slot. */
    [[nodiscard]] bool isMutation(std::string_view action) const;

    /** Emit MCP tool descriptions with the mandatory UUID routing argument added. */
    [[nodiscard]] nlohmann::json tools(bool allowRead, bool allowControl) const;

    /** Validate application arguments after removing the server-owned clientId. */
    [[nodiscard]] bool acceptsArguments(std::string_view action, nlohmann::json const& value) const;

    /** Validate a browser result without applying defaults or exposing invalid payloads. */
    [[nodiscard]] bool acceptsResult(std::string_view action, nlohmann::json const& value) const;

private:
    /** One immutable action owns its metadata and validators; no per-tab schema copies. */
    struct Action
    {
        nlohmann::json definition;
        nlohmann::json_schema::json_validator arguments;
        nlohmann::json_schema::json_validator result;
    };

    std::string id_;
    std::map<std::string, Action, std::less<>> actions_;

    /** Reject unsupported dialects/keywords and nonlocal or recursive references. */
    static void checkSchema(
        nlohmann::json const& schema,
        nlohmann::json const& root,
        size_t depth,
        size_t& remainingNodes);

    /** Compile only a bounded, self-contained Draft-07 schema. */
    static void
    compileSchema(nlohmann::json_schema::json_validator& validator, nlohmann::json const& schema);
};

}  // namespace mapget::detail
