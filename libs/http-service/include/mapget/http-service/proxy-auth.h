#pragma once

#include "mapget/service/authorization.h"

#include <drogon/HttpRequest.h>

#include <string>
#include <vector>

namespace CLI
{
class App;
}

namespace mapget
{

/** Explicit trust boundary for base64url JSON claims exported by an authenticating reverse proxy.
 * Independent of MCP permissions: an ordinary browser may access a source without MCP roles.
 */
struct ProxyAuthentication
{
    std::vector<std::string> trustedAddresses;
    std::string claimsHeader;

    /** Register restart-scoped CLI/YAML options; both settings are required together. */
    void addOptions(CLI::App& serve);

    /** Reject incomplete trust, wildcard/CIDR peers and invalid header names at startup. */
    void validate() const;

    /** Read bounded claims only from the actual trusted socket peer; invalid claims fail closed. */
    [[nodiscard]] std::shared_ptr<nlohmann::json const> claims(drogon::HttpRequestPtr const& request) const;

    /** Keep ordinary header gates and attach only independently trusted claims. */
    [[nodiscard]] ClientAuthorization authorization(drogon::HttpRequestPtr const& request) const;
};

}  // namespace mapget
