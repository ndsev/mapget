#pragma once

#include "mapget/http-service/http-service.h"
#include "mapget/model/stream.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/WebSocketConnection.h>

#include "nlohmann/json.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace mapget::detail
{

class TilesWsSession;

/** Copy auth headers from the opening request into datasource requests. */
AuthHeaders tilesWsAuthHeadersFromRequest(const drogon::HttpRequestPtr& req);

/** Encode a payload with the TileLayerStream frame header. */
std::string tilesWsEncodeStreamMessage(TileLayerStream::MessageType type, std::string_view payload);

/** Create one websocket session for an accepted `/tiles` connection. */
std::shared_ptr<TilesWsSession> tilesWsCreateSession(
    HttpService& service,
    std::weak_ptr<drogon::WebSocketConnection> conn,
    AuthHeaders authHeaders);

/** Register one session in the status-data weak list. */
void tilesWsRegisterForMetrics(const std::shared_ptr<TilesWsSession>& session);

/** Register/unregister one session for `/tiles/next?clientId=...` lookups. */
void tilesWsRegisterSession(const std::shared_ptr<TilesWsSession>& session);
void tilesWsUnregisterSession(int64_t clientId);

/** Return the numeric client id assigned to a session. */
int64_t tilesWsSessionClientId(const std::shared_ptr<TilesWsSession>& session);

/** Apply client-reported string-pool offsets to one session writer. */
bool tilesWsApplyStringPoolOffsetsPatch(
    const std::shared_ptr<TilesWsSession>& session,
    const nlohmann::json& offsetsJson,
    std::string& errorMessage);

/** Allocate/apply a logical request update received from the websocket client. */
uint64_t tilesWsAllocateRequestId(
    const std::shared_ptr<TilesWsSession>& session,
    const nlohmann::json& requestJson);
void tilesWsUpdateFromClientRequestMessage(
    const std::shared_ptr<TilesWsSession>& session,
    const nlohmann::json& requestJson,
    uint64_t requestId);

/** Abort outstanding backend work for one websocket session. */
void tilesWsCancel(const std::shared_ptr<TilesWsSession>& session, std::string message);

/** Maintain websocket connection counters for metrics. */
void tilesWsRecordConnectionOpened();
void tilesWsRecordConnectionClosed();

/** Handle one HTTP long-poll request for the next queued tile frame. */
void tilesWsHandleNextRequest(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

/** Build the websocket metrics payload consumed by `/status-data`. */
nlohmann::json tilesWebSocketMetricsSnapshotImpl();

} // namespace mapget::detail
