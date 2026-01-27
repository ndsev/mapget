#include "tiles-ws-controller.h"

#include "mapget/http-service/http-service.h"

#include "mapget/log.h"
#include "mapget/model/stream.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpTypes.h>
#include <drogon/WebSocketConnection.h>
#include <drogon/WebSocketController.h>

#include <bitsery/adapter/stream.h>
#include <bitsery/bitsery.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fmt/format.h"
#include "nlohmann/json.hpp"

namespace mapget::detail
{
namespace
{

[[nodiscard]] AuthHeaders authHeadersFromRequest(const drogon::HttpRequestPtr& req)
{
    AuthHeaders headers;
    for (auto const& [k, v] : req->headers()) {
        headers.emplace(k, v);
    }
    return headers;
}

[[nodiscard]] std::string_view requestStatusToString(RequestStatus s)
{
    switch (s) {
    case RequestStatus::Open:
        return "Open";
    case RequestStatus::Success:
        return "Success";
    case RequestStatus::NoDataSource:
        return "NoDataSource";
    case RequestStatus::Unauthorized:
        return "Unauthorized";
    case RequestStatus::Aborted:
        return "Aborted";
    }
    return "Unknown";
}

[[nodiscard]] std::string_view loadStateToString(TileLayer::LoadState s)
{
    switch (s) {
    case TileLayer::LoadState::LoadingQueued:
        return "LoadingQueued";
    case TileLayer::LoadState::BackendFetching:
        return "BackendFetching";
    case TileLayer::LoadState::BackendConverting:
        return "BackendConverting";
    }
    return "Unknown";
}

[[nodiscard]] std::string encodeStreamMessage(TileLayerStream::MessageType type, std::string_view payload)
{
    std::ostringstream headerStream;
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(headerStream);
    s.object(TileLayerStream::CurrentProtocolVersion);
    s.value1b(type);
    s.value4b(static_cast<uint32_t>(payload.size()));

    auto message = headerStream.str();
    message.append(payload);
    return message;
}

struct WsConnectionState
{
    AuthHeaders authHeaders;
    TileLayerStream::StringPoolOffsetMap stringPoolOffsets;
    std::shared_ptr<class TilesWsSession> session;
};

class TilesWsSession : public std::enable_shared_from_this<TilesWsSession>
{
public:
    TilesWsSession(
        HttpService& service,
        std::weak_ptr<drogon::WebSocketConnection> conn,
        std::weak_ptr<WsConnectionState> connState,
        AuthHeaders authHeaders,
        TileLayerStream::StringPoolOffsetMap initialOffsets)
        : service_(service),
          loop_(drogon::app().getLoop()),
          conn_(std::move(conn)),
          connState_(std::move(connState)),
          authHeaders_(std::move(authHeaders)),
          offsets_(std::move(initialOffsets)),
          writer_(
              std::make_unique<TileLayerStream::Writer>(
                  [this](std::string msg, TileLayerStream::MessageType type) { onWriterMessage(std::move(msg), type); },
                  offsets_))
    {
    }

    ~TilesWsSession()
    {
        // Best-effort cleanup: abort any in-flight requests if the session is destroyed.
        cancelNoStatus();
    }

    TilesWsSession(TilesWsSession const&) = delete;
    TilesWsSession& operator=(TilesWsSession const&) = delete;

