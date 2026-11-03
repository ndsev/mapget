#include "mapget/http-service/proxy-auth.h"
#include "bounded-json.h"

#include <CLI/CLI.hpp>
#include <trantor/net/InetAddress.h>

#include <algorithm>
#include <regex>
#include <stdexcept>

namespace mapget
{

void ProxyAuthentication::addOptions(CLI::App& serve)
{
    serve.add_option("--auth-trusted-proxy-addresses", trustedAddresses,
        "Exact socket peer IPs allowed to assert datasource identity claims.");
    serve.add_option("--auth-claims-header", claimsHeader,
        "Lowercase header carrying a JSON claims object from the trusted authenticating proxy.");
}

void ProxyAuthentication::validate() const
{
    if (trustedAddresses.empty() != claimsHeader.empty())
        throw std::invalid_argument("Datasource proxy authentication requires both trusted addresses and a claims header.");
    if (trustedAddresses.empty())
        return;
    if (trustedAddresses.size() > 32)
        throw std::invalid_argument("At most 32 trusted proxy addresses are allowed.");
    for (auto const& address : trustedAddresses) {
        auto parsed = trantor::InetAddress(address, 0, address.find(':') != std::string::npos);
        if (address.empty() || address.find_first_not_of("0123456789abcdefABCDEF:.") != std::string::npos ||
            parsed.isUnspecified() || parsed.toIp() != address || address == "0.0.0.0" || address == "::")
            throw std::invalid_argument("Trusted proxy addresses must be canonical concrete IP addresses, not forwarded names or networks.");
    }
    if (!std::regex_match(claimsHeader, std::regex("^[a-z][a-z0-9-]{0,99}$")) ||
        claimsHeader == "authorization" || claimsHeader == "cookie" || claimsHeader == "host")
        throw std::invalid_argument("Use a dedicated lowercase header name for verified datasource claims.");
}

std::shared_ptr<nlohmann::json const> ProxyAuthentication::claims(drogon::HttpRequestPtr const& request) const
{
    if (claimsHeader.empty() ||
        std::find(trustedAddresses.begin(), trustedAddresses.end(), request->peerAddr().toIp()) == trustedAddresses.end())
        return {};
    auto const& value = request->getHeader(claimsHeader);
    if (value.empty())
        return {};
    try {
        auto claims = detail::parseBoundedJson(value, 16 * 1024, 16);
        if (!claims.is_object())
            return {};
        return std::make_shared<nlohmann::json const>(std::move(claims));
    }
    catch (std::exception const&) {
        // Do not log the header or parser exception: either can contain private identity data.
        return {};
    }
}

ClientAuthorization ProxyAuthentication::authorization(drogon::HttpRequestPtr const& request) const
{
    ClientAuthorization result;
    for (auto const& [name, value] : request->headers())
        result.headers.emplace(name, value);
    result.claims = claims(request);
    return result;
}

}  // namespace mapget
