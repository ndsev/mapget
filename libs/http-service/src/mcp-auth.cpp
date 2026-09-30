#include "mcp-auth.h"

#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/nlohmann-json/traits.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace mapget::detail
{
namespace
{
/** Require a closed configuration object so misspelled security settings cannot be ignored. */
void fields(nlohmann::json const& object, std::initializer_list<std::string_view> allowed)
{
    if (!object.is_object()) {
        throw std::invalid_argument("MCP configuration requires an object.");
    }
    for (auto const& [key, value] : object.items()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw std::invalid_argument("Unknown MCP configuration field: " + key);
        }
    }
}

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

McpAuthentication::McpAuthentication(std::filesystem::path const& path)
    : McpAuthentication(readJson(path, 64 * 1024), std::filesystem::absolute(path).parent_path())
{
}

McpAuthentication::McpAuthentication(nlohmann::json config, std::filesystem::path const& directory)
    : config_(std::move(config))
{
    validate(directory);
    if (config_.contains("oauth") && config_["oauth"].contains("jwksFile")) {
        installKeys(readJson(config_["oauth"]["jwksFile"].get<std::string>(), 256 * 1024));
        // A local key artifact is administrator-managed, immutable until service restart.
        keysExpireAt_ = std::chrono::steady_clock::time_point::max();
    }
}

void McpAuthentication::validate(std::filesystem::path const& directory)
{
    fields(
        config_,
        {"authentication",
         "endpoint",
         "catalogPath",
         "allowedHosts",
         "allowedOrigins",
         "oauth",
         "limits"});
    auto const mode = config_.at("authentication").get<std::string>();
    if (mode != "local" && mode != "oauth") {
        throw std::invalid_argument("MCP authentication must be explicitly local or oauth.");
    }
    static std::regex const
        originPattern(R"(^https?://(\[[a-fA-F0-9:]+\]|[a-zA-Z0-9.-]+)(:[0-9]{1,5})?$)");
    auto const endpoint = config_.at("endpoint").get<std::string>();
    if (!endpoint.ends_with("/mcp") ||
        !std::regex_match(endpoint.substr(0, endpoint.size() - 4), originPattern))
    {
        throw std::invalid_argument(
            "MCP endpoint must be a canonical HTTP(S) origin followed by /mcp.");
    }
    for (auto const* key : {"allowedHosts", "allowedOrigins"}) {
        auto const& values = config_.at(key);
        if (!values.is_array() || values.empty() || values.size() > 32) {
            throw std::invalid_argument(
                "MCP requires explicit bounded Host and Origin allowlists.");
        }
        for (auto const& value : values) {
            auto const text = value.get<std::string>();
            auto const origin = std::string(key) == "allowedHosts" ? "http://" + text : text;
            if (text.size() > 2048 || !std::regex_match(origin, originPattern)) {
                throw std::invalid_argument("Invalid MCP allowed Host/Origin.");
            }
        }
    }
    auto resolvePath = [&](nlohmann::json& value)
    {
        auto path = std::filesystem::path(value.get<std::string>());
        if (path.empty()) {
            throw std::invalid_argument("MCP artifact path must not be empty.");
        }
        value = (path.is_absolute() ? path : directory / path).lexically_normal().string();
    };
    resolvePath(config_.at("catalogPath"));
    if (config_.contains("limits")) {
        fields(
            config_["limits"],
            {"timeoutMs",
             "invocationBytes",
             "resultBytes",
             "callsPerSession",
             "callsPerPrincipal",
             "pendingCalls",
             "sessions"});
        for (auto const& [key, value] : config_["limits"].items()) {
            if (!value.is_number_integer() || value.get<int64_t>() <= 0 ||
                value.get<uint64_t>() > 2147483647) {
                throw std::invalid_argument("MCP limits must be positive bounded integers.");
            }
        }
    }
    if (mode == "local") {
        if (config_.contains("oauth")) {
            throw std::invalid_argument("Local MCP must not contain OAuth settings.");
        }
        // Local mode is a deliberate capability, not an authentication fallback behind a proxy.
        for (auto const* key : {"allowedHosts", "allowedOrigins"}) {
            for (auto const& value : config_[key]) {
                auto text = value.get<std::string>();
                if (std::string(key) == "allowedOrigins") {
                    text = text.substr(text.find("://") + 3);
                }
                if (!(text == "localhost" || text.starts_with("localhost:") ||
                      text == "127.0.0.1" || text.starts_with("127.0.0.1:") || text == "[::1]" ||
                      text.starts_with("[::1]:")))
                {
                    throw std::invalid_argument(
                        "Local MCP allowlists must contain loopback hosts only.");
                }
            }
        }
        if (std::find(
                config_["allowedOrigins"].begin(),
                config_["allowedOrigins"].end(),
                endpoint.substr(0, endpoint.size() - 4)) == config_["allowedOrigins"].end())
        {
            throw std::invalid_argument("Local MCP endpoint origin must be allowed.");
        }
        return;
    }
    if (!endpoint.starts_with("https://")) {
        throw std::invalid_argument("OAuth MCP requires an HTTPS public endpoint.");
    }
    auto& oauth = config_.at("oauth");
    fields(
        oauth,
        {"issuer",
         "audience",
         "jwksUrl",
         "jwksFile",
         "requiredScopes",
         "clientId",
         "permissions",
         "browser",
         "clockSkewSeconds"});
    auto issuer = oauth.at("issuer").get<std::string>();
    static std::regex const httpsUrl(R"(^https://[a-zA-Z0-9.\[\]:-]+(/[^\s?#]*)?$)");
    if (!std::regex_match(issuer, httpsUrl) || oauth.at("audience").get<std::string>() != endpoint)
    {
        throw std::invalid_argument(
            "OAuth issuer must be HTTPS and audience must equal the MCP resource URL.");
    }
    if (oauth.contains("jwksUrl") == oauth.contains("jwksFile")) {
        throw std::invalid_argument(
            "Configure exactly one trusted JWKS URL or local key artifact.");
    }
    if (oauth.contains("jwksUrl") &&
        !std::regex_match(oauth["jwksUrl"].get<std::string>(), httpsUrl)) {
        throw std::invalid_argument(
            "MCP JWKS URL must use HTTPS without credentials/query/fragment.");
    }
    if (oauth.contains("jwksFile")) {
        resolvePath(oauth["jwksFile"]);
    }
    auto const& scopes = oauth.at("requiredScopes");
    if (!scopes.is_array() || scopes.empty() || scopes.size() > 16) {
        throw std::invalid_argument("OAuth MCP requires explicit scopes.");
    }
    static std::regex const scopePattern(R"(^[!#-\[\]-~]+$)");
    for (auto const& scope : scopes) {
        if (scope.get<std::string>().size() > 256 ||
            !std::regex_match(scope.get<std::string>(), scopePattern))
        {
            throw std::invalid_argument("Invalid OAuth scope.");
        }
    }
    auto const skew = oauth.value("clockSkewSeconds", 15);
    if (skew < 0 || skew > 60) {
        throw std::invalid_argument("OAuth clock skew must be between zero and 60 seconds.");
    }
    if (oauth.contains("clientId") &&
        (oauth["clientId"].get<std::string>().empty() ||
         oauth["clientId"].get<std::string>().size() > 256))
    {
        throw std::invalid_argument("Invalid public OAuth client ID.");
    }
    fields(oauth.at("permissions"), {"viewer-read", "viewer-control"});
    if (oauth["permissions"].empty()) {
        throw std::invalid_argument("OAuth MCP requires explicit permission rules.");
    }
    for (auto const& rule : oauth["permissions"]) {
        fields(rule, {"claim", "value"});
        auto const path = rule.at("claim").get<std::string>();
        (void)nlohmann::json::json_pointer(path);
        if (path.empty() || rule.at("value").get<std::string>().empty()) {
            throw std::invalid_argument(
                "OAuth permission rules require a claim pointer and string value.");
        }
    }
    auto const& browser = oauth.at("browser");
    fields(
        browser,
        {"trustedProxyAddresses",
         "issuerHeader",
         "subjectHeader",
         "expiryHeader",
         "permissionsHeader",
         "maxLifetimeSeconds"});
    auto const& proxies = browser.at("trustedProxyAddresses");
    if (!proxies.is_array() || proxies.empty() || proxies.size() > 32) {
        throw std::invalid_argument("Browser MCP requires explicitly trusted proxy addresses.");
    }
    for (auto const& proxy : proxies) {
        // Exact peer IPs only; ranges and forwarded chains are intentionally unsupported.
        auto text = proxy.get<std::string>();
        if (text.empty() || text.find_first_not_of("0123456789abcdefABCDEF:.") != std::string::npos)
        {
            throw std::invalid_argument(
                "Trusted MCP proxy addresses must be literal IP addresses.");
        }
    }
    std::set<std::string> headers;
    static std::regex const headerPattern("^[a-z][a-z0-9-]{0,99}$");
    for (auto const* field : {"issuerHeader", "subjectHeader", "expiryHeader", "permissionsHeader"})
    {
        auto name = browser.at(field).get<std::string>();
        if (!std::regex_match(name, headerPattern) || !headers.insert(name).second) {
            throw std::invalid_argument(
                "Browser identity header names must be distinct lowercase HTTP tokens.");
        }
    }
    auto lifetime = browser.value("maxLifetimeSeconds", 3600);
    if (lifetime < 1 || lifetime > 86400) {
        throw std::invalid_argument("Browser MCP lifetime must be between one second and one day.");
    }
}

nlohmann::json McpAuthentication::info(std::string const& catalogId) const
{
    nlohmann::json result{
        {"enabled", true},
        {"endpoint", config_["endpoint"]},
        {"authentication", config_["authentication"]},
        {"catalogId", catalogId},
        {"scopes", nlohmann::json::array()}};
    if (config_.contains("oauth")) {
        result["scopes"] = config_["oauth"]["requiredScopes"];
        if (config_["oauth"].contains("clientId")) {
            result["oauthClientId"] = config_["oauth"]["clientId"];
        }
    }
    return result;
}

nlohmann::json McpAuthentication::metadata() const
{
    return {
        {"resource", config_["endpoint"]},
        {"authorization_servers", {config_["oauth"]["issuer"]}},
        {"scopes_supported", config_["oauth"]["requiredScopes"]},
        {"bearer_methods_supported", {"header"}}};
}

std::string McpAuthentication::challenge(bool insufficient) const
{
    auto endpoint = config_["endpoint"].get<std::string>();
    std::string scopes;
    for (auto const& scope : config_["oauth"]["requiredScopes"]) {
        if (!scopes.empty())
            scopes += ' ';
        scopes += scope.get<std::string>();
    }
    return "Bearer resource_metadata=\"" + endpoint.substr(0, endpoint.size() - 4) +
        "/.well-known/oauth-protected-resource/mcp\", scope=\"" + scopes + "\", error=\"" +
        (insufficient ? "insufficient_scope" : "invalid_token") + "\"";
}

bool McpAuthentication::acceptsRequest(drogon::HttpRequestPtr const& request, bool browser) const
{
    auto const& hosts = config_["allowedHosts"];
    auto const& origins = config_["allowedOrigins"];
    auto const& origin = request->getHeader("origin");
    if (std::find(hosts.begin(), hosts.end(), request->getHeader("host")) == hosts.end() ||
        ((browser || !origin.empty()) &&
         std::find(origins.begin(), origins.end(), origin) == origins.end()))
    {
        return false;
    }
    if (config_["authentication"] == "local") {
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
    if (config_["authentication"] != "local")
        return {};
    return {
        "urn:mapget:local",
        "local-user",
        std::chrono::system_clock::now() + std::chrono::hours(24),
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
    if (config_["authentication"] == "local")
        return localPrincipal();
    auto const& cfg = config_["oauth"]["browser"];
    auto const& proxies = cfg["trustedProxyAddresses"];
    if (std::find(proxies.begin(), proxies.end(), request->peerAddr().toIp()) == proxies.end())
        return {};
    auto header = [&](char const* field) -> std::string const&
    {
        return request->getHeader(cfg[field].get<std::string>());
    };
    if (header("issuerHeader") != config_["oauth"]["issuer"].get<std::string>())
        return {};
    auto const& subject = header("subjectHeader");
    auto const& expires = header("expiryHeader");
    int64_t timestamp = 0;
    auto parsed = std::from_chars(expires.data(), expires.data() + expires.size(), timestamp);
    if (parsed.ec != std::errc() || parsed.ptr != expires.data() + expires.size() ||
        timestamp <= 0 || timestamp > 4102444800LL || subject.empty() || subject.size() > 1024)
        return {};
    auto const expiry = std::min(
        std::chrono::system_clock::time_point(std::chrono::seconds(timestamp)),
        std::chrono::system_clock::now() +
            std::chrono::seconds(cfg.value("maxLifetimeSeconds", 3600)));
    return {
        header("issuerHeader"),
        subject,
        expiry,
        containsWord(header("permissionsHeader"), "viewer-read"),
        containsWord(header("permissionsHeader"), "viewer-control")};
}

bool McpAuthentication::permission(nlohmann::json const& claims, std::string const& name) const
{
    auto const& rules = config_["oauth"]["permissions"];
    if (!rules.contains(name))
        return false;
    auto const& rule = rules[name];
    auto const pointer = nlohmann::json::json_pointer(rule["claim"].get<std::string>());
    if (!claims.contains(pointer))
        return false;
    auto const& value = claims.at(pointer);
    return value.is_string() ?
        value == rule["value"] :
        value.is_array() && std::find(value.begin(), value.end(), rule["value"]) != value.end();
}

McpViewerRelay::Principal McpAuthentication::principal(nlohmann::json const& claims) const
{
    if (!claims.at("exp").is_number_integer() || claims.at("exp").get<int64_t>() <= 0 ||
        claims.at("exp").get<int64_t>() > 4102444800LL ||
        claims.at("sub").get<std::string>().empty() ||
        claims.at("sub").get<std::string>().size() > 1024)
        return {};
    return {
        claims.at("iss"),
        claims.at("sub"),
        std::chrono::system_clock::time_point(
            std::chrono::seconds(claims.at("exp").get<int64_t>())),
        permission(claims, "viewer-read"),
        permission(claims, "viewer-control")};
}

bool McpAuthentication::needsKeys(std::string const& token) const
{
    if (!config_.contains("oauth") || !config_["oauth"].contains("jwksUrl") ||
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
    if (!config_.contains("oauth") || token.size() > 16 * 1024 ||
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
        auto const& oauth = config_["oauth"];
        jwt::verify<jwt::traits::nlohmann_json>()
            .allow_algorithm(jwt::algorithm::rs256(key->second))
            .with_issuer(oauth["issuer"].get<std::string>())
            .with_audience(oauth["audience"].get<std::string>())
            .leeway(oauth.value("clockSkewSeconds", 15))
            .verify(decoded);
        auto const claims = parseJson(decoded.get_payload(), 16 * 1024, 16);
        auto result = principal(claims);
        if (result.issuer.empty() || result.expiresAt <= std::chrono::system_clock::now())
            return {};
        auto scope = claims.value("scope", std::string{});
        for (auto const& required : oauth["requiredScopes"]) {
            if (!containsWord(scope, required.get<std::string>()))
                insufficient = true;
        }
        insufficient |= !result.read && !result.control;
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
