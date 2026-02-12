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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <limits>
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

struct TilesWsMetrics
{
    std::atomic<int64_t> activeConnections{0};
    std::atomic<int64_t> activeSessions{0};
    std::atomic<int64_t> totalQueuedFrames{0};
    std::atomic<int64_t> totalQueuedBytes{0};
    std::atomic<int64_t> totalForwardedFrames{0};
    std::atomic<int64_t> totalForwardedBytes{0};
    std::atomic<int64_t> totalDroppedFrames{0};
    std::atomic<int64_t> totalDroppedBytes{0};
    std::atomic<int64_t> totalDrainCalls{0};
    std::atomic<int64_t> replacedRequests{0};
    std::atomic<int64_t> totalFlowGrantMessages{0};
    std::atomic<int64_t> totalFlowGrantFrames{0};
    std::atomic<int64_t> totalFlowGrantBytes{0};
    std::atomic<int64_t> totalFlowBlockedDrains{0};
};

TilesWsMetrics gTilesWsMetrics;
std::mutex gTrackedSessionsMutex;
std::vector<std::weak_ptr<class TilesWsSession>> gTrackedSessions;
std::mutex gTrackedConnectionsMutex;
std::vector<std::weak_ptr<class WsConnectionState>> gTrackedConnections;

constexpr std::string_view kFlowGrantType = "mapget.tiles.flow-grant";
constexpr int64_t kFlowCreditMaxFrames = 16;
constexpr int64_t kFlowCreditMaxBytes = 64 * 1024 * 1024;

[[nodiscard]] int64_t nonNegative(std::atomic<int64_t> const& value)
{
    const auto v = value.load(std::memory_order_relaxed);
    return v < 0 ? 0 : v;
}

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

[[nodiscard]] int64_t parseNonNegativeInt64(const nlohmann::json& j, std::string_view key)
{
    const auto keyString = std::string(key);
    const auto it = j.find(keyString);
    if (it == j.end()) {
        return 0;
    }
    if (it->is_number_unsigned()) {
        const auto raw = it->get<uint64_t>();
        const auto max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        return static_cast<int64_t>(std::min(raw, max));
    }
    if (it->is_number_integer()) {
        const auto raw = it->get<int64_t>();
        return std::max<int64_t>(0, raw);
    }
    return 0;
}

[[nodiscard]] bool isFlowControlledDataFrameType(TileLayerStream::MessageType type)
{
    return type == TileLayerStream::MessageType::StringPool
        || type == TileLayerStream::MessageType::TileFeatureLayer
        || type == TileLayerStream::MessageType::TileSourceDataLayer;
}

struct FlowControlStateSnapshot
{
    bool enabled = false;
    int64_t creditFrames = 0;
    int64_t creditBytes = 0;
};

struct WsConnectionState
{
    AuthHeaders authHeaders;
    TileLayerStream::StringPoolOffsetMap stringPoolOffsets;
    std::shared_ptr<class TilesWsSession> session;
    uint64_t nextRequestId = 1;

    mutable std::mutex flowControlMutex;
    bool flowControlEnabled = false;
    int64_t flowCreditFrames = 0;
    int64_t flowCreditBytes = 0;

    void setFlowControlEnabled(bool enabled)
    {
        std::lock_guard lock(flowControlMutex);
        if (enabled) {
            if (!flowControlEnabled) {
                flowControlEnabled = true;
                flowCreditFrames = kFlowCreditMaxFrames;
                flowCreditBytes = kFlowCreditMaxBytes;
            }
            return;
        }
        flowControlEnabled = false;
        flowCreditFrames = 0;
        flowCreditBytes = 0;
    }

    [[nodiscard]] std::pair<int64_t, int64_t> grantFlowCredits(int64_t frames, int64_t bytes)
    {
        std::lock_guard lock(flowControlMutex);
        if (!flowControlEnabled) {
            return {0, 0};
        }
        const auto safeFrames = std::max<int64_t>(0, frames);
        const auto safeBytes = std::max<int64_t>(0, bytes);
        const auto oldFrames = flowCreditFrames;
        const auto oldBytes = flowCreditBytes;
        flowCreditFrames = std::min<int64_t>(kFlowCreditMaxFrames, flowCreditFrames + safeFrames);
        flowCreditBytes = std::min<int64_t>(kFlowCreditMaxBytes, flowCreditBytes + safeBytes);
        return {flowCreditFrames - oldFrames, flowCreditBytes - oldBytes};
    }

