#include "tiles-ws-controller.h"

#include "mapget/http-service/http-service.h"
#include "tiles-request-json.h"

#include "mapget/log.h"
#include "mapget/model/stream.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/WebSocketConnection.h>
#include <drogon/WebSocketController.h>

#include <bitsery/adapter/stream.h>
#include <bitsery/bitsery.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fmt/format.h"
#include "nlohmann/json.hpp"

#include <zlib.h>

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
    std::atomic<int64_t> replacedRequests{0};
    std::atomic<int64_t> totalPullRequests{0};
    std::atomic<int64_t> totalPullTimeouts{0};
    std::atomic<int64_t> totalPullSessionMisses{0};
};

TilesWsMetrics gTilesWsMetrics;
std::mutex gTrackedSessionsMutex;
std::vector<std::weak_ptr<class TilesWsSession>> gTrackedSessions;
std::mutex gSessionRegistryMutex;
std::unordered_map<int64_t, std::weak_ptr<class TilesWsSession>> gSessionRegistry;
std::atomic<int64_t> gNextClientId{1};

constexpr int64_t DEFAULT_PULL_WAIT_MS = 25000;
constexpr int64_t MAX_PULL_WAIT_MS = 30000;
constexpr int64_t MAX_PULL_BATCH_BYTES = 64 * 1024 * 1024;
constexpr LayerType REQUEST_TILE_LAYER_TYPE = LayerType::Features;
constexpr int64_t LOWEST_TILE_PRIORITY = std::numeric_limits<int64_t>::max();
constexpr bool EMIT_LOAD_STATE_FRAMES = false;

struct ClientRequestChunk
{
    bool chunked = false;
    uint64_t index = 0;
    bool isLast = true;
};

enum class ClientRequestUpdateMode
{
    Replace,
    Append,
};

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

/// Check whether the HTTP Accept-Encoding header allows gzip responses.
[[nodiscard]] bool containsGzip(std::string_view acceptEncoding)
{
    return !acceptEncoding.empty() && acceptEncoding.find("gzip") != std::string_view::npos;
}

/// Compress one payload as gzip (RFC 1952). Returns nullopt on zlib failure.
[[nodiscard]] std::optional<std::string> gzipCompress(std::string_view input)
{
    z_stream stream{};
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;

    // 16 + MAX_WBITS enables gzip framing instead of raw deflate.
    const int initResult = deflateInit2(
        &stream,
        Z_DEFAULT_COMPRESSION,
        Z_DEFLATED,
        16 + MAX_WBITS,
        8,
        Z_DEFAULT_STRATEGY);
    if (initResult != Z_OK) {
        return std::nullopt;
    }

    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());

    std::string compressed;
    compressed.reserve(input.size() / 2 + 128);

    char outBuffer[8192];
    int deflateResult = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(outBuffer);
        stream.avail_out = sizeof(outBuffer);
        deflateResult = deflate(&stream, Z_FINISH);
        if (deflateResult != Z_OK && deflateResult != Z_STREAM_END) {
            deflateEnd(&stream);
            return std::nullopt;
        }
        compressed.append(outBuffer, sizeof(outBuffer) - stream.avail_out);
    } while (deflateResult != Z_STREAM_END);

    deflateEnd(&stream);
    return compressed;
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

[[nodiscard]] ClientRequestChunk parseClientRequestChunk(const nlohmann::json& j)
{
    auto chunkIt = j.find("chunk");
    if (chunkIt == j.end()) {
        return {};
    }
    if (!chunkIt->is_object()) {
        throw std::runtime_error("chunk must be an object");
    }

    auto indexIt = chunkIt->find("index");
    if (indexIt == chunkIt->end()
        || !(indexIt->is_number_integer() || indexIt->is_number_unsigned())) {
        throw std::runtime_error("chunk.index must be a non-negative integer");
    }
    if (indexIt->is_number_integer() && indexIt->get<int64_t>() < 0) {
        throw std::runtime_error("chunk.index must be a non-negative integer");
    }

    auto isLastIt = chunkIt->find("isLast");
    if (isLastIt == chunkIt->end() || !isLastIt->is_boolean()) {
        throw std::runtime_error("chunk.isLast must be a boolean");
    }

    return ClientRequestChunk{
        .chunked = true,
        .index = static_cast<uint64_t>(parseNonNegativeInt64(*chunkIt, "index")),
        .isLast = isLastIt->get<bool>(),
    };
}

/// Build a canonical request key using map/layer/tile while normalizing layer type.
[[nodiscard]] MapTileKey makeCanonicalRequestedTileKey(
    std::string_view mapId,
    std::string_view layerId,
    TileId tileId,
    uint32_t stage = 0)
{
    return MapTileKey(
        REQUEST_TILE_LAYER_TYPE,
        std::string(mapId),
        std::string(layerId),
        tileId,
        stage);
}

