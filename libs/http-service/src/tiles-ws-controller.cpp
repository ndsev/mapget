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
#include <map>
#include <mutex>
#include <optional>
#include <set>
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
    std::atomic<int64_t> totalFlowBlockedDrains{0};
};

TilesWsMetrics gTilesWsMetrics;
std::mutex gTrackedSessionsMutex;
std::vector<std::weak_ptr<class TilesWsSession>> gTrackedSessions;

constexpr std::string_view FLOW_GRANT_TYPE = "mapget.tiles.flow-grant";
constexpr int64_t FLOW_CREDIT_MAX_FRAMES = 2;
constexpr size_t MAX_FRAMES_PER_DRAIN = 64;
constexpr LayerType REQUEST_TILE_LAYER_TYPE = LayerType::Features;
constexpr int64_t LOWEST_TILE_PRIORITY = std::numeric_limits<int64_t>::max();
constexpr bool EMIT_LOAD_STATE_FRAMES = false;

/// Clamp an atomic metric value to zero to avoid exposing negative snapshots.
[[nodiscard]] int64_t nonNegative(std::atomic<int64_t> const& value)
{
    const auto v = value.load(std::memory_order_relaxed);
    return v < 0 ? 0 : v;
}

/// Copy inbound HTTP headers so backend requests can preserve auth context.
[[nodiscard]] AuthHeaders authHeadersFromRequest(const drogon::HttpRequestPtr& req)
{
    AuthHeaders headers;
    for (auto const& [k, v] : req->headers()) {
        headers.emplace(k, v);
    }
    return headers;
}

/// Convert internal request status enum values to stable UI-facing strings.
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

/// Convert tile load-state enum values to stable UI-facing strings.
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

/// Encode one mapget VTLV frame with protocol header plus payload bytes.
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

/// Parse a JSON numeric field into non-negative int64 while handling missing keys.
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

/// Return true for frame kinds governed by websocket flow-control credits.
[[nodiscard]] bool isFlowControlledDataFrameType(TileLayerStream::MessageType type)
{
    return type == TileLayerStream::MessageType::StringPool
        || type == TileLayerStream::MessageType::TileFeatureLayer
        || type == TileLayerStream::MessageType::TileSourceDataLayer;
}

/// Build a canonical request key using map/layer/tile while normalizing layer type.
[[nodiscard]] MapTileKey makeCanonicalRequestedTileKey(
    std::string_view mapId,
    std::string_view layerId,
    TileId tileId)
{
    return MapTileKey(
        REQUEST_TILE_LAYER_TYPE,
        std::string(mapId),
        std::string(layerId),
        tileId);
}

/// Normalize an existing map tile key so request matching ignores source layer type.
[[nodiscard]] MapTileKey makeCanonicalRequestedTileKey(MapTileKey key)
{
    key.layer_ = REQUEST_TILE_LAYER_TYPE;
    return key;
}

/// Snapshot of flow-control state exposed to `/status-data`.
struct FlowControlStateSnapshot
{
    bool enabled = false;
    int64_t creditFrames = 0;
};

class TilesWsSession : public std::enable_shared_from_this<TilesWsSession>
{
public:
    /// Construct one websocket session object bound 1:1 to a websocket connection.
    TilesWsSession(
        HttpService& service,
        std::weak_ptr<drogon::WebSocketConnection> conn,
        AuthHeaders authHeaders)
        : service_(service),
          conn_(std::move(conn)),
          authHeaders_(std::move(authHeaders)),
          writer_(
              std::make_unique<TileLayerStream::Writer>(
                  [this](std::string msg, TileLayerStream::MessageType type) { onWriterMessage(std::move(msg), type); },
                  writerOffsets_))
    {
        gTilesWsMetrics.activeSessions.fetch_add(1, std::memory_order_relaxed);
    }

    /// Destroy the session and abort any in-flight backend work.
    ~TilesWsSession()
    {
        gTilesWsMetrics.activeSessions.fetch_sub(1, std::memory_order_relaxed);
        // Best-effort cleanup: abort any in-flight requests if the session is destroyed.
        cancelNoStatus();
    }

    TilesWsSession(TilesWsSession const&) = delete;
    TilesWsSession& operator=(TilesWsSession const&) = delete;

    /// Register this session in the global weak list used for `/status-data` snapshots.
    void registerForMetrics()
    {
        std::lock_guard lock(gTrackedSessionsMutex);
        gTrackedSessions.push_back(weak_from_this());
    }

    /// Return currently queued controller frames/bytes.
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