    [[nodiscard]] bool consumeFlowCreditForFrame(int64_t frameSizeBytes)
    {
        std::lock_guard lock(flowControlMutex);
        if (!flowControlEnabled) {
            return true;
        }
        if (flowCreditFrames <= 0 || flowCreditBytes <= 0) {
            return false;
        }
        flowCreditFrames -= 1;
        flowCreditBytes = std::max<int64_t>(0, flowCreditBytes - std::max<int64_t>(0, frameSizeBytes));
        return true;
    }

    [[nodiscard]] FlowControlStateSnapshot flowControlSnapshot() const
    {
        std::lock_guard lock(flowControlMutex);
        return FlowControlStateSnapshot{
            .enabled = flowControlEnabled,
            .creditFrames = flowCreditFrames,
            .creditBytes = flowCreditBytes,
        };
    }
};

class TilesWsSession : public std::enable_shared_from_this<TilesWsSession>
{
public:
    TilesWsSession(
        HttpService& service,
        std::weak_ptr<drogon::WebSocketConnection> conn,
        std::weak_ptr<WsConnectionState> connState,
        uint64_t requestId,
        AuthHeaders authHeaders,
        TileLayerStream::StringPoolOffsetMap initialOffsets)
        : service_(service),
          conn_(std::move(conn)),
          connState_(std::move(connState)),
          requestId_(requestId),
          authHeaders_(std::move(authHeaders)),
          offsets_(std::move(initialOffsets)),
          writer_(
              std::make_unique<TileLayerStream::Writer>(
                  [this](std::string msg, TileLayerStream::MessageType type) { onWriterMessage(std::move(msg), type); },
                  offsets_))
    {
        gTilesWsMetrics.activeSessions.fetch_add(1, std::memory_order_relaxed);
    }

    ~TilesWsSession()
    {
        gTilesWsMetrics.activeSessions.fetch_sub(1, std::memory_order_relaxed);
        // Best-effort cleanup: abort any in-flight requests if the session is destroyed.
        cancelNoStatus();
    }

    TilesWsSession(TilesWsSession const&) = delete;
    TilesWsSession& operator=(TilesWsSession const&) = delete;

    void registerForMetrics()
    {
        std::lock_guard lock(gTrackedSessionsMutex);
        gTrackedSessions.push_back(weak_from_this());
    }

    [[nodiscard]] std::pair<int64_t, int64_t> pendingSnapshot()
    {
        std::lock_guard lock(mutex_);
        int64_t pendingFrames = static_cast<int64_t>(outgoing_.size());
        int64_t pendingBytes = 0;
        for (auto const& frame : outgoing_) {
            pendingBytes += static_cast<int64_t>(frame.bytes.size());
        }
        return {pendingFrames, pendingBytes};
    }

