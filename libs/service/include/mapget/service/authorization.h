#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>

namespace mapget
{

/** Request headers available to the existing datasource header gates. */
using AuthHeaders = std::unordered_map<std::string, std::string>;

/** Every JSON-pointer/string requirement must match the verified identity. */
using AuthClaimRequirements = std::map<nlohmann::json::json_pointer, std::string>;

/** Transport-independent authority carried through source discovery and data requests.
 * Claims must originate from verified authentication, never an arbitrary request body/header.
 * Immutable shared ownership avoids copying the token for every descendant tile request.
 */
struct ClientAuthorization
{
    AuthHeaders headers;
    std::shared_ptr<nlohmann::json const> claims;

    /** Header-only callers remain anonymous with respect to all claim requirements. */
    ClientAuthorization(AuthHeaders headers = {}, std::shared_ptr<nlohmann::json const> claims = {});

    /** Exact string or array-member matching; missing/non-string claims never grant access. */
    [[nodiscard]] bool hasClaim(nlohmann::json::json_pointer const& pointer, std::string const& expected) const;

    /** AND all configured requirements; an empty map imposes no claim restriction. */
    [[nodiscard]] bool matches(AuthClaimRequirements const& required) const;

    /** Compare authority by value, not the identity of the shared claims allocation. */
    [[nodiscard]] bool operator==(ClientAuthorization const& other) const;
};

}  // namespace mapget