    /// Return flow-control state for `/status-data` metrics.
    [[nodiscard]] FlowControlStateSnapshot flowControlSnapshot() const
    {
        std::lock_guard lock(flowControlMutex_);
        return FlowControlStateSnapshot{
            .enabled = flowControlEnabled_,
            .creditFrames = flowCreditFrames_,
        };
    }

    /// Enable/disable frame-credit flow control for this connection.
    void setFlowControlEnabled(bool enabled)
    {
        std::lock_guard lock(flowControlMutex_);
        if (enabled) {
            if (!flowControlEnabled_) {
                flowControlEnabled_ = true;
                flowCreditFrames_ = FLOW_CREDIT_MAX_FRAMES;
            }
            return;
        }
        flowControlEnabled_ = false;
        flowCreditFrames_ = 0;
    }

    /// Add frame credits granted by the client and return credits actually applied.
    [[nodiscard]] int64_t grantFlowCredits(int64_t frames)
    {
        std::lock_guard lock(flowControlMutex_);
        if (!flowControlEnabled_) {
            return 0;
        }
        const auto safeFrames = std::max<int64_t>(0, frames);
        const auto oldFrames = flowCreditFrames_;
        flowCreditFrames_ = std::min<int64_t>(FLOW_CREDIT_MAX_FRAMES, flowCreditFrames_ + safeFrames);
        return flowCreditFrames_ - oldFrames;
    }

    /// Patch per-connection string-pool offsets supplied by the client request.
    [[nodiscard]] bool applyStringPoolOffsetsPatch(const nlohmann::json& offsetsJson, std::string& errorMessage)
    {
        if (!offsetsJson.is_object()) {
            errorMessage = "stringPoolOffsets must be an object.";
            return false;
        }

        try {
            std::lock_guard lock(mutex_);
            for (auto const& item : offsetsJson.items()) {
                const auto value = item.value().get<simfil::StringId>();
                committedStringPoolOffsets_[item.key()] = value;
                writerOffsets_[item.key()] = value;
            }
            return true;
        }
        catch (const std::exception& e) {
            errorMessage = fmt::format("Invalid stringPoolOffsets: {}", e.what());
            return false;
        }
    }

    /// Allocate a request id while respecting optional client-provided request ids.
    [[nodiscard]] uint64_t allocateRequestId(const nlohmann::json& requestJson)
    {
        uint64_t requestId = nextRequestId_++;
        if (auto requestIdIt = requestJson.find("requestId");
            requestIdIt != requestJson.end()
            && (requestIdIt->is_number_integer() || requestIdIt->is_number_unsigned())) {
            const auto parsedRequestId = parseNonNegativeInt64(requestJson, "requestId");
            if (parsedRequestId > 0) {
                requestId = static_cast<uint64_t>(parsedRequestId);
                nextRequestId_ = std::max<uint64_t>(nextRequestId_, requestId + 1);
            }
        }
        return requestId;
    }

    /// Consume granted sent-frame slots and restart draining.
    void onFlowGrant(int64_t grantedFrames)
    {
        if (grantedFrames > 0) {
            consumeSentFlowFrames(grantedFrames);
        }
        scheduleDrain();
    }

