#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace CLI
{
class App;
}

namespace mapget
{

/** Restart-scoped MCP settings shared by the CLI and native HttpService embedders. */
struct McpConfig
{
    /** Local access is explicitly loopback-only, never an OAuth fallback. */
    enum class Mode { Off, Local, OAuth };

    /** Hard admission limits, including mutations whose completion is still uncertain. */
    struct Limits
    {
        std::chrono::milliseconds timeout{30000};  // Maximum elapsed time after admission.
        size_t invocationBytes = 64 * 1024;        // Bounds the complete invoke envelope.
        size_t resultBytes = 256 * 1024;           // Bounds the complete browser result envelope.
        size_t callsPerSession = 4;                // Prevents one tab retaining all callbacks.
        size_t callsPerPrincipal = 16;             // Bounds work across a user's tabs.
        size_t pendingCalls = 128;  // Includes uncertain mutations; no waiting queue.
        size_t sessions = 256;      // Includes connected but unregistered peers.
    };

    Mode mode = Mode::Off;
    std::string endpoint;                   // Canonical resource URL and required audience.
    std::filesystem::path catalogPath;      // Trusted, generated web-mcp-actions.json artifact.
    std::vector<std::filesystem::path>
        helpDocs;                            // Additional operator-owned Markdown folders/files.
    std::filesystem::path webHelpDirectory;  // Derived from the resolved webapp root, never a URL.
    std::vector<std::string> allowedHosts;  // Exact HTTP Host values, including non-default ports.
    std::vector<std::string> allowedOrigins;  // Exact origins; a browser must supply one.
    Limits limits;

    // Provider-neutral OAuth resource settings. The audience is always endpoint.
    std::string issuer;
    std::string jwksUrl;
    std::filesystem::path jwksFile;
    std::vector<std::string> requiredScopes;
    std::string oauthClientId;
    int clockSkewSeconds = 15;
    std::string readClaim;  // JSON pointer; a claim may be a string or an array of strings.
    std::string readValue;  // Exact required membership; not a pattern or an expression.
    std::string controlClaim;
    std::string controlValue;

    // Native administrative capabilities never follow from ordinary viewer control.
    std::string configReadClaim;
    std::string configReadValue;
    std::string configWriteClaim;
    std::string configWriteValue;
    std::string diagnosticsClaim;
    std::string diagnosticsValue;
    bool configReadEnabled = false;
    bool configWriteEnabled = false;
    /** Explicit operator assertion that this file, not a transformed container copy, is durable. */
    bool directConfigPersistence = false;
    /** Lowercase header=JSON-pointer mappings, evaluated only on verified JWT scalar claims. */
    std::vector<std::string> datasourceHeaderClaims;

    // Browser authority comes only from these headers on a trusted socket peer.
    std::vector<std::string> trustedProxyAddresses;
    std::string issuerHeader;
    std::string subjectHeader;
    std::string expiryHeader;
    std::string permissionsHeader;
    int maxBrowserLifetimeSeconds = 3600;

    /** Bind individual CLI11 options; this object must outlive parsing/callbacks on serve. */
    void addOptions(CLI::App& serve);

    /** Fill omitted local hints/catalog from startup inputs; reject non-loopback local listeners.
     */
    void resolveDefaults(std::string const& host, int port, std::filesystem::path const& webRoot);

    /** Reject incomplete trust settings and invalid bounds before accepting connections. */
    void validate() const;
};

}  // namespace mapget
