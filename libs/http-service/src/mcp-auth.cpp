#include "mcp-auth.h"

#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/nlohmann-json/traits.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mapget::detail
{
namespace
{
/** Match whitespace-delimited scope/permission names, not substrings or regular expressions. */
bool containsWord(std::string const& list, std::string const& expected)
{
    std::istringstream words(list);
    std::string word;
    while (words >> word) {
        if (word == expected) {
            return true;
        }
    }
    return false;
}

/** Only literal loopback addresses qualify; DNS and forwarded addresses are not evidence. */
bool loopback(std::string const& address)
{
    return address == "127.0.0.1" || address == "::1" || address == "::ffff:127.0.0.1";
}

/** Bound decoded JSON before jwt-cpp's recursive JSON parser or NumericDate conversions run. */
jwt::decoded_jwt<jwt::traits::nlohmann_json> decodeToken(std::string const& token)
{
    if (token.empty() || token.size() > 16 * 1024 ||
        std::count(token.begin(), token.end(), '.') != 2 ||
        token.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") !=
            std::string::npos)
    {
        throw std::invalid_argument("Invalid compact JWT encoding.");
    }
    auto first = token.find('.');
    auto second = token.find('.', first + 1);
    auto decode = [](std::string const& value)
    {
        return jwt::base::decode<jwt::alphabet::base64url>(
            jwt::base::pad<jwt::alphabet::base64url>(value));
    };
    (void)McpAuthentication::parseJson(decode(token.substr(0, first)), 4096, 8);
    auto claims = McpAuthentication::parseJson(
        decode(token.substr(first + 1, second - first - 1)),
        16 * 1024,
        16);
    for (auto const* field : {"exp", "nbf", "iat"}) {
        if (claims.contains(field) &&
            (!claims[field].is_number_integer() || claims[field].get<int64_t>() <= 0 ||
             claims[field].get<int64_t>() > 4102444800LL))
        {
            throw std::invalid_argument("Invalid JWT NumericDate.");
        }
    }
    return jwt::decode<jwt::traits::nlohmann_json>(token);
}
}  // namespace

nlohmann::json
McpAuthentication::parseJson(std::string_view input, size_t maxBytes, size_t maxDepth)
{
    if (input.size() > maxBytes) {
        throw std::invalid_argument("MCP JSON exceeds its byte limit.");
    }
    std::vector<std::set<std::string>> objectKeys;
    return nlohmann::json::parse(
        input,
        [maxDepth, &objectKeys](int depth, auto event, auto& value)
        {
            if (depth < 0 || static_cast<size_t>(depth) > maxDepth) {
                throw std::invalid_argument("MCP JSON exceeds its nesting limit.");
            }
            if (event == nlohmann::json::parse_event_t::object_start)
                objectKeys.emplace_back();
            if (event == nlohmann::json::parse_event_t::object_end)
                objectKeys.pop_back();
            if (event == nlohmann::json::parse_event_t::key &&
                !objectKeys.back().insert(value.template get<std::string>()).second)
            {
                throw std::invalid_argument("MCP JSON contains a duplicate object member.");
            }
            return true;
        });
}

nlohmann::json McpAuthentication::readJson(std::filesystem::path const& path, size_t maxBytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::invalid_argument("Cannot open MCP configuration artifact.");
    }
    std::string bytes(maxBytes + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(input.gcount()));
    return parseJson(bytes, maxBytes);
}

McpAuthentication::McpAuthentication(McpConfig config) : config_(std::move(config))
{
    config_.validate();
    if (!config_.jwksFile.empty()) {
        installKeys(readJson(config_.jwksFile, 256 * 1024));
        // A local key artifact is administrator-managed, immutable until service restart.
        keysExpireAt_ = std::chrono::steady_clock::time_point::max();
    }
}

nlohmann::json McpAuthentication::info(std::string const& catalogId) const
{
    nlohmann::json result{
        {"enabled", true},
        {"endpoint", config_.endpoint},
        {"authentication", config_.mode == McpConfig::Mode::Local ? "local" : "oauth"},
        {"catalogId", catalogId},
        {"scopes", nlohmann::json::array()}};
    if (config_.mode == McpConfig::Mode::OAuth) {
        result["scopes"] = config_.requiredScopes;
        if (!config_.oauthClientId.empty()) {
            result["oauthClientId"] = config_.oauthClientId;
        }
    }
    return result;
}

nlohmann::json McpAuthentication::metadata() const
{
    return {
        {"resource", config_.endpoint},
        {"authorization_servers", {config_.issuer}},
        {"scopes_supported", config_.requiredScopes},
        {"bearer_methods_supported", {"header"}}};
}

std::string McpAuthentication::challenge(bool insufficient) const
{
    auto endpoint = config_.endpoint;
    std::string scopes;
    for (auto const& scope : config_.requiredScopes) {
        if (!scopes.empty())
            scopes += ' ';
        scopes += scope;
    }
    return "Bearer resource_metadata=\"" + endpoint.substr(0, endpoint.size() - 4) +
        "/.well-known/oauth-protected-resource/mcp\", scope=\"" + scopes + "\", error=\"" +
        (insufficient ? "insufficient_scope" : "invalid_token") + "\"";
}

