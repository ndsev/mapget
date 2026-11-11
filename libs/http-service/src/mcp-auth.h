#pragma once

#include "mcp-viewer-relay.h"
#include "mapget/http-service/proxy-auth.h"

#include <drogon/HttpRequest.h>

#include <filesystem>
#include <map>
#include <optional>

namespace mapget::detail
{

/**
 * Own MCP trust configuration and a bounded set of issuer keys. Permission checks
 * use configured claim/value mappings, without built-in provider-specific role rules.
 * The server serializes key refresh and verification on its control loop;
 * this class adds no lock.
 */
class McpAuthentication
{
public:
    /** Validate explicit, restart-scoped settings shared with the CLI and native embedders. */
    explicit McpAuthentication(McpConfig config, ProxyAuthentication proxyAuth = {});

    /** Return validated deployment settings to the HTTP owner, not to an HTTP caller. */
    [[nodiscard]] McpConfig const& settings() const { return config_; }

    /** Return safe public connection hints; no identity, keys or authorization rules. */
    [[nodiscard]] nlohmann::json info(std::string const& catalogId) const;

    /** Return OAuth protected-resource metadata using configured URLs, never request headers. */
    [[nodiscard]] nlohmann::json metadata() const;

    /** Construct the RFC 9728/6750 challenge, including configured required scopes. */
    [[nodiscard]] std::string challenge(bool insufficient = false) const;

    /** Check configured Host/Origin allowlists without interpreting forwarding headers. */
    [[nodiscard]] bool acceptsRequest(drogon::HttpRequestPtr const& request, bool browser) const;

    /** Bind a browser to verified proxy claims or the explicitly enabled local identity. */
    [[nodiscard]] McpViewerRelay::Principal browser(drogon::HttpRequestPtr const& request) const;

    /** Verify one bearer; insufficient distinguishes missing permissions/scopes from invalid
     * tokens. */
    [[nodiscard]] McpViewerRelay::Principal
    bearer(std::string const& token, bool& insufficient) const;

    /** Extract the case-insensitive Bearer scheme without changing the case-sensitive token. */
    [[nodiscard]] static std::string bearerToken(std::string_view authorization);

    /** Return the local identity only for callers that already passed acceptsRequest(). */
    [[nodiscard]] McpViewerRelay::Principal localPrincipal() const;

    /** Determine whether a structurally plausible token needs a refreshed trusted key set. */
    [[nodiscard]] bool needsKeys(std::string const& token) const;

    /** Validate the entire RSA key set before replacement; failure preserves the prior keys. */
    void installKeys(nlohmann::json const& jwks);

    /** Load bounded key/catalog-adjacent files without accepting URLs. */
    [[nodiscard]] static nlohmann::json
    readJson(std::filesystem::path const& path, size_t maxBytes);

private:
    McpConfig config_;
    ProxyAuthentication proxyAuth_;
    std::map<std::string, std::string, std::less<>> keys_;
    std::chrono::steady_clock::time_point keysExpireAt_;

    /** Extract a bounded subject/expiry and mapped permissions only after signature verification.
     */
    [[nodiscard]] McpViewerRelay::Principal principal(nlohmann::json const& claims) const;

    /** Attach operator grants only to valid authority; native tools still check read permission. */
    [[nodiscard]] McpViewerRelay::Principal
    withDatasourceHeaders(McpViewerRelay::Principal principal) const;
};

}  // namespace mapget::detail
