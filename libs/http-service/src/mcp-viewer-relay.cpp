#include "mcp-viewer-relay.h"

#include "mapget/log.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mapget::detail
{

bool McpViewerRelay::Principal::valid(std::chrono::system_clock::time_point now) const
{
    return !issuer.empty() && !subject.empty() && expiresAt > now && (read || control);
}

bool McpViewerRelay::Principal::sameUser(Principal const& other) const
{
    return issuer == other.issuer && subject == other.subject;
}

McpViewerRelay::McpViewerRelay(
    std::shared_ptr<McpActionCatalog const> catalog,
    Limits limits,
    std::function<std::chrono::system_clock::time_point()> wallNow,
    std::function<std::chrono::steady_clock::time_point()> steadyNow)
    : catalog_(std::move(catalog)),
      limits_(limits),
      wallNow_(std::move(wallNow)),
      steadyNow_(std::move(steadyNow))
{
    if (!catalog_ || !wallNow_ || !steadyNow_ || limits_.timeout.count() <= 0 ||
        limits_.timeout.count() > 2147483647 || !limits_.invocationBytes || !limits_.resultBytes ||
        !limits_.callsPerSession || !limits_.callsPerPrincipal || !limits_.pendingCalls ||
        !limits_.sessions)
    {
        throw std::invalid_argument(
            "MCP relay requires a catalog, clocks and positive bounded limits.");
    }
}

McpViewerRelay::~McpViewerRelay()
{
    try {
        shutdown();
    }
    catch (...) {
        // The configurable logger can itself throw; teardown diagnostics must not.
        std::fputs("mapget: MCP viewer relay cleanup failed.\n", stderr);
    }
}

void McpViewerRelay::checkThread() const
{
    if (std::this_thread::get_id() != ownerThread_) {
        throw std::logic_error("MCP viewer relay must run on its owning control loop.");
    }
}

bool McpViewerRelay::attach(
    std::string clientId,
    Principal principal,
    std::string origin,
    Send sender)
{
    checkThread();
    static nlohmann::json_schema::json_validator const uuid(nlohmann::json{
        {"type", "string"},
        {"pattern", "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"}});
    nlohmann::json_schema::basic_error_handler errors;
    uuid.validate(clientId, errors);
    if (stopped_ || errors || !sender || !principal.valid(wallNow_()) || origin.size() > 4096 ||
        sessions_.size() >= limits_.sessions)
    {
        return false;
    }
    Session session;
    session.principal = std::move(principal);
    session.origin = std::move(origin);
    session.send = std::move(sender);
    return sessions_.try_emplace(std::move(clientId), std::move(session)).second;
}

void McpViewerRelay::disconnect(std::string const& clientId)
{
    checkThread();
    auto session = sessions_.extract(clientId);
    if (!session.empty() && !session.mapped().principal.valid(wallNow_())) {
        // Retire action authority before notifying the still-connected UI; normal tile traffic
        // remains available, and synchronous browser replies cannot revive this registration.
        auto reply =
            error("not_available", "Viewer authorization expired; reconnect to authenticate.");
        reply.update({{"type", "mapget.actions.error"}, {"version", 1}, {"operation", "register"}});
        send(session.mapped().send, reply);
    }
    std::vector<std::string> abandoned;
    for (auto const& [id, call] : calls_) {
        if (call.clientId == clientId) {
            abandoned.push_back(id);
        }
    }
    // Also stop an orphaned mutation after browser-auth expiry/service shutdown.
    // The connection is removed first so a synchronous reply cannot complete a call.
    if (!session.empty() && !session.mapped().mutationCall.empty() &&
        !calls_.contains(session.mapped().mutationCall))
    {
        abandoned.push_back(session.mapped().mutationCall);
    }
    for (auto const& id : abandoned) {
        if (!session.empty()) {
            send(
                session.mapped().send,
                {{"type", "mapget.actions.cancel"},
                 {"version", 1},
                 {"callId", id},
                 {"reason", "disconnected"}});
        }
        finish(id, error("disconnected", "Viewer connection ended.", true));
    }
}

bool McpViewerRelay::boundedDepth(nlohmann::json const& value, size_t depth)
{
    if (depth > 64) {
        return false;
    }
    if (value.is_structured()) {
        for (auto const& child : value) {
            if (!boundedDepth(child, depth + 1)) {
                return false;
            }
        }
    }
    return true;
}

bool McpViewerRelay::attached(std::string const& clientId) const
{
    checkThread();
    return !stopped_ && sessions_.contains(clientId);
}

bool McpViewerRelay::acceptsMessage(nlohmann::json const& message)
{
    // This small server-owned wire contract is checked against the generated
    // browser schema fixtures. Application schemas live only in the trusted catalog.
    static nlohmann::json_schema::json_validator const validator(nlohmann::json::parse(R"json({
      "definitions":{
        "version":{"const":1},
        "callId":{"type":"string","minLength":1,"maxLength":128},
        "catalogId":{"type":"string","pattern":"^sha256:[0-9a-f]{64}$"},
        "actions":{"type":"array","maxItems":64,"uniqueItems":true,"items":{"type":"string","minLength":1,"maxLength":128}},
        "error":{"type":"object","additionalProperties":false,"required":["code","message"],"properties":{
          "code":{"enum":["invalid_arguments","not_available","unsupported_action","busy","cancelled","timeout","disconnected","internal_error"]},
          "message":{"type":"string","maxLength":4096},"reason":{"type":"string","maxLength":128},
          "outcome":{"enum":["not_applied","applied","unknown"]}
        }}
      },
      "oneOf":[
        {"type":"object","additionalProperties":false,"required":["type","version","catalogId","actions","label"],"properties":{
          "type":{"enum":["mapget.actions.register","mapget.actions.update"]},"version":{"$ref":"#/definitions/version"},
          "catalogId":{"$ref":"#/definitions/catalogId"},"actions":{"$ref":"#/definitions/actions"},"label":{"type":"string","maxLength":120}
        }},
        {"type":"object","additionalProperties":false,"required":["type","version","callId","result"],"properties":{
          "type":{"const":"mapget.actions.result"},"version":{"$ref":"#/definitions/version"},
          "callId":{"$ref":"#/definitions/callId"},"result":{"type":"object"}
        }},
        {"type":"object","additionalProperties":false,"required":["type","version","callId","error"],"properties":{
          "type":{"const":"mapget.actions.result"},"version":{"$ref":"#/definitions/version"},
          "callId":{"$ref":"#/definitions/callId"},"error":{"$ref":"#/definitions/error"}
        }}
      ]
    })json"));
    if (!boundedDepth(message)) {
        return false;
    }
    nlohmann::json_schema::basic_error_handler errors;
    validator.validate(message, errors);
    return !errors;
}

bool McpViewerRelay::receive(std::string const& clientId, nlohmann::json const& message)
{
    checkThread();
    auto session = sessions_.find(clientId);
    if (stopped_ || session == sessions_.end()) {
        return false;
    }
    if (!session->second.principal.valid(wallNow_())) {
        disconnect(clientId);
        return false;
    }
    auto type = message.is_object() && message.contains("type") && message.at("type").is_string() ?
        message.at("type").get<std::string>() :
        std::string{};
    bool registration = type == "mapget.actions.register" || type == "mapget.actions.update";
    auto maxBytes = registration ? limits_.invocationBytes : limits_.resultBytes;
    if (!boundedDepth(message) || message.dump().size() > maxBytes || !acceptsMessage(message)) {
        if (registration) {
            auto reply = error("invalid_arguments", "Invalid action registration.");
            reply.update(
                {{"type", "mapget.actions.error"},
                 {"version", 1},
                 {"operation", type == "mapget.actions.register" ? "register" : "update"}});
            if (!send(session->second.send, reply)) {
                disconnect(clientId);
            }
        }
        else if (
            type == "mapget.actions.result" && message.contains("callId") &&
            message.at("callId").is_string())
        {
            auto id = message.at("callId").get<std::string>();
            auto call = calls_.find(id);
            if (call != calls_.end() && call->second.clientId == clientId) {
                finish(
                    id,
                    error("internal_error", "Invalid viewer result.", true),
                    "invalid_result",
                    true);
            }
        }
        return false;
    }
    return registration ? registerSession(clientId, message) : acceptResult(clientId, message);
}

bool McpViewerRelay::registerSession(std::string const& clientId, nlohmann::json const& message)
{
    auto& session = sessions_.at(clientId);
    bool update = message.at("type") == "mapget.actions.update";
    auto reject = [&](std::string code, std::string explanation)
    {
        auto reply = error(std::move(code), std::move(explanation));
        reply.update(
            {{"type", "mapget.actions.error"},
             {"version", 1},
             {"operation", update ? "update" : "register"}});
        if (!send(session.send, reply)) {
            disconnect(clientId);
        }
        return false;
    };
    if (message.at("catalogId") != catalog_->id() || (update && !session.registered)) {
        return reject("not_available", "Viewer action catalog or registration is unavailable.");
    }
    std::set<std::string, std::less<>> actions;
    for (auto const& value : message.at("actions")) {
        auto name = value.get<std::string>();
        if (!catalog_->contains(name)) {
            return reject("unsupported_action", "Viewer advertised an unsupported action.");
        }
        actions.insert(std::move(name));
    }
    session.actions = std::move(actions);
    session.label = message.at("label").get<std::string>();
    session.registered = true;
    auto sender = session.send;
    bool sent = send(
        sender,
        {{"type", update ? "mapget.actions.updated" : "mapget.actions.registered"},
         {"version", 1},
         {"clientId", clientId},
         {"catalogId", catalog_->id()}});
    if (!sent) {
        disconnect(clientId);
    }
    return sent;
}

nlohmann::json McpViewerRelay::sessions(Principal const& caller) const
{
    checkThread();
    auto result = nlohmann::json::array();
    auto now = wallNow_();
    if (stopped_ || !caller.read || !caller.valid(now)) {
        return result;
    }
    for (auto const& [id, session] : sessions_) {
        if (!session.registered || !session.principal.valid(now) ||
            !session.principal.sameUser(caller)) {
            continue;
        }
        auto actions = nlohmann::json::array();
        for (auto const& action : session.actions) {
            bool control = catalog_->requiresControl(action);
            if (control ? caller.control && session.principal.control :
                          caller.read && session.principal.read) {
                actions.push_back(action);
            }
        }
        result.push_back(
            {{"clientId", id},
             {"label", session.label},
             {"origin", session.origin},
             {"catalogId", catalog_->id()},
             {"actions", std::move(actions)},
             {"mutationBusy", !session.mutationCall.empty()}});
    }
    return result;
}

std::string McpViewerRelay::invoke(
    Principal caller,
    std::string action,
    nlohmann::json arguments,
    Complete callback)
{
    checkThread();
    if (!callback) {
        throw std::invalid_argument("MCP action calls require a completion callback.");
    }
    auto reject = [&](std::string code, std::string explanation)
    {
        complete(callback, error(std::move(code), std::move(explanation)));
        return std::string{};
    };
    auto now = wallNow_();
    if (stopped_ || !caller.valid(now)) {
        return reject("not_available", "Viewer session is unavailable.");
    }
    if (!arguments.is_object() || !boundedDepth(arguments) ||
        arguments.dump().size() > limits_.invocationBytes || !arguments.contains("clientId") ||
        !arguments.at("clientId").is_string())
    {
        return reject(
            "invalid_arguments",
            "Expected bounded action arguments with a UUID clientId.");
    }
    auto clientId = arguments.at("clientId").get<std::string>();
    auto found = sessions_.find(clientId);
    if (found == sessions_.end() || !found->second.registered ||
        !found->second.principal.valid(now) || !found->second.principal.sameUser(caller))
    {
        // Do not disclose whether another user's UUID is a live session.
        return reject("not_available", "Viewer session is unavailable.");
    }
    auto& session = found->second;
    if (!session.actions.contains(action)) {
        return reject("unsupported_action", "Viewer does not advertise this action.");
    }
    bool control = catalog_->requiresControl(action);
    if (control ? !caller.control || !session.principal.control :
                  !caller.read || !session.principal.read)
    {
        return reject("not_available", "Viewer session is unavailable.");
    }
    arguments.erase("clientId");
    if (!catalog_->acceptsArguments(action, arguments)) {
        return reject("invalid_arguments", "Arguments do not match the action schema.");
    }
    size_t sessionCalls = 0;
    size_t principalCalls = 0;
    size_t totalCalls = calls_.size();
    for (auto const& [id, call] : calls_) {
        sessionCalls += call.clientId == clientId;
        principalCalls += call.caller.sameUser(caller);
    }
    // A timed-out mutation may still execute. Its released HTTP callback is not
    // permission to bypass process/principal admission through other tabs.
    for (auto const& [id, peer] : sessions_) {
        if (!peer.mutationCall.empty() && !calls_.contains(peer.mutationCall)) {
            ++totalCalls;
            sessionCalls += id == clientId;
            principalCalls += peer.principal.sameUser(caller);
        }
    }
    if (totalCalls >= limits_.pendingCalls || sessionCalls >= limits_.callsPerSession ||
        principalCalls >= limits_.callsPerPrincipal ||
        (catalog_->isMutation(action) && !session.mutationCall.empty()))
    {
        return reject("busy", "Viewer action concurrency limit reached.");
    }
    if (callSequence_ == std::numeric_limits<uint64_t>::max()) {
        return reject("not_available", "Viewer call identities exhausted.");
    }
    // A call ID is correlation, not authorization: knowing another call's ID
    // cannot complete it from a different connection.
    auto id = std::to_string(++callSequence_);
    auto budget = std::min(
        limits_.timeout,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::min(caller.expiresAt, session.principal.expiresAt) - now));
    if (budget.count() <= 0) {
        return reject("not_available", "Viewer session is unavailable.");
    }
    nlohmann::json message{
        {"type", "mapget.actions.invoke"},
        {"version", 1},
        {"callId", id},
        {"action", action},
        {"arguments", std::move(arguments)},
        {"timeoutMs", budget.count()}};
    if (message.dump().size() > limits_.invocationBytes) {
        return reject("invalid_arguments", "Action invocation exceeds the wire budget.");
    }
    auto sender = session.send;
    if (catalog_->isMutation(action)) {
        session.mutationCall = id;
    }
    calls_.emplace(
        id,
        Call{
            clientId,
            std::move(action),
            std::move(caller),
            steadyNow_() + budget,
            std::move(callback)});
    if (!send(sender, message)) {
        disconnect(clientId);
    }
    return id;
}

