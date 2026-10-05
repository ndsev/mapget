#pragma once

#include "mcp-auth.h"
#include "mcp-native-tools.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpClient.h>
#include <drogon/WebSocketConnection.h>
#include <trantor/net/EventLoopThread.h>

#include <atomic>

namespace mapget::detail
{

/** Owns the MCP HTTP boundary and its control loop, independently of tile scheduling/outboxes. */
class McpServer : public std::enable_shared_from_this<McpServer>
{
public:
    /** Load trust/catalog configuration before accepting any browser registrations. */
    McpServer(McpConfig const& config, std::shared_ptr<McpNativeTools> native);

    /** Drain callbacks and destroy thread-affine state before stopping the private control loop. */
    ~McpServer();

    /** Install MCP/resource metadata routes; the service separately provides disabled /mcp/info. */
    void setup(drogon::HttpAppFramework& app);

    /** Authenticate one real browser handshake and attach its UUID, never browser-supplied claims.
     */
    void attach(
        std::string clientId,
        drogon::HttpRequestPtr const& request,
        drogon::WebSocketConnectionPtr const& connection);

    /** Consume the reserved action namespace even when disabled/invalid, without tile fallback. */
    void receive(
        std::string clientId,
        nlohmann::json message,
        size_t wireBytes,
        drogon::WebSocketConnectionPtr const& connection);

    /** Retire viewer calls when the owning WebSocket closes. */
    void disconnect(std::string clientId);

    /** Stop admission, cancel pending actions and join the independent event loop. */
    void stop();

    /** Return public connection hints from the immutable deployment configuration. */
    [[nodiscard]] nlohmann::json info() const;

private:
    /** One POST response owns only its bounded call and stream, not an MCP protocol session. */
    struct Response
    {
        nlohmann::json rpcId;
        McpViewerRelay::Principal principal;
        /** Select result framing and disconnect semantics without storing a protocol session. */
        bool modern = true;
        std::string action;
        nlohmann::json arguments;
        std::string callId;
        std::shared_ptr<drogon::ResponseStream> stream;
        std::chrono::steady_clock::time_point expiresAt;
        std::shared_ptr<std::atomic_bool> nativeCancellation;
    };

    using Reply = std::function<void(drogon::HttpResponsePtr const&)>;
    McpAuthentication auth_;
    std::shared_ptr<McpActionCatalog const> catalog_;
    std::shared_ptr<McpNativeTools> native_;
    McpConfig::Limits limits_;
    trantor::EventLoopThread thread_{"mapget-mcp"};
    std::unique_ptr<McpViewerRelay> relay_;
    trantor::TimerId timer_;
    std::atomic_bool stopped_{false};
    std::atomic_size_t queued_{0};
    uint64_t nextResponse_ = 0;
    std::map<uint64_t, Response> responses_;
    drogon::HttpClientPtr keyClient_;
    bool fetchingKeys_ = false;
    std::chrono::steady_clock::time_point lastKeyRefresh_;
    std::vector<std::function<void()>> waitingForKeys_;

    /** Marshal external input with a hard queue bound; accepted completions are never dropped. */
    bool post(std::function<void()> task, bool admission = true);

    /** Enforce exposure/body limits before retaining a request on the control loop. */
    void handle(drogon::HttpRequestPtr request, Reply reply);

    /** Refresh trusted signing keys without network I/O on Drogon's request threads. */
    void authenticate(drogon::HttpRequestPtr request, Reply reply);

    /** Verify credentials and dispatch only after key refresh has completed or failed. */
    void authenticated(drogon::HttpRequestPtr const& request, Reply const& reply);

    /** Parse and validate the selected MCP revision, then call native or browser-owned tools. */
    void dispatch(
        drogon::HttpRequestPtr const& request,
        Reply const& reply,
        McpViewerRelay::Principal principal);

    /** Negotiate initialize-era MCP without allocating server-side protocol sessions. */
    void initialize(nlohmann::json const& message, Reply const& reply);

    /** Dispatch only after the SSE stream exists; a lost caller cannot start a new mutation. */
    void attachResponse(uint64_t id, std::shared_ptr<drogon::ResponseStream> stream);

    /** Deliver exactly one terminal tool result and relinquish the POST stream. */
    void completeResponse(uint64_t id, nlohmann::json reply);

    /** Sweep auth/deadlines and detect closed SSE callers using bounded heartbeat writes. */
    void tick();

    /** Refresh a configured HTTPS JWKS endpoint once, coalescing bounded waiting callers. */
    void fetchKeys();

    /** Create JSON HTTP responses with fixed security/cache headers. */
    [[nodiscard]] static drogon::HttpResponsePtr
    jsonResponse(nlohmann::json body, drogon::HttpStatusCode status = drogon::k200OK);

    /** Build a protocol error without reflecting untrusted arguments or credentials. */
    [[nodiscard]] static nlohmann::json
    rpcError(nlohmann::json id, int code, std::string message, nlohmann::json data = nullptr);

    /** Frame application results; screenshots carry image bytes outside structured/text content. */
    [[nodiscard]] static nlohmann::json
    toolResult(nlohmann::json reply, std::string_view action = {});

    /** Send action controls directly over the socket, never through the tile payload outbox. */
    [[nodiscard]] static bool
    send(drogon::WebSocketConnectionPtr const& connection, nlohmann::json const& message);
};

}  // namespace mapget::detail