    void start(const nlohmann::json& j)
    {
        auto requestsIt = j.find("requests");
        if (requestsIt == j.end() || !requestsIt->is_array()) {
            queueStatusMessage("Missing or invalid 'requests' array");
            scheduleDrain();
            return;
        }

        try {
            requests_.clear();
            requests_.reserve(requestsIt->size());
            requestStatuses_.clear();
            requestStatuses_.reserve(requestsIt->size());

            for (auto const& requestJson : *requestsIt) {
                const std::string mapId = requestJson.at("mapId").get<std::string>();
                const std::string layerId = requestJson.at("layerId").get<std::string>();
                const auto& tileIdsJson = requestJson.at("tileIds");
                if (!tileIdsJson.is_array()) {
                    throw std::runtime_error("tileIds must be an array");
                }

                std::vector<TileId> tileIds;
                tileIds.reserve(tileIdsJson.size());
                for (auto const& tid : tileIdsJson) {
                    tileIds.emplace_back(tid.get<uint64_t>());
                }

                requests_.push_back(std::make_shared<LayerTilesRequest>(mapId, layerId, std::move(tileIds)));
                requestStatuses_.push_back(RequestStatus::Open);
            }
        }
        catch (const std::exception& e) {
            queueStatusMessage(fmt::format("Invalid request JSON: {}", e.what()));
            scheduleDrain();
            return;
        }

        // Hook request callbacks before calling service_.request so early
        // failures (NoDataSource/Unauthorized) still produce status updates.
        const auto weak = weak_from_this();
        for (size_t i = 0; i < requests_.size(); ++i) {
            auto& req = requests_[i];
            req->onFeatureLayer([weak](auto&& layer) {
                if (auto self = weak.lock()) {
                    self->onTileLayer(std::move(layer));
                }
            });
            req->onSourceDataLayer([weak](auto&& layer) {
                if (auto self = weak.lock()) {
                    self->onTileLayer(std::move(layer));
                }
            });
            req->onLayerLoadStateChanged([weak](MapTileKey const& key, TileLayer::LoadState state) {
                if (auto self = weak.lock()) {
                    self->onLoadStateChanged(key, state);
                }
            });
            req->onDone_ = [weak, i](RequestStatus status) {
                if (auto self = weak.lock()) {
                    self->onRequestDone(i, status);
                }
            };
        }

        // Start processing (may synchronously set request statuses).
        (void)service_.request(requests_, authHeaders_);

        {
            std::lock_guard lock(mutex_);
            statusEmissionEnabled_ = true;
        }
        queueStatusMessage({});
        scheduleDrain();
    }

    void cancel(std::string reason)
    {
        cancelled_ = true;

        // Stop sending any queued tile frames from this session.
        {
            std::lock_guard lock(mutex_);
            outgoing_.clear();
        }

        // Abort in-flight requests (best-effort).
        for (auto const& r : requests_) {
            if (!r || r->isDone())
                continue;
            service_.abort(r);
        }

        // Refresh locally cached statuses after aborting.
        {
            std::lock_guard lock(mutex_);
            for (size_t i = 0; i < requests_.size() && i < requestStatuses_.size(); ++i) {
                if (requests_[i]) {
                    requestStatuses_[i] = requests_[i]->getStatus();
                }
            }
        }

        queueStatusMessage(std::move(reason));
        scheduleDrain();
    }

private:
    struct OutgoingFrame
    {
        std::string bytes;
        std::optional<std::pair<std::string, simfil::StringId>> stringPoolCommit;
    };

    struct WriterMessage
    {
        std::string bytes;
        TileLayerStream::MessageType type{TileLayerStream::MessageType::None};
    };

    void cancelNoStatus()
    {
        if (cancelled_.exchange(true))
            return;

        // Ensure we stop emitting any further frames.
        {
            std::lock_guard lock(mutex_);
            outgoing_.clear();
        }

        for (auto const& r : requests_) {
            if (!r || r->isDone())
                continue;
            service_.abort(r);
        }
    }

    void onWriterMessage(std::string msg, TileLayerStream::MessageType type)
    {
        // Writer messages are only generated from within onTileLayer under mutex_.
        if (!currentWriteBatch_) {
            raise("TilesWsSession writer callback used out-of-band");
        }
        currentWriteBatch_->push_back(WriterMessage{std::move(msg), type});
    }

    void onTileLayer(TileLayer::Ptr layer)
    {
        if (cancelled_)
            return;
        if (!layer)
            return;

        std::vector<WriterMessage> batch;
        std::optional<std::pair<std::string, simfil::StringId>> stringPoolCommit;

        {
            std::lock_guard lock(mutex_);
            if (cancelled_)
                return;

            currentWriteBatch_ = &batch;
            writer_->write(layer);
            currentWriteBatch_ = nullptr;

            // If a StringPool message was generated, the writer updates offsets_
            // to the new highest string ID for this node after emitting it.
            const auto nodeId = layer->nodeId();
            const auto it = offsets_.find(nodeId);
            if (it != offsets_.end()) {
                const auto newOffset = it->second;
                for (auto const& m : batch) {
                    if (m.type == TileLayerStream::MessageType::StringPool) {
                        stringPoolCommit = std::make_pair(nodeId, newOffset);
                        break;
                    }
                }
            }

            for (auto& m : batch) {
                OutgoingFrame frame;
                frame.bytes = std::move(m.bytes);
                if (m.type == TileLayerStream::MessageType::StringPool) {
                    frame.stringPoolCommit = stringPoolCommit;
                }
                outgoing_.push_back(std::move(frame));
            }
        }

        scheduleDrain();
    }

