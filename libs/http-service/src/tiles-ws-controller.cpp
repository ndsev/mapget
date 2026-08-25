#include "tiles-ws-controller.h"

#include "tiles-ws-session.h"

#include "mapget/log.h"
#include "mapget/model/stream.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/WebSocketConnection.h>
#include <drogon/WebSocketController.h>

#include "fmt/format.h"
#include "nlohmann/json.hpp"

#include <memory>
#include <string>
#include <utility>

namespace mapget::detail
{
namespace
{

/** Drogon adapter that forwards websocket lifecycle events to `TilesWsSession`. */
class TilesWebSocketController final : public drogon::WebSocketController<TilesWebSocketController, false>
{
public:
    /** Build the websocket controller bound to one shared HttpService instance. */
    explicit TilesWebSocketController(HttpService& service) : service_(service) {}

    /** Create and attach one `TilesWsSession` per accepted websocket connection. */
    void handleNewConnection(const drogon::HttpRequestPtr& req, const drogon::WebSocketConnectionPtr& conn) override
    {
        tilesWsRecordConnectionOpened();
        auto session = tilesWsCreateSession(service_, conn, tilesWsAuthHeadersFromRequest(req));
        tilesWsRegisterForMetrics(session);
        tilesWsRegisterSession(session);
        conn->setContext(std::move(session));
    }

    /**
     * Handle control and request messages from the websocket client.
     * Drogon invokes this without exception protection, so all parse/session
     * failures are converted to status frames or logged instead of escaping.
     */
    void handleNewMessage(
        const drogon::WebSocketConnectionPtr& conn,
        std::string&& message,
        const drogon::WebSocketMessageType& type) override
    {
        try {
            auto session = conn->getContext<TilesWsSession>();
            if (!session) {
                // Recover from unexpected Drogon context loss by creating a fresh session.
                session = tilesWsCreateSession(service_, conn, AuthHeaders{});
                tilesWsRegisterForMetrics(session);
                tilesWsRegisterSession(session);
                conn->setContext(session);
            }

            // Drogon delivers WebSocket control frames to the controller.
            // In particular, its periodic connection-health ping produces a
            // Pong here.  Control frames are not tile-stream requests and
            // must not be turned into an untagged terminal status.
            if (type == drogon::WebSocketMessageType::Ping
                || type == drogon::WebSocketMessageType::Pong
                || type == drogon::WebSocketMessageType::Close) {
                return;
            }

            if (type != drogon::WebSocketMessageType::Text) {
                const auto payload = nlohmann::json::object({
                    {"type", "mapget.tiles.status"},
                    {"allDone", true},
                    {"requests", nlohmann::json::array()},
                    {"message", "Expected a text message containing JSON."},
                }).dump();
                conn->send(
                    tilesWsEncodeStreamMessage(TileLayerStream::MessageType::Status, payload),
                    drogon::WebSocketMessageType::Binary);
                return;
            }

            nlohmann::json j;
            try {
                j = nlohmann::json::parse(message);
            }
            catch (const std::exception& e) {
                const auto payload = nlohmann::json::object({
                    {"type", "mapget.tiles.status"},
                    {"allDone", true},
                    {"requests", nlohmann::json::array()},
                    {"message", fmt::format("Invalid JSON: {}", e.what())},
                }).dump();
                conn->send(
                    tilesWsEncodeStreamMessage(TileLayerStream::MessageType::Status, payload),
                    drogon::WebSocketMessageType::Binary);
                return;
            }

            if (j.contains("stringPoolOffsets")) {
                std::string errorMessage;
                if (!tilesWsApplyStringPoolOffsetsPatch(session, j["stringPoolOffsets"], errorMessage)) {
                    const auto payload = nlohmann::json::object({
                        {"type", "mapget.tiles.status"},
                        {"allDone", true},
                        {"requests", nlohmann::json::array()},
                        {"message", std::move(errorMessage)},
                    }).dump();
                    conn->send(
                        tilesWsEncodeStreamMessage(TileLayerStream::MessageType::Status, payload),
                        drogon::WebSocketMessageType::Binary);
                    return;
                }
            }

            const auto requestId = tilesWsAllocateRequestId(session, j);
            tilesWsUpdateFromClientRequestMessage(session, std::move(j), requestId);
        }
        catch (const std::exception& e) {
            log().error("WebSocket message handler failed: {}", e.what());
        }
    }

    /** Abort outstanding backend work once the websocket is closed. */
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override
    {
        tilesWsRecordConnectionClosed();
        if (auto session = conn->getContext<TilesWsSession>()) {
            tilesWsUnregisterSession(tilesWsSessionClientId(session));
            tilesWsCancel(session, "WebSocket connection closed.");
        }
    }

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/interactive", drogon::Get);
    // Keep accepting the historic websocket path for deployments whose reverse
    // proxy rules have not yet been updated to `/interactive`.
    WS_PATH_ADD("/tiles", drogon::Get);
    WS_PATH_LIST_END

private:
    HttpService& service_;
};

}  // namespace

/** Register the websocket controller plus HTTP fallback pull endpoint. */
void registerTilesWebSocketController(drogon::HttpAppFramework& app, HttpService& service)
{
    app.registerController(std::make_shared<TilesWebSocketController>(service));
    auto registerPayloadEndpoint = [&app](std::string const& path) {
        app.registerHandler(
            path,
            [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                tilesWsHandleNextRequest(req, std::move(callback));
            },
            {drogon::Get, drogon::Post});
    };

    registerPayloadEndpoint("/interactive/payload");
    // Keep the long-poll drain endpoint paired with the legacy `/tiles`
    // websocket path for stale reverse-proxy configurations.
    registerPayloadEndpoint("/tiles/next");
}

/** Return process-wide websocket metrics for `/status-data`. */
nlohmann::json tilesWebSocketMetricsSnapshot()
{
    return tilesWebSocketMetricsSnapshotImpl();
}

}  // namespace mapget::detail
