#include "mcp-server.h"

#include "mapget/log.h"
#include "tiles-ws-session.h"

#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <cstdio>
#include <future>
#include <stdexcept>

namespace mapget::detail
{
namespace
{
constexpr auto protocol = "2026-07-28";
constexpr auto initializeProtocol = "2025-11-25";
constexpr auto previousInitializeProtocol = "2025-06-18";
constexpr size_t maxQueuedInputs = 256;
constexpr size_t maxRequestBytes = 64 * 1024;
/** Describe tool ownership; workflows belong in the hot-reloadable help corpus. */
constexpr auto serverInstructions =
    "Mapget provides native map-data tools without requiring a browser. Viewer actions control "
    "authenticated tabs selected through viewer_list_sessions. mapget_docs provides searchable "
    "workflows, datasource semantics and examples. Tool schemas define each call's contract; "
    "check complete, reason and issues before treating bounded results as exhaustive.";

/** Recognize media types without treating a substring in an unrelated type as consent. */
bool accepts(std::string const& header, std::string const& mime)
{
    size_t begin = 0;
    while (begin < header.size()) {
        auto end = header.find(',', begin);
        auto value = header.substr(begin, end == std::string::npos ? end : end - begin);
        value = value.substr(0, value.find(';'));
        auto first = value.find_first_not_of(" \t");
        auto last = value.find_last_not_of(" \t");
        if (first != std::string::npos && value.substr(first, last - first + 1) == mime)
            return true;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return false;
}
}  // namespace

McpServer::McpServer(McpConfig const& config, std::shared_ptr<McpNativeTools> native)
    : auth_(config),
      catalogWriteTime_(
          config.catalogPath.empty() ?
              std::filesystem::file_time_type{} :
              std::filesystem::last_write_time(auth_.settings().catalogPath)),
      catalogSize_(
          config.catalogPath.empty() ?
              0 :
              std::filesystem::file_size(auth_.settings().catalogPath)),
      catalog_(
          config.catalogPath.empty() ?
              std::make_shared<McpActionCatalog>(nlohmann::json{
                  {"formatVersion", 1},
                  {"catalogId", "sha256:" + std::string(64, '0')},
                  {"actions", nlohmann::json::array()},
                  {"channels", nlohmann::json::array()}}) :
              std::make_shared<McpActionCatalog>(
                  McpActionCatalog::load(auth_.settings().catalogPath))),
      native_(std::move(native)),
      limits_(auth_.settings().limits)
{
    thread_.run();
    std::promise<void> ready;
    auto future = ready.get_future();
    thread_.getLoop()->queueInLoop(
        [this, &ready]
        {
            try {
                relay_ = std::make_unique<McpViewerRelay>(catalog_, limits_);
                timer_ = thread_.getLoop()->runEvery(1.0, [this] { tick(); });
                ready.set_value();
            }
            catch (...) {
                ready.set_exception(std::current_exception());
            }
        });
    future.get();
}

McpServer::~McpServer()
{
    stop();
}

void McpServer::stop()
{
    if (stopped_.exchange(true))
        return;
    native_->stop();
    auto* loop = thread_.getLoop();
    loop->queueInLoop(
        [this, loop]
        {
            loop->invalidateTimer(timer_);
            // Relay destruction contains cleanup failures and stays on its owning loop.
            relay_.reset();
            responses_.clear();
            auto waiting = std::move(waitingForKeys_);
            keyClient_.reset();
            for (auto& callback : waiting) {
                try {
                    callback();
                }
                catch (...) {
                    // Neither a failed reply nor logger initialization may strand the join.
                    std::fputs("mapget: MCP shutdown response failed.\n", stderr);
                }
            }
            loop->quit();
        });
    // Joining is the completion barrier; no throwing future wait or borrowed promise is needed.
    thread_.wait();
}

bool McpServer::post(std::function<void()> task, bool admission)
{
    if (stopped_)
        return false;
    if (admission && queued_.fetch_add(1) >= maxQueuedInputs) {
        --queued_;
        return false;
    }
    thread_.getLoop()->queueInLoop(
        [this, task = std::move(task), admission]
        {
            if (admission)
                --queued_;
            // Accepted terminal events must retire their work even while shutdown is pending.
            try {
                task();
            }
            catch (...) {
                log().error("MCP control callback failed.");
            }
        });
    return true;
}

void McpServer::refreshCatalog()
{
    auto const& path = auth_.settings().catalogPath;
    if (stopped_ || path.empty())
        return;
    std::shared_ptr<McpActionCatalog const> next;
    try {
        auto modified = std::filesystem::last_write_time(path);
        auto size = std::filesystem::file_size(path);
        if (modified == catalogWriteTime_ && size == catalogSize_)
            return;
        // Remember failed versions too: a partial build must not recompile on every request.
        // Sample before reading so a concurrent rewrite is noticed on the next discovery.
        catalogWriteTime_ = modified;
        catalogSize_ = size;
        next = std::make_shared<McpActionCatalog>(McpActionCatalog::load(path));
    }
    catch (...) {
        if (!catalogReloadFailed_)
            log().warn("MCP action catalog reload failed; keeping the last valid catalog.");
        catalogReloadFailed_ = true;
        return;
    }
    catalogReloadFailed_ = false;
    if (next->id() != catalog_->id()) {
        relay_->replaceCatalog(next);
        catalog_ = std::move(next);
        log().info("Reloaded the trusted MCP browser action catalog.");
    }
}

void McpServer::setup(drogon::HttpAppFramework& app)
{
    auto weak = weak_from_this();
    auto handler = [weak](drogon::HttpRequestPtr const& request, Reply&& reply)
    {
        if (auto self = weak.lock())
            self->handle(request, std::move(reply));
        else
            reply(jsonResponse({{"error", "MCP unavailable"}}, drogon::k503ServiceUnavailable));
    };
    app.registerHandler(
        "/mcp",
        decltype(handler){handler},
        {drogon::Post, drogon::Get, drogon::Delete, drogon::Options});
    // Drogon's forwarding-reference binder retains lvalues by reference. Every route must own
    // its closure after setup() returns, rather than referencing this stack-local handler.
    app.registerHandler("/mcp/info", decltype(handler){handler}, {drogon::Get, drogon::Options});
    app.registerHandler(
        "/.well-known/oauth-protected-resource/mcp",
        decltype(handler){handler},
        {drogon::Get, drogon::Options});
    app.registerHandler(
        "/.well-known/oauth-protected-resource",
        decltype(handler){handler},
        {drogon::Get, drogon::Options});
    if (auth_.settings().mode == McpConfig::Mode::Local) {
        app.registerBeginningAdvice(
            [&app]
            {
                // An embedding application has the same local-only obligation as the serve CLI.
                for (auto const& listener : app.getListeners()) {
                    auto address = listener.toIp();
                    if (address != "127.0.0.1" && address != "::1") {
                        throw std::invalid_argument(
                            "Local MCP requires a loopback-only HTTP listener (--host 127.0.0.1).");
                    }
                }
            });
    }
}

drogon::HttpResponsePtr McpServer::jsonResponse(nlohmann::json body, drogon::HttpStatusCode status)
{
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(status);
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("X-Content-Type-Options", "nosniff");
    if (!body.is_null())
        response->setBody(body.dump());
    return response;
}

void McpServer::handle(drogon::HttpRequestPtr request, Reply reply)
{
    if (!auth_.acceptsRequest(request, false)) {
        reply(jsonResponse(
            {{"error", "MCP Host/Origin or exposure policy rejected the request."}},
            drogon::k403Forbidden));
        return;
    }
    auto origin = request->getHeader("origin");
    auto respond = [reply = std::move(reply), origin](drogon::HttpResponsePtr const& response)
    {
        response->addHeader("Cache-Control", "no-store");
        response->addHeader("X-Content-Type-Options", "nosniff");
        if (!origin.empty()) {
            response->addHeader("Access-Control-Allow-Origin", origin);
            response->addHeader("Vary", "Origin");
            response->addHeader(
                "Access-Control-Expose-Headers",
                "WWW-Authenticate, MCP-Protocol-Version");
        }
        reply(response);
    };
    if (request->method() == drogon::Options) {
        auto response = jsonResponse(nullptr, drogon::k204NoContent);
        response->addHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
        response->addHeader(
            "Access-Control-Allow-Headers",
            "Authorization, Content-Type, MCP-Protocol-Version, Mcp-Method, Mcp-Name");
        respond(response);
        return;
    }
    if (request->path() == "/mcp/info") {
        // Discovery shares the catalog's owner loop; no HTTP thread can observe a half-swap.
        if (!post(
                [this, respond]
                {
                    refreshCatalog();
                    respond(jsonResponse(auth_.info(catalog_->id())));
                }))
            respond(jsonResponse({{"error", "MCP unavailable"}}, drogon::k503ServiceUnavailable));
        return;
    }
    if (request->path().starts_with("/.well-known/oauth-protected-resource")) {
        respond(
            auth_.settings().mode == McpConfig::Mode::OAuth ?
                jsonResponse(auth_.metadata()) :
                jsonResponse({{"error", "OAuth is not enabled."}}, drogon::k404NotFound));
        return;
    }
    if (request->method() != drogon::Post) {
        auto response = jsonResponse(
            {{"error", "Use POST for MCP requests; no GET event stream or replay is provided."}},
            drogon::k405MethodNotAllowed);
        response->addHeader("Allow", "POST, OPTIONS");
        respond(response);
        return;
    }
    if (request->body().size() > maxRequestBytes) {
        respond(jsonResponse(
            {{"error", "MCP request is too large."}},
            drogon::k413RequestEntityTooLarge));
        return;
    }
    if (!accepts(request->getHeader("content-type"), "application/json")) {
        respond(jsonResponse(
            {{"error", "Expected application/json."}},
            drogon::k415UnsupportedMediaType));
        return;
    }
    if (!accepts(request->getHeader("accept"), "application/json") ||
        !accepts(request->getHeader("accept"), "text/event-stream"))
    {
        respond(jsonResponse(
            {{"error", "Accept must include application/json and text/event-stream."}},
            drogon::k406NotAcceptable));
        return;
    }
    if (!post([this, request, respond] { authenticate(request, respond); })) {
        respond(jsonResponse(
            {{"error", "MCP admission is full or shutting down."}},
            drogon::k503ServiceUnavailable));
    }
}

void McpServer::authenticate(drogon::HttpRequestPtr request, Reply reply)
{
    if (stopped_) {
        reply(jsonResponse({{"error", "MCP unavailable"}}, drogon::k503ServiceUnavailable));
        return;
    }
    auto token = McpAuthentication::bearerToken(request->getHeader("authorization"));
    if (!auth_.needsKeys(token)) {
        authenticated(request, reply);
        return;
    }
    // Unknown kid values cannot drive one outbound request per inbound token.
    auto const mayRefresh = std::chrono::steady_clock::now() - lastKeyRefresh_ >=
        std::chrono::seconds(10);
    if ((!fetchingKeys_ && !mayRefresh) || waitingForKeys_.size() >= 128) {
        authenticated(request, reply);
        return;
    }
    waitingForKeys_.emplace_back([this, request = std::move(request), reply = std::move(reply)]
                                 { authenticated(request, reply); });
    if (!fetchingKeys_)
        fetchKeys();
}

void McpServer::fetchKeys()
{
    fetchingKeys_ = true;
    lastKeyRefresh_ = std::chrono::steady_clock::now();
    auto url = auth_.settings().jwksUrl;
    auto slash = url.find('/', 8);
    keyClient_ =
        drogon::HttpClient::newHttpClient(url.substr(0, slash), thread_.getLoop(), false, true);
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setPath(slash == std::string::npos ? "/" : url.substr(slash));
    request->addHeader("Accept", "application/json");
    keyClient_->sendRequest(
        request,
        [weak = weak_from_this()](auto status, auto const& response)
        {
            auto self = weak.lock();
            if (!self || self->stopped_)
                return;
            try {
                if (status != drogon::ReqResult::Ok || !response ||
                    response->statusCode() != drogon::k200OK) {
                    throw std::runtime_error("JWKS request failed");
                }
                // No redirect, token-controlled key URL or stale-key grace beyond the cache
                // lifetime.
                self->auth_
                    .installKeys(McpAuthentication::parseJson(response->body(), 256 * 1024, 16));
            }
            catch (...) {
                log().warn("MCP trusted signing-key refresh failed.");
            }
            self->fetchingKeys_ = false;
            auto waiting = std::move(self->waitingForKeys_);
            for (auto& callback : waiting)
                callback();
        },
        5.0);
}

void McpServer::authenticated(drogon::HttpRequestPtr const& request, Reply const& reply)
{
    if (stopped_) {
        reply(jsonResponse({{"error", "MCP unavailable"}}, drogon::k503ServiceUnavailable));
        return;
    }
    auto principal = auth_.localPrincipal();
    if (auth_.settings().mode == McpConfig::Mode::OAuth) {
        bool insufficient = false;
        principal = auth_.bearer(
            McpAuthentication::bearerToken(request->getHeader("authorization")),
            insufficient);
        if (!principal.valid(std::chrono::system_clock::now())) {
            auto response = jsonResponse(
                {{"error", insufficient ? "insufficient_scope" : "invalid_token"}},
                insufficient ? drogon::k403Forbidden : drogon::k401Unauthorized);
            response->addHeader("WWW-Authenticate", auth_.challenge(insufficient));
            reply(response);
            return;
        }
    }
    dispatch(request, reply, std::move(principal));
}

nlohmann::json
McpServer::rpcError(nlohmann::json id, int code, std::string message, nlohmann::json data)
{
    nlohmann::json error{{"code", code}, {"message", std::move(message)}};
    if (!data.is_null())
        error["data"] = std::move(data);
    return {{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"error", std::move(error)}};
}

nlohmann::json McpServer::toolResult(nlohmann::json reply, std::string_view action)
{
    bool const failed = reply.contains("error");
    auto body = failed ? std::move(reply) : std::move(reply.at("result"));
    auto content = nlohmann::json::array();
    if (!failed && action == "viewer_screenshot") {
        try {
            auto image = std::move(body.at("image"));
            auto metadata = std::move(body.at("metadata"));
            auto const& data = image.at("data").get_ref<std::string const&>();
            // Do not run a recursive regular expression over a large base64 string. A bounded
            // codec roundtrip also rejects whitespace, URL-safe alphabets and noncanonical padding.
            if (image.at("mimeType") != "image/jpeg" || data.empty() || data.size() > 240000 ||
                drogon::utils::base64Encode(drogon::utils::base64Decode(data)) != data)
                throw std::invalid_argument("Invalid screenshot image encoding");
            content.push_back(
                {{"type", "image"},
                 {"mimeType", std::move(image.at("mimeType"))},
                 {"data", std::move(image.at("data"))}});
            body = std::move(metadata);
        }
        catch (...) {
            return toolResult(
                {{"error",
                  {{"code", "internal_error"},
                   {"message", "Viewer screenshot has invalid image content."}}}});
        }
    }
    content.push_back({{"type", "text"}, {"text", body.dump()}});
    return {
        {"resultType", "complete"},
        {"isError", failed},
        {"structuredContent", std::move(body)},
        {"content", std::move(content)}};
}

void McpServer::initialize(nlohmann::json const& message, Reply const& reply)
{
    auto const& params = message.at("params");
    auto requested = params.at("protocolVersion").get<std::string>();
    auto const& client = params.at("clientInfo");
    if (requested.empty() || !params.at("capabilities").is_object() ||
        !client.at("name").is_string() || !client.at("version").is_string())
    {
        reply(jsonResponse(
            rpcError(message["id"], -32602, "Invalid MCP initialization."),
            drogon::k400BadRequest));
        return;
    }
    // 2025 clients negotiate an initialize-era revision. Do not send them 2026's incompatible
    // lifecycle merely because it is newer; every subsequent POST is independently authorized.
    auto const selected = requested == previousInitializeProtocol ?
        previousInitializeProtocol :
        initializeProtocol;
    reply(jsonResponse(
        {{"jsonrpc", "2.0"},
         {"id", message["id"]},
         {"result",
          {{"protocolVersion", selected},
           {"instructions", serverInstructions},
           {"capabilities", {{"tools", nlohmann::json::object()}}},
           {"serverInfo", {{"name", "mapget"}, {"version", MAPGET_MCP_VERSION}}}}}}));
}

void McpServer::dispatch(
    drogon::HttpRequestPtr const& request,
    Reply const& reply,
    McpViewerRelay::Principal principal)
{
    auto const version = request->getHeader("mcp-protocol-version");
    bool const modern = version == protocol;
    nlohmann::json message;
    nlohmann::json id = nullptr;
    try {
        message = McpAuthentication::parseJson(request->body(), maxRequestBytes);
    }
    catch (...) {
        reply(jsonResponse(
            rpcError(nullptr, -32700, "Invalid or excessively nested JSON."),
            drogon::k400BadRequest));
        return;
    }
    if (!message.is_object() || !message.contains("jsonrpc") || message["jsonrpc"] != "2.0" ||
        !message.contains("method") || !message["method"].is_string() ||
        (message.contains("id") &&
         !(message["id"].is_string() || message["id"].is_number_integer())) ||
        (message.contains("id") && message["id"].is_string() &&
         message["id"].get_ref<std::string const&>().size() > 256))
    {
        reply(jsonResponse(
            rpcError(nullptr, -32600, "Expected one JSON-RPC request or notification."),
            drogon::k400BadRequest));
        return;
    }
    id = message.value("id", nlohmann::json(nullptr));
    auto error =
        [&](int code,
            std::string const& text,
            drogon::HttpStatusCode status = drogon::k400BadRequest,
            nlohmann::json data = nullptr)
    {
        reply(jsonResponse(rpcError(id, code, text, std::move(data)), status));
    };
    auto result = [&](nlohmann::json body)
    {
        if (!modern)
            body.erase("resultType");
        reply(jsonResponse({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(body)}}));
    };
    try {
        auto const method = message.at("method").get<std::string>();
        auto const params = message.value("params", nlohmann::json::object());
        if (!params.is_object()) {
            error(-32602, "MCP parameters must be an object.");
            return;
        }
        if (method == "initialize" && !id.is_null() && !modern) {
            initialize(message, reply);
            return;
        }
        if (version.empty() || (modern && request->getHeader("mcp-method").empty())) {
            error(-32020, "Missing required MCP request header");
            return;
        }
        if (!modern && version != initializeProtocol && version != previousInitializeProtocol) {
            error(
                -32022,
                "Unsupported protocol version",
                drogon::k400BadRequest,
                {{"supported", {protocol, initializeProtocol, previousInitializeProtocol}},
                 {"requested", version}});
            return;
        }
        if (modern) {
            auto const& meta = params.at("_meta");
            if (!meta.is_object() ||
                meta.value("io.modelcontextprotocol/protocolVersion", "") != version ||
                request->getHeader("mcp-method") != method)
            {
                error(-32020, "MCP request metadata/header mismatch");
                return;
            }
            auto const& client = meta.at("io.modelcontextprotocol/clientInfo");
            if (!client.is_object() || !client.at("name").is_string() ||
                !client.at("version").is_string() ||
                !meta.at("io.modelcontextprotocol/clientCapabilities").is_object())
            {
                error(-32600, "Invalid MCP client metadata");
                return;
            }
        }
        if (id.is_null()) {
            if (!modern && method == "notifications/initialized") {
                reply(jsonResponse(nullptr, drogon::k202Accepted));
                return;
            }
            if (!modern && method == "notifications/cancelled") {
                auto const& cancelled = params.at("requestId");
                if (!cancelled.is_string() && !cancelled.is_number_integer()) {
                    error(-32602, "Cancellation requires a requestId.");
                    return;
                }
                // This transport has no MCP sessions. Two agents belonging to one user can
                // reuse the same requestId: never guess which browser invocation to cancel.
                // Work remains bounded by the relay deadline and mutation ownership barrier.
                reply(jsonResponse(nullptr, drogon::k202Accepted));
                return;
            }
            error(-32600, "Unsupported MCP notification.");
            return;
        }
        if (!modern && method == "ping") {
            result(nlohmann::json::object());
            return;
        }
        if (modern && method == "server/discover") {
            result(
                {{"resultType", "complete"},
                 {"instructions", serverInstructions},
                 {"supportedVersions", {protocol}},
                 {"capabilities", {{"tools", nlohmann::json::object()}}},
                 {"_meta",
                  {{"io.modelcontextprotocol/serverInfo",
                    {{"name", "mapget"}, {"version", MAPGET_MCP_VERSION}}}}}});
            return;
        }
        if (method == "tools/list" || method == "tools/call")
            refreshCatalog();
        if (method == "tools/list") {
            if (params.contains("cursor")) {
                error(-32602, "MCP tool lists are not paginated.");
                return;
            }
            auto tools = catalog_->tools(principal.read, principal.control);
            for (auto& tool : native_->tools(principal))
                tools.push_back(std::move(tool));
            tools.push_back(
                {{"name", "viewer_list_sessions"},
                 {"description",
                  "List your connected compatible viewer tabs; target a returned clientId "
                  "explicitly in each viewer action."},
                 {"inputSchema",
                  {{"type", "object"},
                   {"properties", nlohmann::json::object()},
                   {"additionalProperties", false}}},
                 {"outputSchema",
                  {{"type", "object"},
                   {"properties",
                    {{"sessions", {{"type", "array"}, {"items", {{"type", "object"}}}}}}},
                   {"required", {"sessions"}}}},
                 {"annotations", {{"readOnlyHint", true}}}});
            result({{"resultType", "complete"}, {"tools", std::move(tools)}});
            return;
        }
        if (method != "tools/call") {
            error(-32601, "Method not found", drogon::k404NotFound);
            return;
        }
        auto const action = params.at("name").get<std::string>();
        auto headerName = request->getHeader("mcp-name");
        if (headerName.starts_with("=?base64?") && headerName.ends_with("?=")) {
            headerName = drogon::utils::base64Decode(headerName.substr(9, headerName.size() - 11));
        }
        if (modern && headerName != action) {
            error(-32020, "MCP tool name/header mismatch");
            return;
        }
        auto arguments = params.value("arguments", nlohmann::json::object());
        if (!arguments.is_object()) {
            error(-32602, "Tool arguments must be an object.");
            return;
        }
        if (action == "viewer_list_sessions") {
            if (!arguments.empty()) {
                error(-32602, "viewer_list_sessions takes no arguments.");
                return;
            }
            result(toolResult({{"result", {{"sessions", relay_->sessions(principal)}}}}));
            return;
        }
        auto const native = native_->contains(action);
        if (!native && !catalog_->contains(action)) {
            error(-32602, "Unknown tool.");
            return;
        }
        // Permissions are checked here and again by the relay, before schema validation/dispatch.
        if (!native &&
            ((catalog_->requiresControl(action) && !principal.control) ||
             (!catalog_->requiresControl(action) && !principal.read)))
        {
            result(toolResult(
                {{"error", {{"code", "not_available"}, {"message", "Tool permission denied."}}}}));
            return;
        }
        if (!native && (!arguments.contains("clientId") || !arguments["clientId"].is_string())) {
            error(-32602, "A clientId UUID is required.");
            return;
        }
        auto applicationArguments = arguments;
        applicationArguments.erase("clientId");
        auto issues = nlohmann::json::array();
        if (native ? !native_->acceptsArguments(action, arguments, &issues) :
                     !catalog_->acceptsArguments(action, applicationArguments, &issues))
        {
            error(
                -32602,
                "Invalid tool arguments; see field-level issues.",
                drogon::k400BadRequest,
                {{"issues", std::move(issues)}});
            return;
        }
        if (responses_.size() >= limits_.pendingCalls) {
            result(toolResult(
                {{"error",
                  {{"code", "busy"}, {"message", "MCP pending response limit reached."}}}}));
            return;
        }
        auto responseId = ++nextResponse_;
        responses_.emplace(
            responseId,
            Response{
                id,
                std::move(principal),
                modern,
                action,
                std::move(arguments),
                {},
                {},
                std::chrono::steady_clock::now() + limits_.timeout});
        auto weak = weak_from_this();
        auto response = drogon::HttpResponse::newAsyncStreamResponse(
            [weak, responseId](drogon::ResponseStreamPtr stream)
            {
                auto sharedStream = std::shared_ptr<drogon::ResponseStream>(std::move(stream));
                if (auto self = weak.lock()) {
                    if (self->post(
                            [self, responseId, sharedStream]
                            { self->attachResponse(responseId, sharedStream); },
                            false))
                        return;
                }
                sharedStream->close();
            },
            true);
        response->setContentTypeString("text/event-stream");
        response->addHeader("X-Accel-Buffering", "no");
        response->addHeader("MCP-Protocol-Version", version);
        reply(response);
    }
    catch (...) {
        error(-32602, "Invalid MCP request parameters.");
    }
}

void McpServer::attachResponse(uint64_t id, std::shared_ptr<drogon::ResponseStream> stream)
{
    auto found = responses_.find(id);
    if (stopped_ || found == responses_.end() || !stream->send(": mapget\n\n")) {
        responses_.erase(id);
        stream->close();
        return;
    }
    found->second.stream = std::move(stream);
    if (native_->contains(found->second.action)) {
        auto weak = weak_from_this();
        auto token = native_->invoke(
            found->second.principal,
            found->second.action,
            std::move(found->second.arguments),
            [weak, id](auto reply)
            {
                if (auto self = weak.lock())
                    (void)self->post(
                        [self, id, reply = std::move(reply)]() mutable
                        { self->completeResponse(id, std::move(reply)); },
                        false);
            });
        if (auto pending = responses_.find(id); pending != responses_.end())
            pending->second.nativeCancellation = std::move(token);
        return;
    }
    // Retain the action name for result presentation, notably screenshot ImageContent.
    auto call = relay_->invoke(
        std::move(found->second.principal),
        found->second.action,
        std::move(found->second.arguments),
        [this, id](auto reply) { completeResponse(id, std::move(reply)); });
    // Invalid/admission-limited calls complete synchronously and may already have erased the
    // response.
    if (auto pending = responses_.find(id); pending != responses_.end())
        pending->second.callId = std::move(call);
}

void McpServer::completeResponse(uint64_t id, nlohmann::json reply)
{
    auto response = responses_.extract(id);
    if (response.empty() || !response.mapped().stream)
        return;
    auto& state = response.mapped();
    auto result = toolResult(std::move(reply), state.action);
    if (!state.modern)
        result.erase("resultType");
    auto body =
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", state.rpcId}, {"result", std::move(result)}}
            .dump();
    // Image data appears once, but metadata is present in both structured and text content.
    // Enforce the complete outgoing envelope too, rather than only the incoming browser frame.
    if (state.action == "viewer_screenshot" && body.size() + 24 > limits_.resultBytes) {
        auto error = toolResult(
            {{"error",
              {{"code", "result_too_large"},
               {"message",
                "Screenshot exceeds the MCP result budget; request smaller dimensions."}}}});
        if (!state.modern)
            error.erase("resultType");
        body = nlohmann::json{{"jsonrpc", "2.0"}, {"id", state.rpcId}, {"result", std::move(error)}}
                   .dump();
    }
    state.stream->send("event: message\ndata: " + body + "\n\n");
    state.stream->close();
}

void McpServer::tick()
{
    if (stopped_)
        return;
    native_->refreshHelp();
    relay_->expire();
    std::vector<uint64_t> closed;
    std::vector<uint64_t> timedOut;
    for (auto const& [id, response] : responses_) {
        if (response.nativeCancellation && response.expiresAt <= std::chrono::steady_clock::now()) {
            native_->cancel(response.nativeCancellation);
            timedOut.push_back(id);
            continue;
        }
        if ((!response.stream && response.expiresAt <= std::chrono::steady_clock::now()) ||
            (response.stream && !response.stream->send(": keep-alive\n\n")))
            closed.push_back(id);
    }
    for (auto id : timedOut)
        completeResponse(
            id,
            {{"error",
              {{"code", "timeout"},
               {"message",
                "Native action deadline exceeded; a mutation may already have been applied."}}}});
    for (auto id : closed) {
        auto response = responses_.extract(id);
        if (!response.empty() && response.mapped().modern && response.mapped().nativeCancellation)
            native_->cancel(response.mapped().nativeCancellation);
        // 2025 disconnect is not cancellation: discard its HTTP waiter, but leave accepted
        // browser work bounded by its existing deadline. 2026 explicitly uses disconnect to cancel.
        if (!response.empty() && response.mapped().modern && !response.mapped().callId.empty())
            relay_->cancel(response.mapped().callId);
    }
}

bool McpServer::send(
    drogon::WebSocketConnectionPtr const& connection,
    nlohmann::json const& message)
{
    if (!connection || !connection->connected())
        return false;
    connection->send(
        tilesWsEncodeStreamMessage(TileLayerStream::MessageType::ActionControl, message.dump()),
        drogon::WebSocketMessageType::Binary);
    return true;
}

void McpServer::attach(
    std::string clientId,
    drogon::HttpRequestPtr const& request,
    drogon::WebSocketConnectionPtr const& connection)
{
    auto principal = auth_.browser(request);
    if (!principal.valid(std::chrono::system_clock::now()))
        return;
    auto weak = std::weak_ptr<drogon::WebSocketConnection>(connection);
    (void)post(
        [this,
         clientId = std::move(clientId),
         principal = std::move(principal),
         origin = request->getHeader("origin"),
         weak]
        {
            if (!stopped_ && !weak.expired()) {
                (void)relay_->attach(
                    clientId,
                    principal,
                    origin,
                    [weak](auto const& message) { return send(weak.lock(), message); });
            }
        });
}

void McpServer::receive(
    std::string clientId,
    nlohmann::json message,
    size_t wireBytes,
    drogon::WebSocketConnectionPtr const& connection)
{
    auto type = message.value("type", "");
    bool const registration = type == "mapget.actions.register" || type == "mapget.actions.update";
    auto operation = type == "mapget.actions.update" ? "update" : "register";
    auto error = nlohmann::json{
        {"type", "mapget.actions.error"},
        {"version", 1},
        {"operation", operation},
        {"error",
         {{"code", "not_available"}, {"message", "Action connection unavailable or invalid."}}}};
    auto weak = std::weak_ptr<drogon::WebSocketConnection>(connection);
    if (wireBytes > limits_.resultBytes ||
        !post(
            [this,
             clientId = std::move(clientId),
             message = std::move(message),
             weak,
             error,
             registration]
            {
                if (stopped_ || !relay_->attached(clientId)) {
                    if (registration)
                        (void)send(weak.lock(), error);
                    return;
                }
                if (registration)
                    refreshCatalog();
                // The relay reports malformed registrations and fails affected calls itself.
                (void)relay_->receive(clientId, message);
            }))
    {
        if (registration)
            (void)send(connection, error);
    }
}

void McpServer::disconnect(std::string clientId)
{
    (void)post(
        [this, clientId = std::move(clientId)]
        {
            if (relay_)
                relay_->disconnect(clientId);
        },
        false);
}

}  // namespace mapget::detail
