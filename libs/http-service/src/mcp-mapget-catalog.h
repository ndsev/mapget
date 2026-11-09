#pragma once

#include "mcp-mapget-tools.h"

namespace mapget::detail
{

/** Immutable mapget action contracts, independent of browser catalog metadata and call state. */
class McpMapgetTools::Catalog
{
public:
    /** Build and compile each authoritative tool schema once at server startup. */
    Catalog();

    /** Project compatible tool descriptions for the caller's deployment/permission predicate. */
    nlohmann::json tools(std::function<bool(std::string_view)> const& permitted) const;

    /** Recognize all server tools, including ones hidden by caller permissions. */
    bool contains(std::string_view name) const;

    /** Validate arguments and optionally retain actionable validation failures. */
    bool
    acceptsArguments(std::string_view name, nlohmann::json const& value, nlohmann::json* issues)
        const;

    /** Validate the final result against the same contract used for discovery. */
    bool acceptsResult(std::string_view name, nlohmann::json const& value) const;

private:
    /** Schema validators and their public descriptions have one immutable owner. */
    struct Action
    {
        nlohmann::json tool;
        nlohmann::json_schema::json_validator input;
        nlohmann::json_schema::json_validator output;
    };
    std::map<std::string, Action, std::less<>> actions_;
};

}  // namespace mapget::detail