    void onRequestDone(size_t requestIndex, RequestStatus status)
    {
        if (cancelled_)
            return;

        bool shouldEmit = false;
        {
            std::lock_guard lock(mutex_);
            if (cancelled_)
                return;
            if (requestIndex >= requestStatuses_.size())
                return;
            if (requestStatuses_[requestIndex] == status)
                return;
            requestStatuses_[requestIndex] = status;
            shouldEmit = statusEmissionEnabled_;
        }

        if (shouldEmit) {
            queueStatusMessage({});
            scheduleDrain();
        }
    }

    void queueStatusMessage(std::string message)
    {
        OutgoingFrame frame;
        frame.bytes = encodeStreamMessage(TileLayerStream::MessageType::Status, buildStatusPayload(std::move(message)));
        {
            std::lock_guard lock(mutex_);
            outgoing_.push_back(std::move(frame));
        }
    }

    void onLoadStateChanged(MapTileKey const& key, TileLayer::LoadState state)
    {
        if (cancelled_)
            return;

        OutgoingFrame frame;
        frame.bytes = encodeStreamMessage(
            TileLayerStream::MessageType::LoadStateChange,
            buildLoadStatePayload(key, state));
        {
            std::lock_guard lock(mutex_);
            outgoing_.push_back(std::move(frame));
        }
        scheduleDrain();
    }

    [[nodiscard]] std::string buildStatusPayload(std::string message)
    {
        nlohmann::json requestsJson = nlohmann::json::array();
        bool allDone = true;

        {
            std::lock_guard lock(mutex_);
            for (size_t i = 0; i < requests_.size(); ++i) {
                const auto status = (i < requestStatuses_.size()) ? requestStatuses_[i] : RequestStatus::Open;
                allDone &= (status != RequestStatus::Open);

                nlohmann::json reqJson = nlohmann::json::object();
                reqJson["index"] = i;
                if (i < requests_.size() && requests_[i]) {
                    reqJson["mapId"] = requests_[i]->mapId_;
                    reqJson["layerId"] = requests_[i]->layerId_;
                } else {
                    reqJson["mapId"] = "";
                    reqJson["layerId"] = "";
                }
                reqJson["status"] = static_cast<std::underlying_type_t<RequestStatus>>(status);
                reqJson["statusText"] = std::string(requestStatusToString(status));
                requestsJson.push_back(std::move(reqJson));
            }
        }

        return nlohmann::json::object({
            {"type", "mapget.tiles.status"},
            {"allDone", allDone},
            {"requests", std::move(requestsJson)},
            {"message", std::move(message)},
        }).dump();
    }

    [[nodiscard]] std::string buildLoadStatePayload(MapTileKey const& key, TileLayer::LoadState state)
    {
        return nlohmann::json::object({
            {"type", "mapget.tiles.load-state"},
            {"mapId", key.mapId_},
            {"layerId", key.layerId_},
            {"tileId", key.tileId_.value_},
            {"state", static_cast<uint8_t>(state)},
            {"stateText", std::string(loadStateToString(state))},
        }).dump();
    }

    void scheduleDrain()
    {
        if (drainScheduled_.exchange(true))
            return;

        auto weak = weak_from_this();
        loop_->queueInLoop([weak = std::move(weak)]() mutable {
            if (auto self = weak.lock()) {
                self->drainOnLoop();
            }
        });
    }

    void drainOnLoop()
    {
        drainScheduled_ = false;

        auto conn = conn_.lock();
        if (!conn || conn->disconnected()) {
            cancelNoStatus();
            return;
        }

        constexpr size_t maxFramesPerDrain = 256;
        for (size_t i = 0; i < maxFramesPerDrain; ++i) {
            OutgoingFrame frame;
            {
                std::lock_guard lock(mutex_);
                if (outgoing_.empty()) {
                    break;
                }
                frame = std::move(outgoing_.front());
                outgoing_.pop_front();
            }

            conn->send(frame.bytes, drogon::WebSocketMessageType::Binary);
            if (frame.stringPoolCommit) {
                if (auto state = connState_.lock()) {
                    state->stringPoolOffsets[frame.stringPoolCommit->first] = frame.stringPoolCommit->second;
                }
            }
        }

        {
            std::lock_guard lock(mutex_);
            if (outgoing_.empty())
                return;
        }
        scheduleDrain();
    }