bool McpViewerRelay::acceptResult(std::string const& clientId, nlohmann::json const& message)
{
    auto id = message.at("callId").get<std::string>();
    auto found = calls_.find(id);
    auto& session = sessions_.at(clientId);
    if (found == calls_.end()) {
        // A timed-out/cancelled mutation still owns its slot until the browser
        // confirms termination. A late result never resurrects the HTTP callback.
        if (session.mutationCall == id) {
            session.mutationCall.clear();
        }
        return false;
    }
    auto const& call = found->second;
    if (call.clientId != clientId) {
        return false;
    }
    if (!call.caller.valid(wallNow_()) || !session.principal.sameUser(call.caller)) {
        // This message confirms termination, so there is no uncertain mutation to retain.
        finish(id, error("not_available", "Viewer authorization expired.", true));
        return false;
    }
    if (steadyNow_() >= call.deadline) {
        finish(id, error("timeout", "Viewer action deadline exceeded.", true));
        return false;
    }
    if (message.contains("result") && !catalog_->acceptsResult(call.action, message.at("result"))) {
        finish(
            id,
            error("internal_error", "Viewer result does not match the action schema.", true));
        return false;
    }
    finish(
        id,
        message.contains("result") ?
            nlohmann::json{{"result", message.at("result")}} :
            nlohmann::json{{"error", message.at("error")}});
    return true;
}