    /// Parse and apply a full logical tile request update from the client.
    void updateFromClientRequest(const nlohmann::json& j, uint64_t requestId)
    {
        auto requestsIt = j.find("requests");
        if (requestsIt == j.end() || !requestsIt->is_array()) {
            // Invalid request payload: publish an immediate status error for observability.
            {
                std::lock_guard lock(mutex_);
                requestId_ = requestId;
                requestInfos_.clear();
                requestStatuses_.clear();
                statusEmissionEnabled_ = true;
            }
            queueRequestContextMessage();
            queueStatusMessage("Missing or invalid 'requests' array");
            scheduleDrain();
            return;
        }

        struct ParsedRequest
        {
            std::string mapId;
            std::string layerId;
            std::vector<TileId> tileIds;
        };
        std::vector<ParsedRequest> parsedRequests;
        std::set<MapTileKey> desiredTileKeys;
        std::map<MapTileKey, int64_t> nextTilePriorityRanks;
        int64_t nextPriorityRank = 0;

        try {
            parsedRequests.reserve(requestsIt->size());
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
                    const auto tileId = TileId{tid.get<uint64_t>()};
                    tileIds.emplace_back(tileId);
                    const auto tileKey = makeCanonicalRequestedTileKey(mapId, layerId, tileId);
                    desiredTileKeys.insert(tileKey);
                    if (nextTilePriorityRanks.find(tileKey) == nextTilePriorityRanks.end()) {
                        nextTilePriorityRanks.emplace(tileKey, nextPriorityRank++);
                    }
                }

                parsedRequests.push_back(ParsedRequest{
                    .mapId = mapId,
                    .layerId = layerId,
                    .tileIds = std::move(tileIds),
                });
            }
        }
        catch (const std::exception& e) {
            {
                std::lock_guard lock(mutex_);
                requestId_ = requestId;
                requestInfos_.clear();
                requestStatuses_.clear();
                statusEmissionEnabled_ = true;
            }
            queueRequestContextMessage();
            queueStatusMessage(fmt::format("Invalid request JSON: {}", e.what()));
            scheduleDrain();
            return;
        }

        std::vector<LayerTilesRequest::Ptr> serviceRequests;
        std::vector<RequestStatus> nextRequestStatuses(parsedRequests.size(), RequestStatus::Success);
        std::vector<RequestInfo> nextRequestInfos;
        nextRequestInfos.reserve(parsedRequests.size());

        for (size_t index = 0; index < parsedRequests.size(); ++index) {
            auto& parsed = parsedRequests[index];
            nextRequestInfos.push_back(RequestInfo{
                .mapId = parsed.mapId,
                .layerId = parsed.layerId,
            });

            std::vector<TileId> tileIdsToFetch;
            tileIdsToFetch.reserve(parsed.tileIds.size());
            {
                std::lock_guard lock(mutex_);
                for (const auto& tileId : parsed.tileIds) {
                    const auto requestedTileKey = makeCanonicalRequestedTileKey(parsed.mapId, parsed.layerId, tileId);
                    const bool alreadyQueued =
                        queuedTileFrameRefCount_.find(requestedTileKey) != queuedTileFrameRefCount_.end();
                    const bool alreadySentNotGranted =
                        sentTileFrameRefCount_.find(requestedTileKey) != sentTileFrameRefCount_.end();
                    // Skip backend fetches for tiles already queued or already sent but not yet granted.
                    if (!alreadyQueued && !alreadySentNotGranted) {
                        tileIdsToFetch.push_back(tileId);
                    }
                }
            }
            if (tileIdsToFetch.empty()) {
                continue;
            }

            auto request = std::make_shared<LayerTilesRequest>(
                parsed.mapId,
                parsed.layerId,
                std::move(tileIdsToFetch));
            serviceRequests.push_back(request);
            {
                std::lock_guard lock(mutex_);
                activeRequests_.push_back(request);
            }
            nextRequestStatuses[index] = RequestStatus::Open;

            const auto weak = weak_from_this();
            const auto expectedRequestId = requestId;
            request->onFeatureLayer([weak](auto&& layer) {
                if (auto self = weak.lock()) {
                    self->onTileLayer(std::forward<decltype(layer)>(layer));
                }
            });
            request->onSourceDataLayer([weak](auto&& layer) {
                if (auto self = weak.lock()) {
                    self->onTileLayer(std::forward<decltype(layer)>(layer));
                }
            });
            if (EMIT_LOAD_STATE_FRAMES) {
                request->onLayerLoadStateChanged([weak](MapTileKey const& key, TileLayer::LoadState state) {
                    if (auto self = weak.lock()) {
                        self->onLoadStateChanged(key, state);
                    }
                });
            }
            request->onDone_ = [weak, index, expectedRequestId, request](RequestStatus status) {
                if (auto self = weak.lock()) {
                    self->onRequestDone(index, expectedRequestId, request, status);
                }
            };
        }

        {
            std::lock_guard lock(mutex_);
            requestId_ = requestId;
            requestInfos_ = std::move(nextRequestInfos);
            requestStatuses_ = std::move(nextRequestStatuses);
            desiredTileKeys_ = std::move(desiredTileKeys);
            tilePriorityRanks_ = std::move(nextTilePriorityRanks);
            // When request scope shrinks, remove stale tile data already queued for send.
            filterOutgoingByDesiredLocked();
            // Refresh ordering so queued tiles follow the latest request priority.
            reprioritizeOutgoingLocked();
            statusEmissionEnabled_ = true;
        }

        queueRequestContextMessage();
        if (!serviceRequests.empty()) {
            (void)service_.request(serviceRequests, authHeaders_);
        }
        queueStatusMessage({});
        scheduleDrain();
    }

    /// Cancel current requests, clear queued frames, and emit a terminal status.
    void cancel(std::string reason)
    {
        cancelled_ = true;

        // Stop sending any queued tile frames from this session.
        {
            std::lock_guard lock(mutex_);
            clearOutgoingLocked();
        }

        // Abort in-flight requests (best-effort).
        for (auto const& r : activeRequests_) {
            if (!r || r->isDone())
                continue;
            service_.abort(r);
        }
        activeRequests_.clear();

        // Refresh locally cached statuses after aborting.
        {
            std::lock_guard lock(mutex_);
            for (auto& status : requestStatuses_) {
                if (status == RequestStatus::Open) {
                    status = RequestStatus::Aborted;
                }
            }
        }

        queueStatusMessage(std::move(reason));
        scheduleDrain();
    }