/// Normalize an existing map tile key so request matching ignores source layer type.
[[nodiscard]] MapTileKey makeCanonicalRequestedTileKey(MapTileKey key)
{
    key.layer_ = REQUEST_TILE_LAYER_TYPE;
    return key;
}

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
        {
            std::lock_guard lock(gSessionRegistryMutex);
            gSessionRegistry.erase(clientId_);
        }
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

    /// Return numeric client id used by `/tiles/next` pull requests.
    [[nodiscard]] int64_t clientId() const
    {
        return clientId_;
    }

    /// Return current number of blocked `/tiles/next` long-poll requests.
    [[nodiscard]] int64_t pendingPullRequestCount() const
    {
        std::lock_guard lock(mutex_);
        return static_cast<int64_t>(pendingPullWaiters_.size());
    }

    struct PullFrameResult
    {
        enum class Status {
            Frame,
            Timeout,
            Closed,
        };

        Status status = Status::Timeout;
        std::string frameBytes;
    };

    using PullResultCallback = std::function<void(PullFrameResult)>;

    /// Resolve one `/tiles/next` request immediately or register an async waiter.
    void requestNextTileFrameAsync(
        std::chrono::milliseconds waitTimeout,
        size_t maxBatchBytes,
        PullResultCallback callback)
    {
        PullResultCallback immediateCallback;
        std::optional<PullFrameResult> immediateResult;
        uint64_t timeoutWaiterId = 0;
        double timeoutSeconds = 0.0;

        {
            std::lock_guard lock(mutex_);
            if (cancelled_) {
                immediateCallback = std::move(callback);
                immediateResult = PullFrameResult{.status = PullFrameResult::Status::Closed};
            }
            else if (!outgoing_.empty()) {
                immediateCallback = std::move(callback);
                immediateResult = popFrameBatchLocked(maxBatchBytes);
            }
            else if (waitTimeout.count() <= 0) {
                immediateCallback = std::move(callback);
                immediateResult = PullFrameResult{.status = PullFrameResult::Status::Timeout};
            }
            else {
                timeoutWaiterId = nextPullWaiterId_++;
                pendingPullWaiterOrder_.push_back(timeoutWaiterId);
                pendingPullWaiters_.emplace(timeoutWaiterId, PullWaiter{
                    .waiterId = timeoutWaiterId,
                    .maxBatchBytes = maxBatchBytes,
                    .callback = std::move(callback),
                });
                timeoutSeconds = std::chrono::duration<double>(waitTimeout).count();
            }
        }

        if (immediateResult) {
            dispatchPullResult(std::move(immediateCallback), std::move(*immediateResult));
            return;
        }

        if (timeoutWaiterId == 0) {
            return;
        }
        const auto weak = weak_from_this();
        drogon::app().getLoop()->runAfter(timeoutSeconds, [weak, timeoutWaiterId]() {
            if (auto self = weak.lock()) {
                self->onPullWaiterTimeout(timeoutWaiterId);
            }
        });
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

    /// Parse a possibly chunked request message and apply each chunk immediately.
    void updateFromClientRequestMessage(const nlohmann::json& j, uint64_t requestId)
    {
        ClientRequestChunk chunk;
        try {
            chunk = parseClientRequestChunk(j);
        }
        catch (const std::exception& e) {
            rejectClientRequest(requestId, fmt::format("Invalid request chunk: {}", e.what()));
            return;
        }

        if (!chunk.chunked) {
            {
                std::lock_guard lock(mutex_);
                pendingChunkedRequestId_ = 0;
                pendingChunkedNextIndex_ = 0;
                requestChunksComplete_ = true;
            }
            updateFromClientRequest(
                j,
                requestId,
                ClientRequestUpdateMode::Replace,
                true);
            return;
        }

        auto updateMode = ClientRequestUpdateMode::Replace;
        std::optional<std::string> errorMessage;
        {
            std::lock_guard lock(mutex_);
            auto requestsIt = j.find("requests");
            if (requestsIt == j.end() || !requestsIt->is_array()) {
                pendingChunkedRequestId_ = 0;
                pendingChunkedNextIndex_ = 0;
                requestChunksComplete_ = true;
                errorMessage = "Missing or invalid 'requests' array in chunk.";
            } else if (chunk.index == 0) {
                updateMode = ClientRequestUpdateMode::Replace;
                if (chunk.isLast) {
                    pendingChunkedRequestId_ = 0;
                    pendingChunkedNextIndex_ = 0;
                    requestChunksComplete_ = true;
                } else {
                    pendingChunkedRequestId_ = requestId;
                    pendingChunkedNextIndex_ = 1;
                    requestChunksComplete_ = false;
                }
            } else if (pendingChunkedRequestId_ != requestId || pendingChunkedNextIndex_ != chunk.index) {
                const auto expectedRequestId = pendingChunkedRequestId_;
                const auto expectedChunkIndex = pendingChunkedNextIndex_;
                pendingChunkedRequestId_ = 0;
                pendingChunkedNextIndex_ = 0;
                requestChunksComplete_ = true;
                errorMessage = fmt::format(
                    "Invalid request chunk sequence: expected chunk {} for request {}, got chunk {} for request {}.",
                    expectedChunkIndex,
                    expectedRequestId,
                    chunk.index,
                    requestId);
            } else {
                updateMode = ClientRequestUpdateMode::Append;
                if (chunk.isLast) {
                    pendingChunkedRequestId_ = 0;
                    pendingChunkedNextIndex_ = 0;
                    requestChunksComplete_ = true;
                } else {
                    pendingChunkedNextIndex_ = chunk.index + 1;
                    requestChunksComplete_ = false;
                }
            }
        }

        if (errorMessage) {
            rejectClientRequest(requestId, std::move(*errorMessage));
            return;
        }
        updateFromClientRequest(j, requestId, updateMode, chunk.isLast);
    }

    void rejectClientRequest(uint64_t requestId, std::string message)
    {
        {
            std::lock_guard lock(mutex_);
            requestId_ = requestId;
            requestInfos_.clear();
            requestStatuses_.clear();
            pendingChunkedRequestId_ = 0;
            pendingChunkedNextIndex_ = 0;
            requestChunksComplete_ = true;
            statusEmissionEnabled_ = true;
        }
        queueRequestContextMessage();
        queueStatusMessage(std::move(message));
    }

    /// Parse and apply a full logical tile request update from the client.
    void updateFromClientRequest(
        const nlohmann::json& j,
        uint64_t requestId,
        ClientRequestUpdateMode updateMode,
        bool requestChunksComplete)
    {
        auto requestsIt = j.find("requests");
        if (requestsIt == j.end() || !requestsIt->is_array()) {
            // Invalid request payload: publish an immediate status error for observability.
            {
                std::lock_guard lock(mutex_);
                requestId_ = requestId;
                requestInfos_.clear();
                requestStatuses_.clear();
                pendingChunkedRequestId_ = 0;
                pendingChunkedNextIndex_ = 0;
                requestChunksComplete_ = true;
                statusEmissionEnabled_ = true;
            }
            queueRequestContextMessage();
            queueStatusMessage("Missing or invalid 'requests' array");
            return;
        }

        struct ParsedRequest
        {
            detail::ParsedLayerTilesRequest request;
            LayerRequestContext context;
        };
        std::vector<ParsedRequest> parsedRequests;
        std::set<MapTileKey> desiredTileKeys;
        std::map<MapTileKey, int64_t> nextTilePriorityRanks;

        try {
            parsedRequests.reserve(requestsIt->size());
            for (auto const& requestJson : *requestsIt) {
                int64_t nextPriorityRank = 0;
                auto parsedRequest = detail::parseLayerTilesRequestJson(requestJson);
                auto layerContext = service_.resolveLayerRequest(
                    parsedRequest.mapId,
                    parsedRequest.layerId,
                    authHeaders_);
                auto expandedTileKeys = detail::expandLayerTilesRequestKeys(
                    parsedRequest,
                    REQUEST_TILE_LAYER_TYPE,
                    layerContext.stages_);

                // Priority ranks are layer-local: rank 0 means "highest priority"
                // within that layer request, then increases with request order/stage.
                for (auto const& tileKey : expandedTileKeys) {
                    auto requestedTileKey = makeCanonicalRequestedTileKey(tileKey);
                    desiredTileKeys.insert(requestedTileKey);
                    if (nextTilePriorityRanks.find(requestedTileKey) == nextTilePriorityRanks.end()) {
                        nextTilePriorityRanks.emplace(requestedTileKey, nextPriorityRank++);
                    }
                }

                parsedRequests.push_back(ParsedRequest{
                    .request = std::move(parsedRequest),
                    .context = std::move(layerContext),
                });
            }
        }
        catch (const std::exception& e) {
            {
                std::lock_guard lock(mutex_);
                requestId_ = requestId;
                requestInfos_.clear();
                requestStatuses_.clear();
                pendingChunkedRequestId_ = 0;
                pendingChunkedNextIndex_ = 0;
                requestChunksComplete_ = true;
                statusEmissionEnabled_ = true;
            }
            queueRequestContextMessage();
            queueStatusMessage(fmt::format("Invalid request JSON: {}", e.what()));
            return;
        }

        std::vector<LayerTilesRequest::Ptr> serviceRequests;
        std::vector<LayerTilesRequest::Ptr> nextActiveRequests;
        std::vector<RequestStatus> nextRequestStatuses(parsedRequests.size(), RequestStatus::Success);
        std::vector<RequestInfo> nextRequestInfos;
        nextRequestInfos.reserve(parsedRequests.size());
        size_t requestIndexBase = 0;
        std::optional<std::string> appendError;
        if (updateMode == ClientRequestUpdateMode::Append) {
            std::lock_guard lock(mutex_);
            if (requestId_ != requestId) {
                appendError = fmt::format(
                    "Cannot append request chunk {} to active request {}.",
                    requestId,
                    requestId_);
            } else {
                requestIndexBase = requestInfos_.size();
            }
        }
        if (appendError) {
            rejectClientRequest(requestId, std::move(*appendError));
            return;
        }

        for (size_t index = 0; index < parsedRequests.size(); ++index) {
            auto& parsed = parsedRequests[index];
            nextRequestInfos.push_back(RequestInfo{
                .mapId = parsed.request.mapId,
                .layerId = parsed.request.layerId,
            });

            std::vector<std::vector<TileId>> tileIdsByNextStageToFetch;
            std::vector<TileId> unstagedTileIdsToFetch;
            bool hasTilesToFetch = false;
            auto stageCount = std::max<uint32_t>(1U, parsed.context.stages_);
            {
                std::lock_guard lock(mutex_);
                if (parsed.request.usesStageBuckets) {
                    tileIdsByNextStageToFetch.resize(parsed.request.tileIdsByNextStage.size());
                    for (size_t bucketIndex = 0; bucketIndex < parsed.request.tileIdsByNextStage.size(); ++bucketIndex) {
                        auto nextMissingStage = static_cast<uint32_t>(bucketIndex);
                        if (nextMissingStage >= stageCount) {
                            continue;
                        }
                        for (auto const& tileId : parsed.request.tileIdsByNextStage[bucketIndex]) {
                            bool needsBackendFetch = false;
                            for (uint32_t stage = nextMissingStage; stage < stageCount; ++stage) {
                                auto requestedTileKey = makeCanonicalRequestedTileKey(
                                    parsed.request.mapId,
                                    parsed.request.layerId,
                                    tileId,
                                    stage);
                                const bool alreadyQueued =
                                    queuedTileFrameRefCount_.find(requestedTileKey) != queuedTileFrameRefCount_.end();
                                if (!alreadyQueued) {
                                    needsBackendFetch = true;
                                    break;
                                }
                            }
                            if (needsBackendFetch) {
                                tileIdsByNextStageToFetch[bucketIndex].push_back(tileId);
                                hasTilesToFetch = true;
                            }
                        }
                    }
                } else if (!parsed.request.tileIdsByNextStage.empty()) {
                    for (auto const& tileId : parsed.request.tileIdsByNextStage.front()) {
                        auto requestedTileKey = makeCanonicalRequestedTileKey(
                            parsed.request.mapId,
                            parsed.request.layerId,
                            tileId,
                            UnspecifiedStage);
                        const bool alreadyQueued =
                            queuedTileFrameRefCount_.find(requestedTileKey) != queuedTileFrameRefCount_.end();
                        if (!alreadyQueued) {
                            unstagedTileIdsToFetch.push_back(tileId);
                            hasTilesToFetch = true;
                        }
                    }
                }
            }

            if (!hasTilesToFetch) {
                continue;
            }

            LayerTilesRequest::Ptr request;
            if (parsed.request.usesStageBuckets) {
                request = std::make_shared<LayerTilesRequest>(
                    parsed.request.mapId,
                    parsed.request.layerId,
                    std::move(tileIdsByNextStageToFetch));
            } else {
                request = std::make_shared<LayerTilesRequest>(
                    parsed.request.mapId,
                    parsed.request.layerId,
                    std::move(unstagedTileIdsToFetch));
            }
            serviceRequests.push_back(request);
            nextActiveRequests.push_back(request);
            nextRequestStatuses[index] = RequestStatus::Open;

            const auto weak = weak_from_this();
            const auto expectedRequestId = requestId;
            const auto statusIndex = requestIndexBase + index;
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
            request->onDone_ = [weak, statusIndex, expectedRequestId, request](RequestStatus status) {
                if (auto self = weak.lock()) {
                    self->onRequestDone(statusIndex, expectedRequestId, request, status);
                }
            };
        }

        std::vector<LayerTilesRequest::Ptr> replacedRequests;
        {
            std::lock_guard lock(mutex_);
            if (updateMode == ClientRequestUpdateMode::Append && requestId_ == requestId) {
                for (auto& request : nextActiveRequests) {
                    activeRequests_.push_back(std::move(request));
                }
                for (auto& info : nextRequestInfos) {
                    requestInfos_.push_back(std::move(info));
                }
                for (auto status : nextRequestStatuses) {
                    requestStatuses_.push_back(status);
                }
                desiredTileKeys_.insert(desiredTileKeys.begin(), desiredTileKeys.end());
                for (auto const& [tileKey, priorityRank] : nextTilePriorityRanks) {
                    tilePriorityRanks_.emplace(tileKey, priorityRank);
                }
            } else {
                replacedRequests = std::move(activeRequests_);
                activeRequests_ = std::move(nextActiveRequests);
                requestId_ = requestId;
                requestInfos_ = std::move(nextRequestInfos);
                requestStatuses_ = std::move(nextRequestStatuses);
                desiredTileKeys_ = std::move(desiredTileKeys);
                tilePriorityRanks_ = std::move(nextTilePriorityRanks);
                // When request scope shrinks, remove stale tile data already queued for send.
                filterOutgoingByDesiredLocked();
            }
            requestChunksComplete_ = requestChunksComplete;
            // Refresh ordering so queued tiles follow the latest request priority.
            reprioritizeOutgoingLocked();
            statusEmissionEnabled_ = true;
        }

        if (!replacedRequests.empty()) {
            gTilesWsMetrics.replacedRequests.fetch_add(
                static_cast<int64_t>(replacedRequests.size()),
                std::memory_order_relaxed);
            abortRequests(std::move(replacedRequests));
        }

        queueRequestContextMessage();
        if (!serviceRequests.empty()) {
            (void)service_.request(serviceRequests, authHeaders_);
        }
        queueStatusMessage({});
    }

    /// Cancel current requests, clear queued frames, and emit a terminal status.
    void cancel(std::string reason)
    {
        cancelled_ = true;
        std::vector<LayerTilesRequest::Ptr> requestsToAbort;
        std::vector<PullDispatch> pullDispatches;

        // Stop sending any queued tile frames from this session.
        {
            std::lock_guard lock(mutex_);
            clearOutgoingLocked();
            requestsToAbort = std::move(activeRequests_);
            activeRequests_.clear();
            collectAllPullWaitersLocked(PullFrameResult::Status::Closed, pullDispatches);
        }

        // Abort in-flight requests (best-effort).
        abortRequests(std::move(requestsToAbort));

        // Refresh locally cached statuses after aborting.
        {
            std::lock_guard lock(mutex_);
            for (auto& status : requestStatuses_) {
                if (status == RequestStatus::Open) {
                    status = RequestStatus::Aborted;
                }
            }
        }

        dispatchPullResults(std::move(pullDispatches));
        queueStatusMessage(std::move(reason));
    }

private:
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

    /// One pending `/tiles/next` callback waiting for a frame or timeout.
    struct PullWaiter
    {
        uint64_t waiterId = 0;
        size_t maxBatchBytes = 0;
        PullResultCallback callback;
    };

    /// One completed pull waiter callback plus the result to emit.
    struct PullDispatch
    {
        PullResultCallback callback;
        PullFrameResult result;
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

    /// Pop the highest-priority queued tile frame and account forwarding metrics.
    [[nodiscard]] PullFrameResult popNextFrameLocked()
    {
        if (outgoing_.empty()) {
            return PullFrameResult{.status = PullFrameResult::Status::Timeout};
        }

        auto frame = std::move(outgoing_.front());
        outgoing_.pop_front();
        untrackQueuedFrameLocked(frame);
        if (frame.stringPoolCommit) {
            committedStringPoolOffsets_[frame.stringPoolCommit->first] = frame.stringPoolCommit->second;
        }

        const auto frameBytes = static_cast<int64_t>(frame.bytes.size());
        gTilesWsMetrics.totalForwardedFrames.fetch_add(1, std::memory_order_relaxed);
        gTilesWsMetrics.totalForwardedBytes.fetch_add(frameBytes, std::memory_order_relaxed);

        return PullFrameResult{
            .status = PullFrameResult::Status::Frame,
            .frameBytes = std::move(frame.bytes),
        };
    }

    /// Pop and concatenate queued frames up to one batch byte budget.
    [[nodiscard]] PullFrameResult popFrameBatchLocked(size_t maxBatchBytes)
    {
        if (outgoing_.empty()) {
            return PullFrameResult{.status = PullFrameResult::Status::Timeout};
        }
        if (maxBatchBytes == 0) {
            return popNextFrameLocked();
        }

        std::string batchBytes;
        batchBytes.reserve(std::min<size_t>(maxBatchBytes, outgoing_.front().bytes.size()));

        size_t appendedBytes = 0;
        bool appendedAny = false;
        while (!outgoing_.empty()) {
            const auto& nextFrame = outgoing_.front();
            const auto nextBytes = nextFrame.bytes.size();
            if (appendedAny && appendedBytes + nextBytes > maxBatchBytes) {
                break;
            }
            auto frameResult = popNextFrameLocked();
            if (frameResult.status != PullFrameResult::Status::Frame) {
                break;
            }
            appendedBytes += frameResult.frameBytes.size();
            batchBytes.append(frameResult.frameBytes);
            appendedAny = true;
            if (appendedBytes >= maxBatchBytes) {
                break;
            }
        }

        if (!appendedAny) {
            return PullFrameResult{.status = PullFrameResult::Status::Timeout};
        }
        return PullFrameResult{
            .status = PullFrameResult::Status::Frame,
            .frameBytes = std::move(batchBytes),
        };
    }

    /// Pop the next valid waiter in arrival order, skipping stale order entries.
    [[nodiscard]] bool popNextPullWaiterLocked(PullWaiter& out)
    {
        while (!pendingPullWaiterOrder_.empty()) {
            const auto waiterId = pendingPullWaiterOrder_.front();
            pendingPullWaiterOrder_.pop_front();
            auto waiterIt = pendingPullWaiters_.find(waiterId);
            if (waiterIt == pendingPullWaiters_.end()) {
                continue;
            }
            out = std::move(waiterIt->second);
            pendingPullWaiters_.erase(waiterIt);
            return true;
        }
        return false;
    }

    /// Remove one waiter id from the FIFO order list.
    void erasePullWaiterOrderEntryLocked(uint64_t waiterId)
    {
        pendingPullWaiterOrder_.erase(
            std::remove(pendingPullWaiterOrder_.begin(), pendingPullWaiterOrder_.end(), waiterId),
            pendingPullWaiterOrder_.end());
    }

    /// Complete queued pull waiters while both waiters and frames are available.
    void drainReadyPullWaitersLocked(std::vector<PullDispatch>& dispatches)
    {
        PullWaiter waiter;
        while (!outgoing_.empty() && popNextPullWaiterLocked(waiter)) {
            dispatches.push_back(PullDispatch{
                .callback = std::move(waiter.callback),
                .result = popFrameBatchLocked(waiter.maxBatchBytes),
            });
            waiter = PullWaiter{};
        }
    }

    /// Match one backend-produced tile key against the currently desired request set.
    [[nodiscard]] std::optional<MapTileKey> matchDesiredTileKeyLocked(
        MapTileKey key,
        uint32_t advertisedStages) const
    {
        auto requestedTileKey = makeCanonicalRequestedTileKey(std::move(key));
        if (desiredTileKeys_.find(requestedTileKey) != desiredTileKeys_.end()) {
            return requestedTileKey;
        }

        // Single-stage datasources legitimately return stage-less tiles even when
        // the client used staged bucket requests. Treat stage 0 and "unspecified"
        // as equivalent only for those layers.
        if (advertisedStages <= 1U) {
            if (requestedTileKey.stage_ == UnspecifiedStage) {
                requestedTileKey.stage_ = 0;
                if (desiredTileKeys_.find(requestedTileKey) != desiredTileKeys_.end()) {
                    return requestedTileKey;
                }
            } else if (requestedTileKey.stage_ == 0) {
                requestedTileKey.stage_ = UnspecifiedStage;
                if (desiredTileKeys_.find(requestedTileKey) != desiredTileKeys_.end()) {
                    return requestedTileKey;
                }
            }
        }

        return std::nullopt;
    }

    /// Complete all currently pending pull waiters with one terminal status.
    void collectAllPullWaitersLocked(PullFrameResult::Status status, std::vector<PullDispatch>& dispatches)
    {
        PullWaiter waiter;
        while (popNextPullWaiterLocked(waiter)) {
            dispatches.push_back(PullDispatch{
                .callback = std::move(waiter.callback),
                .result = PullFrameResult{.status = status},
            });
            waiter = PullWaiter{};
        }
        pendingPullWaiters_.clear();
    }

    /// Dispatch one completed pull callback on Drogon's loop.
    static void dispatchPullResult(PullResultCallback callback, PullFrameResult result)
    {
        if (!callback) {
            return;
        }
        drogon::app().getLoop()->queueInLoop(
            [callback = std::move(callback), result = std::move(result)]() mutable {
                callback(std::move(result));
            });
    }

    /// Dispatch a batch of completed pull callbacks on Drogon's loop.
    static void dispatchPullResults(std::vector<PullDispatch> dispatches)
    {
        for (auto& dispatch : dispatches) {
            dispatchPullResult(std::move(dispatch.callback), std::move(dispatch.result));
        }
    }

    /// Resolve one waiter with timeout if it is still pending.
    void onPullWaiterTimeout(uint64_t waiterId)
    {
        PullResultCallback timeoutCallback;
        {
            std::lock_guard lock(mutex_);
            auto waiterIt = pendingPullWaiters_.find(waiterId);
            if (waiterIt == pendingPullWaiters_.end()) {
                return;
            }
            timeoutCallback = std::move(waiterIt->second.callback);
            pendingPullWaiters_.erase(waiterIt);
            erasePullWaiterOrderEntryLocked(waiterId);
        }

        dispatchPullResult(
            std::move(timeoutCallback),
            PullFrameResult{.status = PullFrameResult::Status::Timeout});
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
        std::vector<LayerTilesRequest::Ptr> requestsToAbort;
        std::vector<PullDispatch> pullDispatches;

        // Ensure we stop emitting any further frames.
        {
            std::lock_guard lock(mutex_);
            clearOutgoingLocked();
            requestsToAbort = std::move(activeRequests_);
            activeRequests_.clear();
            collectAllPullWaitersLocked(PullFrameResult::Status::Closed, pullDispatches);
        }

        abortRequests(std::move(requestsToAbort));
        dispatchPullResults(std::move(pullDispatches));
    }

    /// Abort a batch of backend requests outside `mutex_` to avoid lock inversion.
    void abortRequests(std::vector<LayerTilesRequest::Ptr> requests)
    {
        for (auto const& request : requests) {
            if (!request || request->isDone()) {
                continue;
            }
            service_.abort(request);
        }
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

        try {
            std::optional<std::pair<std::string, simfil::StringId>> stringPoolCommit;
            std::vector<PullDispatch> pullDispatches;

            {
                std::lock_guard lock(mutex_);
                if (cancelled_)
                    return;
                auto requestedTileKey = matchDesiredTileKeyLocked(
                    layer->id(),
                    layer->layerInfo() ? std::max<uint32_t>(1U, layer->layerInfo()->stages_) : 1U);
                // Late-arriving tile for an outdated request: drop before serialization work.
                if (!requestedTileKey.has_value()) {
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
                        frame.requestedTileKey = *requestedTileKey;
                    }
                    if (m.type == TileLayerStream::MessageType::TileFeatureLayer
                        || m.type == TileLayerStream::MessageType::TileSourceDataLayer) {
                        frame.requestedTileKey = *requestedTileKey;
                    }
                    enqueueOutgoingLocked(std::move(frame));
                }
                // Newly queued frames can immediately satisfy blocked pull waiters.
                drainReadyPullWaitersLocked(pullDispatches);
            }
            dispatchPullResults(std::move(pullDispatches));
        }
        catch (const std::exception& e) {
            log().error("Failed to stream tile layer: {}", e.what());
            cancelNoStatus();
        }
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
        }
    }

    /// Send a websocket control frame immediately (status/request-context/load-state).
    void sendControlMessage(TileLayerStream::MessageType type, std::string payload)
    {
        auto conn = conn_.lock();
        if (!conn || conn->disconnected()) {
            cancelNoStatus();
            return;
        }
        try {
            conn->send(
                encodeStreamMessage(type, payload),
                drogon::WebSocketMessageType::Binary);
        }
        catch (const std::exception& e) {
            log().warn("WebSocket send failed: {}", e.what());
            cancelNoStatus();
        }
    }

    /// Send a status frame describing the current request statuses.
    void queueStatusMessage(std::string message)
    {
        sendControlMessage(TileLayerStream::MessageType::Status, buildStatusPayload(std::move(message)));
    }

    /// Send a request-context frame so the client can track the active request id + client id.
    void queueRequestContextMessage()
    {
        sendControlMessage(TileLayerStream::MessageType::RequestContext, buildRequestContextPayload());
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

        sendControlMessage(
            TileLayerStream::MessageType::LoadStateChange,
            buildLoadStatePayload(key, state));
    }

    /// Build the JSON payload for `mapget.tiles.status`.
    [[nodiscard]] std::string buildStatusPayload(std::string message)
    {
        nlohmann::json requestsJson = nlohmann::json::array();
        bool allDone = true;

        {
            std::lock_guard lock(mutex_);
            allDone = requestChunksComplete_;
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
            {"stage", key.stage_},
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
            {"clientId", clientId_},
        }).dump();
    }

    HttpService& service_;
    std::weak_ptr<drogon::WebSocketConnection> conn_;
    int64_t clientId_ = gNextClientId.fetch_add(1, std::memory_order_relaxed);
    uint64_t requestId_ = 0;
    uint64_t nextRequestId_ = 1;

    AuthHeaders authHeaders_;

    mutable std::mutex mutex_;
    uint64_t nextPullWaiterId_ = 1;
    std::deque<uint64_t> pendingPullWaiterOrder_;
    std::unordered_map<uint64_t, PullWaiter> pendingPullWaiters_;
    std::deque<OutgoingFrame> outgoing_;
    std::vector<RequestInfo> requestInfos_;
    std::vector<RequestStatus> requestStatuses_;
    std::vector<LayerTilesRequest::Ptr> activeRequests_;
    std::set<MapTileKey> desiredTileKeys_;
    std::map<MapTileKey, int64_t> tilePriorityRanks_;
    std::map<MapTileKey, int64_t> queuedTileFrameRefCount_;
    bool statusEmissionEnabled_ = false;
    uint64_t pendingChunkedRequestId_ = 0;
    uint64_t pendingChunkedNextIndex_ = 0;
    bool requestChunksComplete_ = true;

    TileLayerStream::StringPoolOffsetMap committedStringPoolOffsets_;
    TileLayerStream::StringPoolOffsetMap writerOffsets_;
    std::unique_ptr<TileLayerStream::Writer> writer_;
    std::optional<std::vector<WriterMessage>> currentWriteBatch_;

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
        {
            std::lock_guard lock(gSessionRegistryMutex);
            gSessionRegistry[session->clientId()] = session;
        }
        conn->setContext(std::move(session));
    }

    /// Handle control and request messages from the websocket client.
    /// Wrapped in try-catch because Drogon calls this with no exception
    /// protection — an uncaught exception would terminate the process.
    void handleNewMessage(
        const drogon::WebSocketConnectionPtr& conn,
        std::string&& message,
        const drogon::WebSocketMessageType& type) override
    {
        try {
            auto session = conn->getContext<TilesWsSession>();
            if (!session) {
                // This is a defensive fallback for unexpected context loss.
                session = std::make_shared<TilesWsSession>(service_, conn, AuthHeaders{});
                session->registerForMetrics();
                {
                    std::lock_guard lock(gSessionRegistryMutex);
                    gSessionRegistry[session->clientId()] = session;
                }
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
            session->updateFromClientRequestMessage(j, requestId);
        }
        catch (const std::exception& e) {
            log().error("WebSocket message handler failed: {}", e.what());
        }
    }

    /// Abort outstanding backend work once the websocket is closed.
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override
    {
        gTilesWsMetrics.activeConnections.fetch_sub(1, std::memory_order_relaxed);
        if (auto session = conn->getContext<TilesWsSession>()) {
            {
                std::lock_guard lock(gSessionRegistryMutex);
                gSessionRegistry.erase(session->clientId());
            }
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

namespace
{

[[nodiscard]] std::shared_ptr<TilesWsSession> findSessionByClientId(int64_t clientId)
{
    std::lock_guard lock(gSessionRegistryMutex);
    auto it = gSessionRegistry.find(clientId);
    if (it == gSessionRegistry.end()) {
        return {};
    }
    auto session = it->second.lock();
    if (!session) {
        gSessionRegistry.erase(it);
        return {};
    }
    return session;
}

[[nodiscard]] int64_t parseClampedInt64Parameter(
    const drogon::HttpRequestPtr& req,
    std::string_view key,
    int64_t defaultValue,
    int64_t minValue,
    int64_t maxValue)
{
    const auto rawValue = req->getParameter(std::string(key));
    if (rawValue.empty()) {
        return defaultValue;
    }
    try {
        const auto parsed = static_cast<int64_t>(std::stoll(rawValue));
        return std::clamp(parsed, minValue, maxValue);
    }
    catch (const std::exception&) {
        return defaultValue;
    }
}

void handleTilesNextRequest(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    gTilesWsMetrics.totalPullRequests.fetch_add(1, std::memory_order_relaxed);

    const auto clientId = parseClampedInt64Parameter(req, "clientId", 0, 0, std::numeric_limits<int64_t>::max());
    if (clientId <= 0) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k400BadRequest);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody("Missing or invalid clientId parameter.");
        callback(resp);
        return;
    }

    auto session = findSessionByClientId(clientId);
    if (!session) {
        gTilesWsMetrics.totalPullSessionMisses.fetch_add(1, std::memory_order_relaxed);
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k410Gone);
        callback(resp);
        return;
    }

    const auto waitMs = parseClampedInt64Parameter(
        req,
        "waitMs",
        DEFAULT_PULL_WAIT_MS,
        0,
        MAX_PULL_WAIT_MS);
    const auto maxBytes = parseClampedInt64Parameter(
        req,
        "maxBytes",
        0,
        0,
        MAX_PULL_BATCH_BYTES);
    const bool compressRequested = parseClampedInt64Parameter(req, "compress", 0, 0, 1) != 0;
    const bool enableGzip = compressRequested && containsGzip(req->getHeader("Accept-Encoding"));
    session->requestNextTileFrameAsync(
        std::chrono::milliseconds(waitMs),
        static_cast<size_t>(maxBytes),
        [callback = std::move(callback), enableGzip](TilesWsSession::PullFrameResult result) mutable {
            auto resp = drogon::HttpResponse::newHttpResponse();
            switch (result.status) {
            case TilesWsSession::PullFrameResult::Status::Frame:
                resp->setStatusCode(drogon::k200OK);
                resp->setContentTypeCode(drogon::CT_APPLICATION_OCTET_STREAM);
                if (enableGzip) {
                    if (auto compressed = gzipCompress(result.frameBytes)) {
                        resp->addHeader("x-mapget-compressed-bytes", std::to_string(compressed->size()));
                        resp->setBody(std::move(*compressed));
                        resp->addHeader("Content-Encoding", "gzip");
                        resp->addHeader("Vary", "Accept-Encoding");
                    } else {
                        resp->setBody(std::move(result.frameBytes));
                    }
                } else {
                    resp->setBody(std::move(result.frameBytes));
                }
                break;
            case TilesWsSession::PullFrameResult::Status::Timeout:
                gTilesWsMetrics.totalPullTimeouts.fetch_add(1, std::memory_order_relaxed);
                resp->setStatusCode(drogon::k204NoContent);
                break;
            case TilesWsSession::PullFrameResult::Status::Closed:
                resp->setStatusCode(drogon::k410Gone);
                break;
            }
            callback(resp);
        });
}

}  // namespace

