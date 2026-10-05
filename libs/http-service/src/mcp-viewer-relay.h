#pragma once

#include "mapget/http-service/mcp-config.h"
#include "mcp-action-catalog.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>

namespace mapget::detail
{

/**
 * Owns authenticated viewer registrations and bounded, asynchronous action calls.
 * All operations run on the constructing thread (the MCP control event loop),
 * independently of tile request replacement, payload admission and service workers.
 */
class McpViewerRelay
{
public:
    /** Verified identity supplied by the resource-server boundary, never by browser JSON. */
    struct Principal
    {
        std::string issuer;   // Subjects are only unique within their issuing authority.
        std::string subject;  // Stable ownership key; email is not an identity.
        std::chrono::system_clock::time_point expiresAt;  // Caps long-lived browser/call authority.
        bool read = false;     // Viewer reads are independent from control/config/data permissions.
        bool control = false;  // Allows mutations but does not imply other privileges.
        bool configRead = false;
        bool configWrite = false;
        bool diagnostics = false;
        /** Explicit deployment mappings from verified claims, never forwarded caller headers. */
        std::unordered_map<std::string, std::string> datasourceHeaders;

        /** Require a named, unexpired identity with an applicable permission. */
        [[nodiscard]] bool valid(std::chrono::system_clock::time_point now) const;

        /** Compare ownership without comparing descriptive or permission metadata. */
        [[nodiscard]] bool sameUser(Principal const& other) const;
    };

    /** Sending means handing a control frame to the connection, never to the tile outbox. */
    using Send = std::function<bool(nlohmann::json const&)>;
    /** Completion contains exactly one result object or application error object. */
    using Complete = std::function<void(nlohmann::json)>;

    /** Bind the initial immutable catalog and clocks; injected clocks make expiry deterministic. */
    McpViewerRelay(
        std::shared_ptr<McpActionCatalog const> catalog,
        McpConfig::Limits limits,
        std::function<std::chrono::system_clock::time_point()> wallNow =
            std::chrono::system_clock::now,
        std::function<std::chrono::steady_clock::time_point()> steadyNow =
            std::chrono::steady_clock::now);

    /** Best-effort shutdown without escaping exceptions or replaying uncertain browser effects. */
    ~McpViewerRelay();

    /** Retire old registrations without closing sockets or changing admitted calls' validators. */
    void replaceCatalog(std::shared_ptr<McpActionCatalog const> catalog);

    /** Attach a verified live connection; duplicate UUIDs never replace an existing owner. */
    [[nodiscard]] bool
    attach(std::string clientId, Principal principal, std::string origin, Send send);

    /** Remove a connection and fail its calls; reconnect must attach a fresh UUID. */
    void disconnect(std::string const& clientId);

    /** Test connection attachment without exposing its owner or registration metadata. */
    [[nodiscard]] bool attached(std::string const& clientId) const;

    /** Consume a validated registration/update/result; invalid or late messages return false. */
    [[nodiscard]] bool receive(std::string const& clientId, nlohmann::json const& message);

    /** Check the versioned client-control envelope without applying defaults/coercions. */
    [[nodiscard]] static bool acceptsMessage(nlohmann::json const& message);

    /** List only the caller's compatible, unexpired tabs, independent of frontend origin. */
    [[nodiscard]] nlohmann::json sessions(Principal const& caller) const;

    /** Admit a call or immediately complete with an error; return its opaque cancellation handle.
     */
    [[nodiscard]] std::string
    invoke(Principal caller, std::string action, nlohmann::json arguments, Complete complete);

    /** Cancel an already admitted call on HTTP disconnect or explicit client cancellation. */
    void cancel(std::string const& callId);

    /** Enforce monotonic deadlines and authentication expiry from the control-loop timer. */
    void expire();

    /** Stop admission and release callbacks before the owning event loop is destroyed. */
    void shutdown();

private:
    /** One connection owns its auth snapshot, capabilities, and uncertain mutation slot. */
    struct Session
    {
        Principal principal;
        std::string origin;
        std::string label;
        std::set<std::string, std::less<>> actions;
        Send send;
        std::string mutationCall;
        bool registered = false;
    };

    /** One dispatched action retains only the callback and state needed until its deadline. */
    struct Call
    {
        std::string clientId;
        std::string action;
        Principal caller;
        std::chrono::steady_clock::time_point deadline;
        Complete complete;
        std::shared_ptr<McpActionCatalog const> catalog;
    };

    std::shared_ptr<McpActionCatalog const> catalog_;
    McpConfig::Limits limits_;
    std::function<std::chrono::system_clock::time_point()> wallNow_;
    std::function<std::chrono::steady_clock::time_point()> steadyNow_;
    std::thread::id const ownerThread_ = std::this_thread::get_id();
    std::map<std::string, Session, std::less<>> sessions_;
    std::map<std::string, Call, std::less<>> calls_;
    uint64_t callSequence_ = 0;
    bool stopped_ = false;

    /** Detect accidental calls from HTTP/data-worker threads instead of introducing a coarse lock.
     */
    void checkThread() const;

    /** Apply metadata atomically after validating every advertised action. */
    bool registerSession(std::string const& clientId, nlohmann::json const& message);

    /** Resolve only the addressed connection's call, rechecking auth and output schema. */
    bool acceptResult(std::string const& clientId, nlohmann::json const& message);

    /** Release a call before callbacks; preserve a mutation slot when its outcome is uncertain. */
    void finish(
        std::string const& callId,
        nlohmann::json reply,
        std::string const& cancelReason = {},
        bool uncertain = false);

    /** Contain callback exceptions without exposing action arguments or results in logs. */
    static void complete(Complete const& callback, nlohmann::json reply);

    /** Contain transport failures; a failed send is not proof a mutation was not received. */
    static bool send(Send callback, nlohmann::json const& message);

    /** Build the shared application error envelope, separate from MCP protocol/auth errors. */
    static nlohmann::json error(std::string code, std::string message, bool uncertain = false);

    /** Reject pathological nesting before invoking the recursive schema/JSON serializer. */
    static bool boundedDepth(nlohmann::json const& value, size_t depth = 0);
};

}  // namespace mapget::detail