private:
    /// Consume exactly one frame credit before sending a flow-controlled frame.
    [[nodiscard]] bool consumeFlowCreditForFrame()
    {
        std::lock_guard lock(flowControlMutex_);
        if (!flowControlEnabled_) {
            return true;
        }
        if (flowCreditFrames_ <= 0) {
            return false;
        }
        flowCreditFrames_ -= 1;
        return true;
    }

    /// Lightweight metadata emitted in status payloads for each logical request.
    struct RequestInfo
    {
        std::string mapId;
        std::string layerId;
    };

    /// One queued websocket frame plus metadata used for bookkeeping.
    struct OutgoingFrame
    {
        std::string bytes;
        TileLayerStream::MessageType type{TileLayerStream::MessageType::None};
        std::optional<std::pair<std::string, simfil::StringId>> stringPoolCommit;
        std::optional<MapTileKey> requestedTileKey;
        int64_t priorityRank = LOWEST_TILE_PRIORITY;
    };

    /// Batched writer output captured while serializing one tile layer.
    struct WriterMessage
    {
        std::string bytes;
        TileLayerStream::MessageType type{TileLayerStream::MessageType::None};
    };

    /// Increment queued/sent reference counters for one canonical tile key.
    void incrementFrameRefCount(std::map<MapTileKey, int64_t>& counts, const MapTileKey& key)
    {
        counts[key] += 1;
    }

    /// Decrement queued/sent reference counters and erase exhausted entries.
    void decrementFrameRefCount(std::map<MapTileKey, int64_t>& counts, const MapTileKey& key)
    {
        auto it = counts.find(key);
        if (it == counts.end()) {
            return;
        }
        it->second -= 1;
        if (it->second <= 0) {
            counts.erase(it);
        }
    }

    /// Mark a frame as queued so request updates can avoid duplicate backend fetches.
    void trackQueuedFrameLocked(const OutgoingFrame& frame)
    {
        if (frame.requestedTileKey) {
            incrementFrameRefCount(queuedTileFrameRefCount_, *frame.requestedTileKey);
        }
    }

    /// Remove a frame from queued bookkeeping once it is dequeued or dropped.
    void untrackQueuedFrameLocked(const OutgoingFrame& frame)
    {
        if (frame.requestedTileKey) {
            decrementFrameRefCount(queuedTileFrameRefCount_, *frame.requestedTileKey);
        }
    }

    /// Track flow-controlled frames that were sent but not yet granted back by the client.
    void trackSentFrameLocked(const OutgoingFrame& frame)
    {
        sentFlowFrames_.push_back(frame.requestedTileKey);
        if (frame.requestedTileKey) {
            incrementFrameRefCount(sentTileFrameRefCount_, *frame.requestedTileKey);
        }
    }

    /// Apply client grants to the sent-frame ledger to release in-flight dedupe entries.
    void consumeSentFlowFrames(int64_t grantedFrames)
    {
        std::lock_guard lock(mutex_);
        for (int64_t i = 0; i < grantedFrames && !sentFlowFrames_.empty(); ++i) {
            auto key = std::move(sentFlowFrames_.front());
            sentFlowFrames_.pop_front();
            if (key) {
                decrementFrameRefCount(sentTileFrameRefCount_, *key);
            }
        }
    }

    /// Look up the current priority rank for one tile key, defaulting to lowest priority.
    [[nodiscard]] int64_t tilePriorityRankLocked(const MapTileKey& tileKey) const
    {
        const auto it = tilePriorityRanks_.find(tileKey);
        if (it == tilePriorityRanks_.end()) {
            return LOWEST_TILE_PRIORITY;
        }
        return it->second;
    }

    /// Refresh one queued frame's cached priority rank against the latest request priorities.
    void refreshFramePriorityLocked(OutgoingFrame& frame) const
    {
        if (!frame.requestedTileKey) {
            frame.priorityRank = LOWEST_TILE_PRIORITY;
            return;
        }
        frame.priorityRank = tilePriorityRankLocked(*frame.requestedTileKey);
    }

    /// Compare two frames for queue order; returns true if lhs should be sent before rhs.
    [[nodiscard]] static bool framePrecedes(const OutgoingFrame& lhs, const OutgoingFrame& rhs)
    {
        const bool lhsIsStringPool = lhs.type == TileLayerStream::MessageType::StringPool;
        const bool rhsIsStringPool = rhs.type == TileLayerStream::MessageType::StringPool;
        if (lhsIsStringPool != rhsIsStringPool) {
            // String pool updates always outrank everything else.
            return lhsIsStringPool;
        }

        const bool lhsHasTile = lhs.requestedTileKey.has_value();
        const bool rhsHasTile = rhs.requestedTileKey.has_value();
        if (lhsHasTile != rhsHasTile) {
            // Keep non-tile control frames ahead of regular tile data frames.
            return !lhsHasTile;
        }
        if (!lhsHasTile) {
            return false;
        }
        return lhs.priorityRank < rhs.priorityRank;
    }

    /// Drop queued tile data frames that no longer belong to the latest request set.
    void filterOutgoingByDesiredLocked()
    {
        if (outgoing_.empty()) {
            return;
        }

        int64_t droppedFrames = 0;
        int64_t droppedBytes = 0;
        std::deque<OutgoingFrame> filtered;
        for (auto& frame : outgoing_) {
            const bool dropLoadStateFrame = !EMIT_LOAD_STATE_FRAMES
                && frame.type == TileLayerStream::MessageType::LoadStateChange;
            const bool dropStaleTileFrame = frame.requestedTileKey
                && desiredTileKeys_.find(*frame.requestedTileKey) == desiredTileKeys_.end();
            const bool dropFrame = dropLoadStateFrame || dropStaleTileFrame;
            if (dropFrame) {
                ++droppedFrames;
                droppedBytes += static_cast<int64_t>(frame.bytes.size());
                untrackQueuedFrameLocked(frame);
                continue;
            }
            filtered.push_back(std::move(frame));
        }
        outgoing_ = std::move(filtered);
        if (droppedFrames > 0) {
            gTilesWsMetrics.totalDroppedFrames.fetch_add(droppedFrames, std::memory_order_relaxed);
            gTilesWsMetrics.totalDroppedBytes.fetch_add(droppedBytes, std::memory_order_relaxed);
        }
    }

    /// Reorder queued frames according to string-pool and tile-priority policy.
    void reprioritizeOutgoingLocked()
    {
        if (outgoing_.size() < 2) {
            return;
        }

        for (auto& frame : outgoing_) {
            refreshFramePriorityLocked(frame);
        }

        std::vector<OutgoingFrame> reordered;
        reordered.reserve(outgoing_.size());
        for (auto& frame : outgoing_) {
            reordered.push_back(std::move(frame));
        }

        std::stable_sort(
            reordered.begin(),
            reordered.end(),
            [](const OutgoingFrame& lhs, const OutgoingFrame& rhs) { return framePrecedes(lhs, rhs); });

        outgoing_.clear();
        for (auto& frame : reordered) {
            outgoing_.push_back(std::move(frame));
        }
    }

    /// Append one frame to the websocket controller queue and update counters.
    void enqueueOutgoingLocked(OutgoingFrame&& frame)
    {
        refreshFramePriorityLocked(frame);
        trackQueuedFrameLocked(frame);
        const auto bytes = static_cast<int64_t>(frame.bytes.size());
        auto insertIt = outgoing_.end();
        for (auto it = outgoing_.begin(); it != outgoing_.end(); ++it) {
            if (framePrecedes(frame, *it)) {
                insertIt = it;
                break;
            }
        }
        outgoing_.insert(insertIt, std::move(frame));
        gTilesWsMetrics.totalQueuedFrames.fetch_add(1, std::memory_order_relaxed);
        gTilesWsMetrics.totalQueuedBytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    /// Drop all queued frames and account them as controller-side drops.
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
            untrackQueuedFrameLocked(frame);
        }
        outgoing_.clear();

        gTilesWsMetrics.totalDroppedFrames.fetch_add(droppedFrames, std::memory_order_relaxed);
        gTilesWsMetrics.totalDroppedBytes.fetch_add(droppedBytes, std::memory_order_relaxed);
    }

    /// Internal cancel path used by destructor/connection tear-down (no status emission).
    void cancelNoStatus()
    {
        if (cancelled_.exchange(true))
            return;

        // Ensure we stop emitting any further frames.
        {
            std::lock_guard lock(mutex_);
            clearOutgoingLocked();
        }

        for (auto const& r : activeRequests_) {
            if (!r || r->isDone())
                continue;
            service_.abort(r);
        }
        activeRequests_.clear();
    }

    /// Collect writer callbacks generated while serializing one tile layer.
    void onWriterMessage(std::string msg, TileLayerStream::MessageType type)
    {
        // Writer messages are only generated from within onTileLayer under mutex_.
        if (!currentWriteBatch_.has_value()) {
            raise("TilesWsSession writer callback used out-of-band");
        }
        currentWriteBatch_->push_back(WriterMessage{std::move(msg), type});
    }

    /// Convert one backend tile layer into outgoing websocket frames.
    void onTileLayer(TileLayer::Ptr const& layer)
    {
        if (cancelled_)
            return;
        if (!layer)
            return;

        const auto requestedTileKey = makeCanonicalRequestedTileKey(layer->id());
        std::optional<std::pair<std::string, simfil::StringId>> stringPoolCommit;

        {
            std::lock_guard lock(mutex_);
            if (cancelled_)
                return;
            // Late-arriving tile for an outdated request: drop before serialization work.
            if (desiredTileKeys_.find(requestedTileKey) == desiredTileKeys_.end()) {
                return;
            }

            if (currentWriteBatch_.has_value()) {
                raise("TilesWsSession writer callback re-entered");
            }
            currentWriteBatch_.emplace();
            writer_->write(layer);
            auto batch = std::move(*currentWriteBatch_);
            currentWriteBatch_.reset();

            // If a StringPool message was generated, the writer updates writerOffsets_
            // to the new highest string ID for this node after emitting it.
            const auto nodeId = layer->nodeId();
            const auto it = writerOffsets_.find(nodeId);
            if (it != writerOffsets_.end()) {
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
                    frame.requestedTileKey = requestedTileKey;
                }
                if (m.type == TileLayerStream::MessageType::TileFeatureLayer
                    || m.type == TileLayerStream::MessageType::TileSourceDataLayer) {
                    frame.requestedTileKey = requestedTileKey;
                }
                enqueueOutgoingLocked(std::move(frame));
            }
        }

        scheduleDrain();
    }

    /// Update per-request completion state and emit status when it changes.
    void onRequestDone(
        size_t requestIndex,
        uint64_t expectedRequestId,
        const LayerTilesRequest::Ptr& completedRequest,
        RequestStatus status)
    {
        if (cancelled_)
            return;

        bool shouldEmit = false;
        {
            std::lock_guard lock(mutex_);
            if (cancelled_)
                return;
            activeRequests_.erase(
                std::remove_if(
                    activeRequests_.begin(),
                    activeRequests_.end(),
                    [&](const LayerTilesRequest::Ptr& req) {
                        return !req || req == completedRequest || req->isDone();
                    }),
                activeRequests_.end());
            if (expectedRequestId == requestId_ && requestIndex < requestStatuses_.size()) {
                if (requestStatuses_[requestIndex] == status) {
                    return;
                }
                requestStatuses_[requestIndex] = status;
                shouldEmit = statusEmissionEnabled_;
            }
        }

        if (shouldEmit) {
            queueStatusMessage({});
            scheduleDrain();
        }
    }

    /// Queue a status frame describing the current request statuses.
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

    /// Queue a request-context frame so the client can track the active request id.
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

    /// Forward backend tile load-state changes for tiles still requested by the client.
    void onLoadStateChanged(MapTileKey const& key, TileLayer::LoadState state)
    {
        if (!EMIT_LOAD_STATE_FRAMES) {
            return;
        }
        if (cancelled_)
            return;
        const auto requestedTileKey = makeCanonicalRequestedTileKey(key);
        {
            std::lock_guard lock(mutex_);
            // Keep load-state traffic scoped to the currently requested tile set.
            if (desiredTileKeys_.find(requestedTileKey) == desiredTileKeys_.end()) {
                return;
            }
        }

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

    /// Build the JSON payload for `mapget.tiles.status`.
    [[nodiscard]] std::string buildStatusPayload(std::string message)
    {
        nlohmann::json requestsJson = nlohmann::json::array();
        bool allDone = true;

        {
            std::lock_guard lock(mutex_);
            for (size_t i = 0; i < requestInfos_.size(); ++i) {
                const auto status = (i < requestStatuses_.size()) ? requestStatuses_[i] : RequestStatus::Open;
                allDone &= (status != RequestStatus::Open);

                nlohmann::json reqJson = nlohmann::json::object();
                reqJson["index"] = i;
                reqJson["mapId"] = requestInfos_[i].mapId;
                reqJson["layerId"] = requestInfos_[i].layerId;
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

    /// Build the JSON payload for `mapget.tiles.load-state`.
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

    /// Build the JSON payload for `mapget.tiles.request-context`.
    [[nodiscard]] std::string buildRequestContextPayload() const
    {
        return nlohmann::json::object({
            {"type", "mapget.tiles.request-context"},
            {"requestId", requestId_},
        }).dump();
    }

    /// Schedule queue draining while guaranteeing at most one active drainer.
    void scheduleDrain()
    {
        if (drainScheduled_.exchange(true))
            return;
        drainNow();
    }

    /// Drain queued frames to Drogon while respecting flow-control credits.
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

            bool blockedByFlowControl = false;

            for (size_t i = 0; i < MAX_FRAMES_PER_DRAIN; ++i) {
                OutgoingFrame frame;
                {
                    std::lock_guard lock(mutex_);
                    if (outgoing_.empty()) {
                        break;
                    }
                    frame = std::move(outgoing_.front());
                    outgoing_.pop_front();
                    untrackQueuedFrameLocked(frame);
                }

                const auto frameBytes = static_cast<int64_t>(frame.bytes.size());

                if (cancelled_) {
                    gTilesWsMetrics.totalDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                    gTilesWsMetrics.totalDroppedBytes.fetch_add(frameBytes, std::memory_order_relaxed);
                    continue;
                }

                if (isFlowControlledDataFrameType(frame.type)) {
                    // No credits available: put frame back at the front and stop this drain pass.
                    if (!consumeFlowCreditForFrame()) {
                        std::lock_guard lock(mutex_);
                        outgoing_.push_front(std::move(frame));
                        trackQueuedFrameLocked(outgoing_.front());
                        blockedByFlowControl = true;
                        break;
                    }
                    std::lock_guard lock(mutex_);
                    trackSentFrameLocked(frame);
                }

                gTilesWsMetrics.totalForwardedFrames.fetch_add(1, std::memory_order_relaxed);
                gTilesWsMetrics.totalForwardedBytes.fetch_add(frameBytes, std::memory_order_relaxed);
                conn->send(frame.bytes, drogon::WebSocketMessageType::Binary);
                if (frame.stringPoolCommit) {
                    std::lock_guard lock(mutex_);
                    committedStringPoolOffsets_[frame.stringPoolCommit->first] = frame.stringPoolCommit->second;
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
    uint64_t requestId_ = 0;
    uint64_t nextRequestId_ = 1;

    AuthHeaders authHeaders_;

    mutable std::mutex flowControlMutex_;
    bool flowControlEnabled_ = false;
    int64_t flowCreditFrames_ = 0;

    std::mutex mutex_;
    std::deque<OutgoingFrame> outgoing_;
    std::vector<RequestInfo> requestInfos_;
    std::vector<RequestStatus> requestStatuses_;
    std::vector<LayerTilesRequest::Ptr> activeRequests_;
    std::set<MapTileKey> desiredTileKeys_;
    std::map<MapTileKey, int64_t> tilePriorityRanks_;
    std::map<MapTileKey, int64_t> queuedTileFrameRefCount_;
    std::map<MapTileKey, int64_t> sentTileFrameRefCount_;
    std::deque<std::optional<MapTileKey>> sentFlowFrames_;
    bool statusEmissionEnabled_ = false;

    TileLayerStream::StringPoolOffsetMap committedStringPoolOffsets_;
    TileLayerStream::StringPoolOffsetMap writerOffsets_;
    std::unique_ptr<TileLayerStream::Writer> writer_;
    std::optional<std::vector<WriterMessage>> currentWriteBatch_;

    std::atomic_bool drainScheduled_{false};
    std::atomic_bool cancelled_{false};
};

class TilesWebSocketController final : public drogon::WebSocketController<TilesWebSocketController, false>
{
public:
    /// Build the websocket controller bound to one shared HttpService instance.
    explicit TilesWebSocketController(HttpService& service) : service_(service) {}

    /// Create and attach one `TilesWsSession` per accepted websocket connection.
    void handleNewConnection(const drogon::HttpRequestPtr& req, const drogon::WebSocketConnectionPtr& conn) override
    {
        gTilesWsMetrics.activeConnections.fetch_add(1, std::memory_order_relaxed);
        auto session = std::make_shared<TilesWsSession>(service_, conn, authHeadersFromRequest(req));
        session->registerForMetrics();
        conn->setContext(std::move(session));
    }

    /// Handle control and request messages from the websocket client.
    void handleNewMessage(
        const drogon::WebSocketConnectionPtr& conn,
        std::string&& message,
        const drogon::WebSocketMessageType& type) override
    {
        auto session = conn->getContext<TilesWsSession>();
        if (!session) {
            // This is a defensive fallback for unexpected context loss.
            session = std::make_shared<TilesWsSession>(service_, conn, AuthHeaders{});
            session->registerForMetrics();
            conn->setContext(session);
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

        if (messageType == FLOW_GRANT_TYPE) {
            const auto grantedFrames = session->grantFlowCredits(parseNonNegativeInt64(j, "frames"));
            gTilesWsMetrics.totalFlowGrantMessages.fetch_add(1, std::memory_order_relaxed);
            gTilesWsMetrics.totalFlowGrantFrames.fetch_add(grantedFrames, std::memory_order_relaxed);
            session->onFlowGrant(grantedFrames);
            return;
        }

        bool flowControl = false;
        if (auto flowControlIt = j.find("flowControl"); flowControlIt != j.end() && flowControlIt->is_boolean()) {
            flowControl = flowControlIt->get<bool>();
        }
        session->setFlowControlEnabled(flowControl);

        // Patch per-connection string pool offsets if supplied.
        if (j.contains("stringPoolOffsets")) {
            std::string errorMessage;
            if (!session->applyStringPoolOffsetsPatch(j["stringPoolOffsets"], errorMessage)) {
                const auto payload = nlohmann::json::object({
                    {"type", "mapget.tiles.status"},
                    {"allDone", true},
                    {"requests", nlohmann::json::array()},
                    {"message", std::move(errorMessage)},
                }).dump();
                conn->send(encodeStreamMessage(TileLayerStream::MessageType::Status, payload), drogon::WebSocketMessageType::Binary);
                return;
            }
        }

        const auto requestId = session->allocateRequestId(j);
        session->updateFromClientRequest(j, requestId);
    }

    /// Abort outstanding backend work once the websocket is closed.
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override
    {
        gTilesWsMetrics.activeConnections.fetch_sub(1, std::memory_order_relaxed);
        if (auto session = conn->getContext<TilesWsSession>()) {
            session->cancel("WebSocket connection closed.");
        }
    }

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/tiles", drogon::Get);
    WS_PATH_LIST_END

private:
    HttpService& service_;
};

}  // namespace

/// Register the `/tiles` websocket controller with Drogon.
void registerTilesWebSocketController(drogon::HttpAppFramework& app, HttpService& service)
{
    app.registerController(std::make_shared<TilesWebSocketController>(service));
}

/// Build the websocket metrics payload consumed by `/status-data`.
nlohmann::json tilesWebSocketMetricsSnapshot()
{
    int64_t pendingControllerFrames = 0;
    int64_t pendingControllerBytes = 0;
    int64_t flowControlEnabledConnections = 0;
    int64_t flowControlBlockedConnections = 0;
    int64_t flowControlCreditFrames = 0;
    {
        std::lock_guard lock(gTrackedSessionsMutex);
        auto out = gTrackedSessions.begin();
        for (auto it = gTrackedSessions.begin(); it != gTrackedSessions.end(); ++it) {
            if (auto session = it->lock()) {
                auto [frames, bytes] = session->pendingSnapshot();
                const auto flowSnapshot = session->flowControlSnapshot();
                pendingControllerFrames += frames;
                pendingControllerBytes += bytes;
                if (flowSnapshot.enabled) {
                    ++flowControlEnabledConnections;
                    flowControlCreditFrames += flowSnapshot.creditFrames;
                    if (flowSnapshot.creditFrames <= 0) {
                        ++flowControlBlockedConnections;
                    }
                }
                *out++ = *it;
            }
        }
        gTrackedSessions.erase(out, gTrackedSessions.end());
    }

    return nlohmann::json::object({
        {"active-connections", nonNegative(gTilesWsMetrics.activeConnections)},
        {"active-sessions", nonNegative(gTilesWsMetrics.activeSessions)},
        {"pending-controller-frames", pendingControllerFrames},
        {"pending-controller-bytes", pendingControllerBytes},
        {"flow-control-enabled-connections", flowControlEnabledConnections},
        {"flow-control-blocked-connections", flowControlBlockedConnections},
        {"flow-control-credit-frames", flowControlCreditFrames},
        {"total-queued-frames", nonNegative(gTilesWsMetrics.totalQueuedFrames)},
        {"total-queued-bytes", nonNegative(gTilesWsMetrics.totalQueuedBytes)},
        {"total-forwarded-frames", nonNegative(gTilesWsMetrics.totalForwardedFrames)},
        {"total-forwarded-bytes", nonNegative(gTilesWsMetrics.totalForwardedBytes)},
        {"total-dropped-frames", nonNegative(gTilesWsMetrics.totalDroppedFrames)},
        {"total-dropped-bytes", nonNegative(gTilesWsMetrics.totalDroppedBytes)},
        {"total-drain-calls", nonNegative(gTilesWsMetrics.totalDrainCalls)},
        {"total-flow-grant-messages", nonNegative(gTilesWsMetrics.totalFlowGrantMessages)},
        {"total-flow-grant-frames", nonNegative(gTilesWsMetrics.totalFlowGrantFrames)},
        {"total-flow-blocked-drains", nonNegative(gTilesWsMetrics.totalFlowBlockedDrains)},
        {"replaced-requests", nonNegative(gTilesWsMetrics.replacedRequests)},
    });
}

}  // namespace mapget::detail