bool McpAuthentication::acceptsRequest(drogon::HttpRequestPtr const& request, bool browser) const
{
    auto const& hosts = config_.allowedHosts;
    auto const& origins = config_.allowedOrigins;
    auto const& origin = request->getHeader("origin");
    if (std::find(hosts.begin(), hosts.end(), request->getHeader("host")) == hosts.end() ||
        ((browser || !origin.empty()) &&
         std::find(origins.begin(), origins.end(), origin) == origins.end()))
    {
        return false;
    }
    if (config_.mode == McpConfig::Mode::Local) {
        if (!loopback(request->peerAddr().toIp()))
            return false;
        for (auto const& [name, value] : request->headers()) {
            if (name == "forwarded" || name.starts_with("x-forwarded-"))
                return false;
        }
    }
    return true;
}

McpViewerRelay::Principal McpAuthentication::localPrincipal() const
{
    if (config_.mode != McpConfig::Mode::Local)
        return {};
    return {
        "urn:mapget:local",
        "local-user",
        std::chrono::system_clock::now() + std::chrono::hours(24),
        true,
        true,
        true,
        true,
        true};
}

std::string McpAuthentication::bearerToken(std::string_view authorization)
{
    constexpr std::string_view scheme = "bearer";
    if (authorization.size() <= scheme.size() || authorization[scheme.size()] != ' ' ||
        !std::equal(
            scheme.begin(),
            scheme.end(),
            authorization.begin(),
            [](char expected, unsigned char actual) { return expected == std::tolower(actual); }))
    {
        return {};
    }
    // HTTP authentication allows one or more spaces between the scheme and credentials.
    auto begin = authorization.find_first_not_of(' ', scheme.size());
    return begin == std::string_view::npos ?
        std::string{} :
        std::string(authorization.substr(begin));
}

McpViewerRelay::Principal McpAuthentication::browser(drogon::HttpRequestPtr const& request) const
{
    if (!acceptsRequest(request, true))
        return {};
    if (config_.mode == McpConfig::Mode::Local)
        return localPrincipal();
    auto const& proxies = config_.trustedProxyAddresses;
    if (std::find(proxies.begin(), proxies.end(), request->peerAddr().toIp()) == proxies.end())
        return {};
    if (request->getHeader(config_.issuerHeader) != config_.issuer)
        return {};
    auto const& subject = request->getHeader(config_.subjectHeader);
    auto const& expires = request->getHeader(config_.expiryHeader);
    int64_t timestamp = 0;
    auto parsed = std::from_chars(expires.data(), expires.data() + expires.size(), timestamp);
    if (parsed.ec != std::errc() || parsed.ptr != expires.data() + expires.size() ||
        timestamp <= 0 || timestamp > 4102444800LL || subject.empty() || subject.size() > 1024)
        return {};
    auto const expiry = std::min(
        std::chrono::system_clock::time_point(std::chrono::seconds(timestamp)),
        std::chrono::system_clock::now() + std::chrono::seconds(config_.maxBrowserLifetimeSeconds));
    return {
        request->getHeader(config_.issuerHeader),
        subject,
        expiry,
        containsWord(request->getHeader(config_.permissionsHeader), "viewer-read"),
        containsWord(request->getHeader(config_.permissionsHeader), "viewer-control")};
}

bool McpAuthentication::permission(
    nlohmann::json const& claims,
    std::string const& claim,
    std::string const& expected)
{
    if (claim.empty())
        return false;
    auto const pointer = nlohmann::json::json_pointer(claim);
    if (!claims.contains(pointer))
        return false;
    auto const& value = claims.at(pointer);
    return value.is_string() ?
        value == expected :
        value.is_array() && std::find(value.begin(), value.end(), expected) != value.end();
}

McpViewerRelay::Principal McpAuthentication::principal(nlohmann::json const& claims) const
{
    if (!claims.at("exp").is_number_integer() || claims.at("exp").get<int64_t>() <= 0 ||
        claims.at("exp").get<int64_t>() > 4102444800LL ||
        claims.at("sub").get<std::string>().empty() ||
        claims.at("sub").get<std::string>().size() > 1024)
        return {};
    McpViewerRelay::Principal result{
        claims.at("iss"),
        claims.at("sub"),
        std::chrono::system_clock::time_point(
            std::chrono::seconds(claims.at("exp").get<int64_t>())),
        permission(claims, config_.readClaim, config_.readValue),
        permission(claims, config_.controlClaim, config_.controlValue),
        permission(claims, config_.configReadClaim, config_.configReadValue),
        permission(claims, config_.configWriteClaim, config_.configWriteValue),
        permission(claims, config_.diagnosticsClaim, config_.diagnosticsValue)};
    for (auto const& mapping : config_.datasourceHeaderClaims) {
        auto separator = mapping.find('=');
        auto pointer = nlohmann::json::json_pointer(mapping.substr(separator + 1));
        if (!claims.contains(pointer))
            continue;
        auto const& claim = claims.at(pointer);
        if (!claim.is_string())
            continue;
        auto value = claim.get<std::string>();
        if (value.size() > 4096 || value.find_first_of("\r\n") != std::string::npos)
            return {};
        result.datasourceHeaders.emplace(mapping.substr(0, separator), std::move(value));
    }
    return result;
}