/// Register the `/tiles` websocket controller with Drogon.
void registerTilesWebSocketController(drogon::HttpAppFramework& app, HttpService& service)
{
    app.registerController(std::make_shared<TilesWebSocketController>(service));
    app.registerHandler(
        "/tiles/next",
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handleTilesNextRequest(req, std::move(callback));
        },
        {drogon::Get, drogon::Post});
}

/// Build the websocket metrics payload consumed by `/status-data`.
nlohmann::json tilesWebSocketMetricsSnapshot()
{
    int64_t pendingControllerFrames = 0;
    int64_t pendingControllerBytes = 0;
    int64_t pendingPullRequests = 0;
    {
        std::lock_guard lock(gTrackedSessionsMutex);
        auto out = gTrackedSessions.begin();
        for (auto it = gTrackedSessions.begin(); it != gTrackedSessions.end(); ++it) {
            if (auto session = it->lock()) {
                auto [frames, bytes] = session->pendingSnapshot();
                pendingControllerFrames += frames;
                pendingControllerBytes += bytes;
                pendingPullRequests += session->pendingPullRequestCount();
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
        {"pending-pull-requests", pendingPullRequests},
        {"total-queued-frames", nonNegative(gTilesWsMetrics.totalQueuedFrames)},
        {"total-queued-bytes", nonNegative(gTilesWsMetrics.totalQueuedBytes)},
        {"total-forwarded-frames", nonNegative(gTilesWsMetrics.totalForwardedFrames)},
        {"total-forwarded-bytes", nonNegative(gTilesWsMetrics.totalForwardedBytes)},
        {"total-dropped-frames", nonNegative(gTilesWsMetrics.totalDroppedFrames)},
        {"total-dropped-bytes", nonNegative(gTilesWsMetrics.totalDroppedBytes)},
        {"total-pull-requests", nonNegative(gTilesWsMetrics.totalPullRequests)},
        {"total-pull-timeouts", nonNegative(gTilesWsMetrics.totalPullTimeouts)},
        {"total-pull-session-misses", nonNegative(gTilesWsMetrics.totalPullSessionMisses)},
        {"replaced-requests", nonNegative(gTilesWsMetrics.replacedRequests)},
    });
}

}  // namespace mapget::detail