    void onFlowGrant()
    {
        scheduleDrain();
    }

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
                    self->onTileLayer(std::forward<decltype(layer)>(layer));
                }
            });
            req->onSourceDataLayer([weak](auto&& layer) {
                if (auto self = weak.lock()) {
                    self->onTileLayer(std::forward<decltype(layer)>(layer));
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
        queueRequestContextMessage();
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
            clearOutgoingLocked();
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
        TileLayerStream::MessageType type{TileLayerStream::MessageType::None};
        std::optional<std::pair<std::string, simfil::StringId>> stringPoolCommit;
    };

    struct WriterMessage
    {
        std::string bytes;
        TileLayerStream::MessageType type{TileLayerStream::MessageType::None};
    };

    void enqueueOutgoingLocked(OutgoingFrame&& frame)
    {
        const auto bytes = static_cast<int64_t>(frame.bytes.size());
        outgoing_.push_back(std::move(frame));
        gTilesWsMetrics.totalQueuedFrames.fetch_add(1, std::memory_order_relaxed);
        gTilesWsMetrics.totalQueuedBytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    void clearOutgoingLocked()
    {
        if (outgoing_.empty()) {
            return;
        }

        int64_t droppedFrames = 0;
        int64_t droppedBytes = 0;
        for (auto const& frame : outgoing_) {
            ++droppedFrames;
            droppedBytes += static_cast<int64_t>(frame.bytes.size());
        }
        outgoing_.clear();

        gTilesWsMetrics.totalDroppedFrames.fetch_add(droppedFrames, std::memory_order_relaxed);
        gTilesWsMetrics.totalDroppedBytes.fetch_add(droppedBytes, std::memory_order_relaxed);
    }

    void cancelNoStatus()
    {
        if (cancelled_.exchange(true))
            return;

        // Ensure we stop emitting any further frames.
        {
            std::lock_guard lock(mutex_);
            clearOutgoingLocked();
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
        if (!currentWriteBatch_.has_value()) {
            raise("TilesWsSession writer callback used out-of-band");
        }
        currentWriteBatch_->push_back(WriterMessage{std::move(msg), type});
    }

    void onTileLayer(TileLayer::Ptr const& layer)
    {
        if (cancelled_)
            return;
        if (!layer)
            return;

        std::optional<std::pair<std::string, simfil::StringId>> stringPoolCommit;

        {
            std::lock_guard lock(mutex_);
            if (cancelled_)
                return;

            if (currentWriteBatch_.has_value()) {
                raise("TilesWsSession writer callback re-entered");
            }
            currentWriteBatch_.emplace();
            writer_->write(layer);
            auto batch = std::move(*currentWriteBatch_);
            currentWriteBatch_.reset();

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
                frame.type = m.type;
                if (m.type == TileLayerStream::MessageType::StringPool) {
                    frame.stringPoolCommit = stringPoolCommit;
                }
                enqueueOutgoingLocked(std::move(frame));
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
        frame.type = TileLayerStream::MessageType::Status;
        {
            std::lock_guard lock(mutex_);
            enqueueOutgoingLocked(std::move(frame));
        }
    }

    void queueRequestContextMessage()
    {
        OutgoingFrame frame;
        frame.bytes =
            encodeStreamMessage(TileLayerStream::MessageType::RequestContext, buildRequestContextPayload());
        frame.type = TileLayerStream::MessageType::RequestContext;
        {
            std::lock_guard lock(mutex_);
            enqueueOutgoingLocked(std::move(frame));
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
        frame.type = TileLayerStream::MessageType::LoadStateChange;
        {
            std::lock_guard lock(mutex_);
            enqueueOutgoingLocked(std::move(frame));
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
            {"requestId", requestId_},
            {"allDone", allDone},
            {"requests", std::move(requestsJson)},
            {"message", std::move(message)},
        }).dump();
    }

    [[nodiscard]] std::string buildLoadStatePayload(MapTileKey const& key, TileLayer::LoadState state) const
    {
        return nlohmann::json::object({
            {"type", "mapget.tiles.load-state"},
            {"requestId", requestId_},
            {"mapId", key.mapId_},
            {"layerId", key.layerId_},
            {"tileId", key.tileId_.value_},
            {"state", static_cast<uint8_t>(state)},
            {"stateText", std::string(loadStateToString(state))},
        }).dump();
    }

    [[nodiscard]] std::string buildRequestContextPayload() const
    {
        return nlohmann::json::object({
            {"type", "mapget.tiles.request-context"},
            {"requestId", requestId_},
        }).dump();
    }

    void scheduleDrain()
    {
        if (drainScheduled_.exchange(true))
            return;
        drainNow();
    }

    void drainNow()
    {
        gTilesWsMetrics.totalDrainCalls.fetch_add(1, std::memory_order_relaxed);

        // Keep one active drainer at a time and bound each batch to avoid
        // pushing very large bursts into Drogon's internal connection buffers.
        for (;;) {
            auto conn = conn_.lock();
            if (!conn || conn->disconnected()) {
                drainScheduled_.store(false, std::memory_order_relaxed);
                cancelNoStatus();
                return;
            }

            constexpr size_t maxFramesPerDrain = 64;
            constexpr size_t maxBytesPerDrain = 2 * 1024 * 1024;
            size_t drainedBytes = 0;
            bool blockedByFlowControl = false;

            for (size_t i = 0; i < maxFramesPerDrain && drainedBytes < maxBytesPerDrain; ++i) {
                OutgoingFrame frame;
                {
                    std::lock_guard lock(mutex_);
                    if (outgoing_.empty()) {
                        break;
                    }
                    frame = std::move(outgoing_.front());
                    outgoing_.pop_front();
                }

                const auto frameBytes = static_cast<int64_t>(frame.bytes.size());

                if (cancelled_) {
                    gTilesWsMetrics.totalDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                    gTilesWsMetrics.totalDroppedBytes.fetch_add(frameBytes, std::memory_order_relaxed);
                    continue;
                }

                if (isFlowControlledDataFrameType(frame.type)) {
                    auto state = connState_.lock();
                    if (!state || !state->consumeFlowCreditForFrame(frameBytes)) {
                        std::lock_guard lock(mutex_);
                        outgoing_.push_front(std::move(frame));
                        blockedByFlowControl = true;
                        break;
                    }
                }

                drainedBytes += static_cast<size_t>(frameBytes);
                gTilesWsMetrics.totalForwardedFrames.fetch_add(1, std::memory_order_relaxed);
                gTilesWsMetrics.totalForwardedBytes.fetch_add(frameBytes, std::memory_order_relaxed);
                conn->send(frame.bytes, drogon::WebSocketMessageType::Binary);
                if (frame.stringPoolCommit) {
                    if (auto state = connState_.lock()) {
                        state->stringPoolOffsets[frame.stringPoolCommit->first] = frame.stringPoolCommit->second;
                    }
                }
            }

            bool done = false;
            {
                std::lock_guard lock(mutex_);
                if (blockedByFlowControl || outgoing_.empty()) {
                    // Release ownership only while holding mutex_ so enqueuers can
                    // reliably schedule a new drain for subsequently queued frames.
                    drainScheduled_.store(false, std::memory_order_relaxed);
                    done = true;
                }
            }
            if (blockedByFlowControl) {
                gTilesWsMetrics.totalFlowBlockedDrains.fetch_add(1, std::memory_order_relaxed);
            }
            if (done) {
                return;
            }
        }
    }

    HttpService& service_;
    std::weak_ptr<drogon::WebSocketConnection> conn_;
    std::weak_ptr<WsConnectionState> connState_;
    uint64_t requestId_;

    AuthHeaders authHeaders_;

    std::mutex mutex_;
    std::deque<OutgoingFrame> outgoing_;

    std::vector<LayerTilesRequest::Ptr> requests_;
    std::vector<RequestStatus> requestStatuses_;
    bool statusEmissionEnabled_ = false;

    TileLayerStream::StringPoolOffsetMap offsets_;
    std::unique_ptr<TileLayerStream::Writer> writer_;
    std::optional<std::vector<WriterMessage>> currentWriteBatch_;

    std::atomic_bool drainScheduled_{false};
    std::atomic_bool cancelled_{false};
};

class TilesWebSocketController final : public drogon::WebSocketController<TilesWebSocketController, false>
{
public:
    explicit TilesWebSocketController(HttpService& service) : service_(service) {}

    void handleNewConnection(const drogon::HttpRequestPtr& req, const drogon::WebSocketConnectionPtr& conn) override
    {
        gTilesWsMetrics.activeConnections.fetch_add(1, std::memory_order_relaxed);
        auto state = std::make_shared<WsConnectionState>();
        state->authHeaders = authHeadersFromRequest(req);
        {
            std::lock_guard lock(gTrackedConnectionsMutex);
            gTrackedConnections.push_back(state);
        }
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

        std::string messageType;
        if (auto typeIt = j.find("type"); typeIt != j.end() && typeIt->is_string()) {
            messageType = typeIt->get<std::string>();
        }

        if (messageType == kFlowGrantType) {
            auto [grantedFrames, grantedBytes] = state->grantFlowCredits(
                parseNonNegativeInt64(j, "frames"),
                parseNonNegativeInt64(j, "bytes"));
            gTilesWsMetrics.totalFlowGrantMessages.fetch_add(1, std::memory_order_relaxed);
            gTilesWsMetrics.totalFlowGrantFrames.fetch_add(grantedFrames, std::memory_order_relaxed);
            gTilesWsMetrics.totalFlowGrantBytes.fetch_add(grantedBytes, std::memory_order_relaxed);
            if (state->session) {
                state->session->onFlowGrant();
            }
            return;
        }

        bool flowControl = false;
        if (auto flowControlIt = j.find("flowControl"); flowControlIt != j.end() && flowControlIt->is_boolean()) {
            flowControl = flowControlIt->get<bool>();
        }
        state->setFlowControlEnabled(flowControl);

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
            gTilesWsMetrics.replacedRequests.fetch_add(1, std::memory_order_relaxed);
            state->session->cancel("Replaced by a new /tiles WebSocket request.");
            state->session.reset();
        }

        uint64_t requestId = state->nextRequestId++;
        if (auto requestIdIt = j.find("requestId");
            requestIdIt != j.end() && (requestIdIt->is_number_integer() || requestIdIt->is_number_unsigned())) {
            const auto parsedRequestId = parseNonNegativeInt64(j, "requestId");
            if (parsedRequestId > 0) {
                requestId = static_cast<uint64_t>(parsedRequestId);
                state->nextRequestId = std::max<uint64_t>(state->nextRequestId, requestId + 1);
            }
        }

        state->session = std::make_shared<TilesWsSession>(
            service_,
            conn,
            state,
            requestId,
            state->authHeaders,
            state->stringPoolOffsets);
        state->session->registerForMetrics();
        state->session->start(j);
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override
    {
        gTilesWsMetrics.activeConnections.fetch_sub(1, std::memory_order_relaxed);
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

nlohmann::json tilesWebSocketMetricsSnapshot()
{
    int64_t pendingControllerFrames = 0;
    int64_t pendingControllerBytes = 0;
    int64_t flowControlEnabledConnections = 0;
    int64_t flowControlBlockedConnections = 0;
    int64_t flowControlCreditFrames = 0;
    int64_t flowControlCreditBytes = 0;
    {
        std::lock_guard lock(gTrackedSessionsMutex);
        auto out = gTrackedSessions.begin();
        for (auto it = gTrackedSessions.begin(); it != gTrackedSessions.end(); ++it) {
            if (auto session = it->lock()) {
                auto [frames, bytes] = session->pendingSnapshot();
                pendingControllerFrames += frames;
                pendingControllerBytes += bytes;
                *out++ = *it;
            }
        }
        gTrackedSessions.erase(out, gTrackedSessions.end());
    }
    {
        std::lock_guard lock(gTrackedConnectionsMutex);
        auto out = gTrackedConnections.begin();
        for (auto it = gTrackedConnections.begin(); it != gTrackedConnections.end(); ++it) {
            if (auto state = it->lock()) {
                const auto snapshot = state->flowControlSnapshot();
                if (snapshot.enabled) {
                    ++flowControlEnabledConnections;
                    flowControlCreditFrames += snapshot.creditFrames;
                    flowControlCreditBytes += snapshot.creditBytes;
                    if (snapshot.creditFrames <= 0 || snapshot.creditBytes <= 0) {
                        ++flowControlBlockedConnections;
                    }
                }
                *out++ = *it;
            }
        }
        gTrackedConnections.erase(out, gTrackedConnections.end());
    }

    return nlohmann::json::object({
        {"active-connections", nonNegative(gTilesWsMetrics.activeConnections)},
        {"active-sessions", nonNegative(gTilesWsMetrics.activeSessions)},
        {"pending-controller-frames", pendingControllerFrames},
        {"pending-controller-bytes", pendingControllerBytes},
        {"flow-control-enabled-connections", flowControlEnabledConnections},
        {"flow-control-blocked-connections", flowControlBlockedConnections},
        {"flow-control-credit-frames", flowControlCreditFrames},
        {"flow-control-credit-bytes", flowControlCreditBytes},
        {"total-queued-frames", nonNegative(gTilesWsMetrics.totalQueuedFrames)},
        {"total-queued-bytes", nonNegative(gTilesWsMetrics.totalQueuedBytes)},
        {"total-forwarded-frames", nonNegative(gTilesWsMetrics.totalForwardedFrames)},
        {"total-forwarded-bytes", nonNegative(gTilesWsMetrics.totalForwardedBytes)},
        {"total-dropped-frames", nonNegative(gTilesWsMetrics.totalDroppedFrames)},
        {"total-dropped-bytes", nonNegative(gTilesWsMetrics.totalDroppedBytes)},
        {"total-drain-calls", nonNegative(gTilesWsMetrics.totalDrainCalls)},
        {"total-flow-grant-messages", nonNegative(gTilesWsMetrics.totalFlowGrantMessages)},
        {"total-flow-grant-frames", nonNegative(gTilesWsMetrics.totalFlowGrantFrames)},
        {"total-flow-grant-bytes", nonNegative(gTilesWsMetrics.totalFlowGrantBytes)},
        {"total-flow-blocked-drains", nonNegative(gTilesWsMetrics.totalFlowBlockedDrains)},
        {"replaced-requests", nonNegative(gTilesWsMetrics.replacedRequests)},
    });
}

}  // namespace mapget::detail