void McpViewerRelay::finish(
    std::string const& callId,
    nlohmann::json reply,
    std::string const& cancelReason,
    bool uncertain)
{
    auto node = calls_.extract(callId);
    if (node.empty()) {
        return;
    }
    auto session = sessions_.find(node.mapped().clientId);
    if (session != sessions_.end()) {
        if (!uncertain && session->second.mutationCall == callId) {
            session->second.mutationCall.clear();
        }
        if (!cancelReason.empty()) {
            auto sender = session->second.send;
            auto clientId = session->first;
            if (!send(
                    sender,
                    {{"type", "mapget.actions.cancel"},
                     {"version", 1},
                     {"callId", callId},
                     {"reason", cancelReason}}))
            {
                disconnect(clientId);
            }
        }
    }
    complete(node.mapped().complete, std::move(reply));
}

void McpViewerRelay::cancel(std::string const& callId)
{
    checkThread();
    finish(callId, error("cancelled", "Viewer action cancelled.", true), "cancelled", true);
}

void McpViewerRelay::expire()
{
    checkThread();
    std::vector<std::string> expiredSessions;
    for (auto const& [id, session] : sessions_) {
        if (!session.principal.valid(wallNow_())) {
            expiredSessions.push_back(id);
        }
    }
    for (auto const& id : expiredSessions) {
        disconnect(id);
    }
    std::vector<std::string> expiredCalls;
    for (auto const& [id, call] : calls_) {
        if (!call.caller.valid(wallNow_()) || steadyNow_() >= call.deadline) {
            expiredCalls.push_back(id);
        }
    }
    for (auto const& id : expiredCalls) {
        finish(id, error("timeout", "Viewer action deadline exceeded.", true), "timeout", true);
    }
}

void McpViewerRelay::shutdown()
{
    checkThread();
    stopped_ = true;
    while (!sessions_.empty()) {
        auto id = sessions_.begin()->first;
        disconnect(id);
    }
}

void McpViewerRelay::complete(Complete const& callback, nlohmann::json reply)
{
    try {
        callback(std::move(reply));
    }
    catch (...) {
        log().error("MCP action completion callback failed.");
    }
}

bool McpViewerRelay::send(Send callback, nlohmann::json const& message)
{
    try {
        return callback(message);
    }
    catch (...) {
        log().error("MCP action control delivery failed.");
        return false;
    }
}

nlohmann::json McpViewerRelay::error(std::string code, std::string message, bool uncertain)
{
    return {
        {"error",
         {{"code", std::move(code)},
          {"message", std::move(message)},
          {"outcome", uncertain ? "unknown" : "not_applied"}}}};
}

}  // namespace mapget::detail
