#include "mapget/http-service/mcp-config.h"

#include <trantor/net/InetAddress.h>
#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <regex>
#include <set>
#include <stdexcept>

namespace mapget
{

void McpConfig::addOptions(CLI::App& serve)
{
    // Disabled product builds must not accept MCP from either CLI flags or YAML.
    if constexpr (!MAPGET_WITH_MCP)
        return;
    auto option = [&serve](auto const& name, auto& value, auto const& description)
    {
        return serve.add_option(name, value, description)->group("MCP");
    };
    serve
        .add_option_function<std::string>(
            "--mcp",
            [this](auto const& value) {
                mode = value == "local" ? Mode::Local : value == "oauth" ? Mode::OAuth : Mode::Off;
            },
            "MCP access: off, local (unauthenticated; shares server network exposure), or oauth.")
        ->check(CLI::IsMember({"off", "local", "oauth"}))
        ->group("MCP")
        ->default_str("off");
    option(
        "--mcp-endpoint",
        endpoint,
        "Client-facing URL ending in /mcp; local mode derives it from --host/--port, using "
        "localhost for wildcard listeners. Override for port publishing or a reverse proxy.");
    serve
        .add_option_function<std::string>(
            "--mcp-catalog",
            [this](auto const& path) { catalogPath = path; },
            "Action catalog; defaults to <webapp>/web-mcp-actions.json.")
        ->group("MCP");
    option(
        "--mcp-help-docs",
        helpDocs,
        "Additional Markdown help folders/files, scanned recursively and hot-reloaded. Supplements "
        "bundled and <webapp>/mcp-help docs.");
    option(
        "--mcp-allowed-hosts",
        allowedHosts,
        "Allowed HTTP Host values; local mode defaults to the public endpoint authority.");
    option(
        "--mcp-allowed-origins",
        allowedOrigins,
        "Allowed browser origins; local mode defaults to the public endpoint origin.");
    option(
        "--mcp-issuer",
        issuer,
        "Trusted HTTPS OAuth issuer; tokens must target --mcp-endpoint.");
    option(
        "--mcp-jwks-url",
        jwksUrl,
        "Trusted HTTPS signing-key URL; mutually exclusive with --mcp-jwks-file.");
    serve
        .add_option_function<std::string>(
            "--mcp-jwks-file",
            [this](auto const& path) { jwksFile = path; },
            "Local public signing-key artifact, immutable until restart.")
        ->group("MCP");
    option(
        "--mcp-required-scopes",
        requiredScopes,
        "Required OAuth scopes advertised to MCP hosts.");
    option(
        "--mcp-oauth-client-id",
        oauthClientId,
        "Optional public OAuth client ID advertised in connection hints.");
    option("--mcp-clock-skew-seconds", clockSkewSeconds, "JWT clock tolerance (0..60 seconds).")
        ->default_val(clockSkewSeconds);
    option(
        "--mcp-read-claim",
        readClaim,
        "JSON pointer to a string/array claim granting native-data and viewer reads.");
    option("--mcp-read-value", readValue, "Exact string value required by --mcp-read-claim.");
    option(
        "--mcp-control-claim",
        controlClaim,
        "JSON pointer to a string/array claim granting viewer-control.");
    option(
        "--mcp-control-value",
        controlValue,
        "Exact string value required by --mcp-control-claim.");
    option(
        "--mcp-trusted-proxy-addresses",
        trustedProxyAddresses,
        "Exact socket-peer IPs permitted to supply browser identity headers.");
    option("--mcp-config-read", configReadEnabled, "Enable privileged datasource config reads.");
    option("--mcp-config-write", configWriteEnabled, "Enable privileged datasource config writes.");
    option(
        "--mcp-direct-config-persistence",
        directConfigPersistence,
        "Assert that the native config file is the durable source of truth (not a wrapper copy).");
    option(
        "--mcp-config-read-claim",
        configReadClaim,
        "Verified claim pointer granting config reads.");
    option("--mcp-config-read-value", configReadValue, "Required config-read claim membership.");
    option(
        "--mcp-config-write-claim",
        configWriteClaim,
        "Verified claim pointer granting config writes.");
    option("--mcp-config-write-value", configWriteValue, "Required config-write claim membership.");
    option(
        "--mcp-diagnostics-claim",
        diagnosticsClaim,
        "Verified claim pointer granting global diagnostics.");
    option("--mcp-diagnostics-value", diagnosticsValue, "Required diagnostics claim membership.");
    option(
        "--mcp-datasource-headers",
        datasourceHeaders,
        "Fixed lowercase header=value grants for authenticated OAuth native datasource access.");
    option(
        "--mcp-datasource-header-claims",
        datasourceHeaderClaims,
        "Trusted lowercase header=/claim mappings for native datasource authorization.");
    option(
        "--mcp-browser-issuer-header",
        issuerHeader,
        "Trusted proxy header carrying the browser's OAuth issuer.");
    option(
        "--mcp-browser-subject-header",
        subjectHeader,
        "Trusted proxy header carrying the browser's stable subject.");
    option(
        "--mcp-browser-expiry-header",
        expiryHeader,
        "Trusted proxy header carrying identity expiry as Unix seconds.");
    option(
        "--mcp-browser-permissions-header",
        permissionsHeader,
        "Trusted proxy header carrying viewer-read, viewer-control, config-read, config-write, or "
        "diagnostics words.");
    serve
        .add_option_function<int64_t>(
            "--mcp-timeout-ms",
            [this](auto value) { limits.timeout = std::chrono::milliseconds(value); },
            "Action deadline in milliseconds.")
        ->group("MCP")
        ->default_val(limits.timeout.count());
    option(
        "--mcp-invocation-bytes",
        limits.invocationBytes,
        "Maximum complete action-invocation envelope size.")
        ->default_val(limits.invocationBytes);
    option(
        "--mcp-result-bytes",
        limits.resultBytes,
        "Maximum complete native or browser result envelope size.")
        ->default_val(limits.resultBytes);
    option(
        "--mcp-calls-per-session",
        limits.callsPerSession,
        "Maximum outstanding calls per viewer tab.")
        ->default_val(limits.callsPerSession);
    option(
        "--mcp-calls-per-principal",
        limits.callsPerPrincipal,
        "Per-user limit, enforced separately for native and viewer calls.")
        ->default_val(limits.callsPerPrincipal);
    option(
        "--mcp-pending-calls",
        limits.pendingCalls,
        "Pending HTTP response limit; also bounds retained native and viewer calls separately.")
        ->default_val(limits.pendingCalls);
    option(
        "--mcp-sessions",
        limits.sessions,
        "Maximum connected action peers, including unregistered peers.")
        ->default_val(limits.sessions);
}

void McpConfig::resolveDefaults(
    std::string const& host,
    int port,
    std::filesystem::path const& webRoot)
{
    if (mode == Mode::Off)
        return;
    if constexpr (!MAPGET_WITH_MCP)
        throw std::invalid_argument("MCP support is not included in this build.");
    if (catalogPath.empty() && !webRoot.empty())
        catalogPath = webRoot / "web-mcp-actions.json";
    if (!webRoot.empty())
        webHelpDirectory = webRoot / "mcp-help";
    if (mode != Mode::Local)
        return;

    if (endpoint.empty()) {
        if (port < 1 || port > 65535)
            throw std::invalid_argument(
                "Deriving a local MCP endpoint requires --port in 1..65535.");
        // A wildcard is a bind address, not a usable client-facing URL. Never infer an
        // advertised endpoint from request headers (including forwarded headers).
        auto name = host == "0.0.0.0" || host == "::" ? std::string("localhost") : host;
        if (name.find(':') != std::string::npos && !name.starts_with('['))
            name = "[" + name + "]";
        endpoint = "http://" + name + (port == 80 ? std::string{} : ":" + std::to_string(port)) +
            "/mcp";
    }
    // Derive browser allowlists from the public endpoint, not an internal container port.
    static std::regex const
        localEndpoint(R"(^(https?)://(\[[a-fA-F0-9:]+\]|[a-zA-Z0-9.-]+)(:([0-9]{1,5}))?/mcp$)");
    std::smatch match;
    if (!std::regex_match(endpoint, match, localEndpoint) ||
        (match[4].matched && (std::stoi(match[4]) < 1 || std::stoi(match[4]) > 65535)))
        throw std::invalid_argument(
            "Local MCP endpoint must be an HTTP(S) origin with a valid port followed by /mcp.");
    auto const scheme = match[1].str();
    auto name = match[2].str();
    // Browser URL parsing lowercases DNS names and compresses IPv6 literals. The
    // derived exact Host/Origin allowlists must use the same spelling.
    if (name.starts_with('[')) {
        trantor::InetAddress address(name.substr(1, name.size() - 2), 0, true);
        if (address.isUnspecified())
            throw std::invalid_argument("Local MCP endpoint contains an invalid IPv6 address.");
        name = "[" + address.toIp() + "]";
    }
    else {
        std::transform(
            name.begin(),
            name.end(),
            name.begin(),
            [](unsigned char c) { return std::tolower(c); });
        if (std::all_of(
                name.begin(),
                name.end(),
                [](unsigned char c) { return std::isdigit(c) || c == '.'; }))
        {
            trantor::InetAddress address(name, 0);
            if (address.isUnspecified() || address.toIp() != name)
                throw std::invalid_argument(
                    "Local MCP endpoint requires canonical dotted IPv4 notation.");
        }
    }
    auto const defaultPort = scheme == "https" ? 443 : 80;
    auto const publicPort = match[4].matched ? std::stoi(match[4]) : defaultPort;
    auto const suffix = publicPort == defaultPort ?
        std::string{} :
        ":" + std::to_string(publicPort);
    // Browsers canonicalize default ports and leading zeroes before sending Host/Origin.
    endpoint = scheme + "://" + name + suffix + "/mcp";
    auto const names = name == "localhost" || name == "127.0.0.1" || name == "[::1]" ?
        std::vector<std::string>{"localhost", "127.0.0.1", "[::1]"} :
        std::vector<std::string>{name};
    if (allowedHosts.empty()) {
        for (auto const& name : names)
            allowedHosts.push_back(name + suffix);
    }
    if (allowedOrigins.empty()) {
        for (auto const& name : names)
            allowedOrigins.push_back(scheme + "://" + name + suffix);
    }
}

void McpConfig::validate() const
{
    // Disabling MCP must also work when a YAML file retains hosted deployment settings.
    if (mode == Mode::Off)
        return;
    if constexpr (!MAPGET_WITH_MCP)
        throw std::invalid_argument("MCP support is not included in this build.");
    if (mode != Mode::Local && mode != Mode::OAuth)
        throw std::invalid_argument("MCP mode must be off, local or oauth.");
    static std::regex const
        originPattern(R"(^https?://(\[[a-fA-F0-9:]+\]|[a-zA-Z0-9.-]+)(:[0-9]{1,5})?$)");
    if (!endpoint.ends_with("/mcp") ||
        !std::regex_match(endpoint.substr(0, endpoint.size() - 4), originPattern))
        throw std::invalid_argument(
            "MCP endpoint must be a canonical HTTP(S) origin followed by /mcp.");
    for (auto const* values : {&allowedHosts, &allowedOrigins}) {
        if (values->empty() || values->size() > 32)
            throw std::invalid_argument("MCP requires bounded Host and Origin allowlists.");
        for (auto const& value : *values) {
            auto const origin = values == &allowedHosts ? "http://" + value : value;
            if (value.size() > 2048 || !std::regex_match(origin, originPattern))
                throw std::invalid_argument("Invalid MCP allowed Host/Origin.");
        }
    }
    // A browser catalog is optional: native data tools must also work in a headless service.
    if (helpDocs.size() > 32)
        throw std::invalid_argument("At most 32 additional MCP help paths are allowed.");
    for (auto const& path : helpDocs)
        if (path.empty())
            throw std::invalid_argument("MCP help paths must not be empty.");
    if (configWriteEnabled && !directConfigPersistence)
        throw std::invalid_argument("MCP config writes require explicit direct-file persistence.");
    if (limits.timeout.count() <= 0 || limits.timeout.count() > 2147483647)
        throw std::invalid_argument("MCP timeout must be a positive bounded integer.");
    for (auto value :
         {limits.invocationBytes,
          limits.resultBytes,
          limits.callsPerSession,
          limits.callsPerPrincipal,
          limits.pendingCalls,
          limits.sessions})
    {
        if (value == 0 || value > 2147483647)
            throw std::invalid_argument("MCP limits must be positive bounded integers.");
    }
    if (mode == Mode::Local) {
        if (!issuer.empty() || !jwksUrl.empty() || !jwksFile.empty() || !requiredScopes.empty() ||
            !oauthClientId.empty() || !readClaim.empty() || !readValue.empty() ||
            !controlClaim.empty() || !controlValue.empty() || !trustedProxyAddresses.empty() ||
            !configReadClaim.empty() || !configReadValue.empty() || !configWriteClaim.empty() ||
            !configWriteValue.empty() || !diagnosticsClaim.empty() || !diagnosticsValue.empty() ||
            !datasourceHeaders.empty() || !datasourceHeaderClaims.empty() ||
            !issuerHeader.empty() || !subjectHeader.empty() || !expiryHeader.empty() ||
            !permissionsHeader.empty())
            throw std::invalid_argument("Local MCP must not contain OAuth settings.");
        if (std::find(
                allowedOrigins.begin(),
                allowedOrigins.end(),
                endpoint.substr(0, endpoint.size() - 4)) == allowedOrigins.end())
            throw std::invalid_argument("Local MCP endpoint origin must be allowed.");
        return;
    }

    if (!endpoint.starts_with("https://"))
        throw std::invalid_argument("OAuth MCP requires an HTTPS public endpoint.");
    static std::regex const httpsUrl(R"(^https://[a-zA-Z0-9.\[\]:-]+(/[^\s?#]*)?$)");
    if (!std::regex_match(issuer, httpsUrl))
        throw std::invalid_argument("OAuth issuer must be HTTPS.");
    if (jwksUrl.empty() == jwksFile.empty())
        throw std::invalid_argument(
            "Configure exactly one trusted JWKS URL or local key artifact.");
    if (!jwksUrl.empty() && !std::regex_match(jwksUrl, httpsUrl))
        throw std::invalid_argument(
            "MCP JWKS URL must use HTTPS without credentials/query/fragment.");
    if (requiredScopes.empty() || requiredScopes.size() > 16)
        throw std::invalid_argument("OAuth MCP requires explicit scopes.");
    static std::regex const scopePattern(R"(^[!#-\[\]-~]+$)");
    for (auto const& scope : requiredScopes) {
        if (scope.size() > 256 || !std::regex_match(scope, scopePattern))
            throw std::invalid_argument("Invalid OAuth scope.");
    }
    if (clockSkewSeconds < 0 || clockSkewSeconds > 60)
        throw std::invalid_argument("OAuth clock skew must be between zero and 60 seconds.");
    if (oauthClientId.size() > 256)
        throw std::invalid_argument("Invalid public OAuth client ID.");
    // Administrative-only clients need not receive data access or browser control.
    if (readClaim.empty() && controlClaim.empty() && configReadClaim.empty() &&
        configWriteClaim.empty() && diagnosticsClaim.empty())
        throw std::invalid_argument("OAuth MCP requires explicit permission rules.");
    for (auto const& [claim, value] :
         {std::pair{readClaim, readValue},
          std::pair{controlClaim, controlValue},
          std::pair{configReadClaim, configReadValue},
          std::pair{configWriteClaim, configWriteValue},
          std::pair{diagnosticsClaim, diagnosticsValue}})
    {
        if (claim.empty() != value.empty())
            throw std::invalid_argument(
                "OAuth permission rules require a claim pointer and string value.");
        (void)nlohmann::json::json_pointer(claim);
    }
    if (trustedProxyAddresses.empty() || trustedProxyAddresses.size() > 32)
        throw std::invalid_argument("Browser MCP requires explicitly trusted proxy addresses.");
    for (auto const& address : trustedProxyAddresses) {
        // Exact socket peers only; forwarded chains, hostnames and CIDRs are unsupported.
        if (address.empty() ||
            address.find_first_not_of("0123456789abcdefABCDEF:.") != std::string::npos)
            throw std::invalid_argument(
                "Trusted MCP proxy addresses must be literal IP addresses.");
    }
    std::set<std::string> headers;
    static std::regex const headerPattern("^[a-z][a-z0-9-]{0,99}$");
    if (datasourceHeaders.size() + datasourceHeaderClaims.size() > 16)
        throw std::invalid_argument("At most sixteen MCP datasource header mappings are allowed.");
    for (auto const& mapping : datasourceHeaders) {
        auto separator = mapping.find('=');
        auto name = mapping.substr(0, separator);
        if (separator == std::string::npos || !std::regex_match(name, headerPattern) ||
            !headers.insert(name).second || separator + 1 == mapping.size())
            throw std::invalid_argument("Expected unique lowercase header=value mappings.");
        auto value = mapping.substr(separator + 1);
        if (value.size() > 4096 ||
            std::any_of(
                value.begin(),
                value.end(),
                [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
            throw std::invalid_argument(
                "Static MCP datasource header values must fit 4096 bytes without control "
                "characters.");
    }
    for (auto const& mapping : datasourceHeaderClaims) {
        auto separator = mapping.find('=');
        auto name = mapping.substr(0, separator);
        if (separator == std::string::npos || !std::regex_match(name, headerPattern) ||
            !headers.insert(name).second || mapping.size() > 1024 ||
            separator + 1 == mapping.size() || mapping[separator + 1] != '/')
            throw std::invalid_argument("Expected unique lowercase header=/claim mappings.");
        (void)nlohmann::json::json_pointer(mapping.substr(separator + 1));
    }
    headers.clear();
    for (auto const& name : {issuerHeader, subjectHeader, expiryHeader, permissionsHeader}) {
        if (!std::regex_match(name, headerPattern) || !headers.insert(name).second)
            throw std::invalid_argument(
                "Browser identity header names must be distinct lowercase HTTP tokens.");
    }
}

}  // namespace mapget