bool McpAuthentication::needsKeys(std::string const& token) const
{
    if (config_.mode != McpConfig::Mode::OAuth || config_.jwksUrl.empty() ||
        token.size() > 16 * 1024)
        return false;
    try {
        auto const decoded = decodeToken(token);
        auto const header = parseJson(decoded.get_header(), 4096, 8);
        if (header.value("alg", "") != "RS256" || !header.contains("kid") ||
            header["kid"].get<std::string>().size() > 256)
            return false;
        return keysExpireAt_ <= std::chrono::steady_clock::now() ||
            !keys_.contains(header["kid"].get<std::string>());
    }
    catch (...) {
        return false;
    }
}

McpViewerRelay::Principal
McpAuthentication::bearer(std::string const& token, bool& insufficient) const
{
    insufficient = false;
    if (config_.mode != McpConfig::Mode::OAuth || token.size() > 16 * 1024 ||
        keysExpireAt_ <= std::chrono::steady_clock::now())
        return {};
    try {
        auto const decoded = decodeToken(token);
        auto const header = parseJson(decoded.get_header(), 4096, 8);
        if (header.value("alg", "") != "RS256" || header.contains("crit") ||
            header.contains("jku") || header.contains("x5u") || header.contains("b64"))
            return {};
        auto const key = keys_.find(header.at("kid").get<std::string>());
        if (key == keys_.end())
            return {};
        jwt::verify<jwt::traits::nlohmann_json>()
            .allow_algorithm(jwt::algorithm::rs256(key->second))
            .with_issuer(config_.issuer)
            .with_audience(config_.endpoint)
            .leeway(config_.clockSkewSeconds)
            .verify(decoded);
        auto const claims = parseJson(decoded.get_payload(), 16 * 1024, 16);
        auto result = principal(claims);
        if (result.issuer.empty() || result.expiresAt <= std::chrono::system_clock::now())
            return {};
        auto scope = claims.value("scope", std::string{});
        for (auto const& required : config_.requiredScopes) {
            if (!containsWord(scope, required))
                insufficient = true;
        }
        insufficient |= !result.read && !result.control && !result.configRead &&
            !result.configWrite && !result.diagnostics;
        return insufficient ? McpViewerRelay::Principal{} : result;
    }
    catch (...) {
        return {};
    }
}

void McpAuthentication::installKeys(nlohmann::json const& jwks)
{
    auto const& values = jwks.at("keys");
    if (!values.is_array() || values.empty() || values.size() > 32) {
        throw std::invalid_argument("MCP JWKS requires at most 32 public keys.");
    }
    std::map<std::string, std::string, std::less<>> keys;
    for (auto const& key : values) {
        // Ignore unrelated signing/encryption keys; never let a token choose an algorithm or URL.
        if (key.value("kty", "") != "RSA" || key.value("use", "sig") != "sig" ||
            key.value("alg", "RS256") != "RS256")
            continue;
        if (key.contains("key_ops") && key["key_ops"] != nlohmann::json::array({"verify"}))
            continue;
        auto const kid = key.at("kid").get<std::string>();
        auto const modulus = key.at("n").get<std::string>();
        auto const exponent = key.at("e").get<std::string>();
        if (kid.empty() || kid.size() > 256 || modulus.size() < 342 || modulus.size() > 1366 ||
            exponent.empty() || exponent.size() > 12 || key.contains("d"))
        {
            throw std::invalid_argument(
                "MCP signing keys must be public RSA keys of 2048..8192 bits.");
        }
        auto pem = jwt::helper::create_public_key_from_rsa_components(modulus, exponent);
        auto parsed = jwt::helper::load_public_key_from_string<jwt::error::rsa_error>(pem, "");
        if (EVP_PKEY_bits(parsed.get()) < 2048 || EVP_PKEY_bits(parsed.get()) > 8192) {
            throw std::invalid_argument("Invalid MCP RSA modulus bit length.");
        }
        if (!keys.emplace(kid, std::move(pem)).second) {
            throw std::invalid_argument("MCP signing key IDs must be unique.");
        }
    }
    if (keys.empty())
        throw std::invalid_argument("No usable MCP signing keys.");
    keys_ = std::move(keys);
    keysExpireAt_ = std::chrono::steady_clock::now() + std::chrono::minutes(10);
}

}  // namespace mapget::detail