    HttpService& service_;
    trantor::EventLoop* loop_;
    std::weak_ptr<drogon::WebSocketConnection> conn_;
    std::weak_ptr<WsConnectionState> connState_;

    AuthHeaders authHeaders_;

    std::mutex mutex_;
    std::deque<OutgoingFrame> outgoing_;

    std::vector<LayerTilesRequest::Ptr> requests_;
    std::vector<RequestStatus> requestStatuses_;
    bool statusEmissionEnabled_ = false;

    TileLayerStream::StringPoolOffsetMap offsets_;
    std::unique_ptr<TileLayerStream::Writer> writer_;
    std::vector<WriterMessage>* currentWriteBatch_ = nullptr;

    std::atomic_bool drainScheduled_{false};
    std::atomic_bool cancelled_{false};
};

class TilesWebSocketController final : public drogon::WebSocketController<TilesWebSocketController, false>
{
public:
    explicit TilesWebSocketController(HttpService& service) : service_(service) {}

    void handleNewConnection(const drogon::HttpRequestPtr& req, const drogon::WebSocketConnectionPtr& conn) override
    {
        auto state = std::make_shared<WsConnectionState>();
        state->authHeaders = authHeadersFromRequest(req);
        conn->setContext(std::move(state));
    }

    void handleNewMessage(
        const drogon::WebSocketConnectionPtr& conn,
        std::string&& message,
        const drogon::WebSocketMessageType& type) override
    {
        auto state = conn->getContext<WsConnectionState>();
        if (!state) {
            state = std::make_shared<WsConnectionState>();
            conn->setContext(state);
        }

        if (type != drogon::WebSocketMessageType::Text) {
            const auto payload = nlohmann::json::object({
                {"type", "mapget.tiles.status"},
                {"allDone", true},
                {"requests", nlohmann::json::array()},
                {"message", "Expected a text message containing JSON."},
            }).dump();
            conn->send(encodeStreamMessage(TileLayerStream::MessageType::Status, payload), drogon::WebSocketMessageType::Binary);
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
            conn->send(encodeStreamMessage(TileLayerStream::MessageType::Status, payload), drogon::WebSocketMessageType::Binary);
            return;
        }

        // Patch per-connection string pool offsets if supplied.
        if (j.contains("stringPoolOffsets")) {
            if (!j["stringPoolOffsets"].is_object()) {
                const auto payload = nlohmann::json::object({
                    {"type", "mapget.tiles.status"},
                    {"allDone", true},
                    {"requests", nlohmann::json::array()},
                    {"message", "stringPoolOffsets must be an object."},
                }).dump();
                conn->send(encodeStreamMessage(TileLayerStream::MessageType::Status, payload), drogon::WebSocketMessageType::Binary);
                return;
            }
            try {
                for (auto const& item : j["stringPoolOffsets"].items()) {
                    state->stringPoolOffsets[item.key()] = item.value().get<simfil::StringId>();
                }
            }
            catch (const std::exception& e) {
                const auto payload = nlohmann::json::object({
                    {"type", "mapget.tiles.status"},
                    {"allDone", true},
                    {"requests", nlohmann::json::array()},
                    {"message", fmt::format("Invalid stringPoolOffsets: {}", e.what())},
                }).dump();
                conn->send(encodeStreamMessage(TileLayerStream::MessageType::Status, payload), drogon::WebSocketMessageType::Binary);
                return;
            }
        }

        if (state->session) {
            state->session->cancel("Replaced by a new /tiles WebSocket request.");
            state->session.reset();
        }

        state->session = std::make_shared<TilesWsSession>(
            service_,
            conn,
            state,
            state->authHeaders,
            state->stringPoolOffsets);
        state->session->start(j);
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override
    {
        if (auto state = conn->getContext<WsConnectionState>()) {
            if (state->session) {
                state->session->cancel("WebSocket connection closed.");
            }
        }
    }

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/tiles", drogon::Get);
    WS_PATH_LIST_END

private:
    HttpService& service_;
};

}  // namespace

void registerTilesWebSocketController(drogon::HttpAppFramework& app, HttpService& service)
{
    app.registerController(std::make_shared<TilesWebSocketController>(service));
}

}  // namespace mapget::detail
