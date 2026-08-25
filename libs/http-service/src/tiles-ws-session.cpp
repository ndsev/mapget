#include "tiles-ws-session.h"

#include "tiles-request-json.h"
#include "tiles-stream-encoding.h"
#include "tiles-ws-request.h"
#include "tiles-ws-status.h"

#include "mapget/log.h"
#include "mapget/model/featurelayer-filter.h"
#include "mapget/model/subsetlayer.h"
#include "mapget/model/stream.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/WebSocketConnection.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fmt/format.h"
#include "nlohmann/json.hpp"


namespace mapget::detail
{

/** Process-wide counters backing the websocket status-data snapshot. */
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
    std::atomic<int64_t> reconciledSnapshots{0};
    std::atomic<int64_t> supersededSnapshots{0};
    std::atomic<int64_t> totalPullRequests{0};
    std::atomic<int64_t> totalPullTimeouts{0};
    std::atomic<int64_t> totalPullSessionMisses{0};
    std::atomic<int64_t> pendingAllocatedBytes{0};
    std::atomic<int64_t> peakPendingAllocatedBytes{0};
    std::atomic<int64_t> suppressedActiveOutputs{0};
    std::atomic<int64_t> suppressedQueuedOutputs{0};
    std::atomic<int64_t> suppressedHandoffOutputs{0};
    std::atomic<int64_t> expiredHandoffRecords{0};
    std::atomic<int64_t> retainedOutputs{0};
    std::atomic<int64_t> prunedOutputs{0};
    std::atomic<int64_t> obsoleteOwnerCallbacks{0};
};

TilesWsMetrics gTilesWsMetrics;
std::mutex gTrackedSessionsMutex;
std::vector<std::weak_ptr<class TilesWsSession>> gTrackedSessions;
std::mutex gSessionRegistryMutex;
std::unordered_map<int64_t, std::weak_ptr<class TilesWsSession>> gSessionRegistry;
std::atomic<int64_t> gNextClientId{1};

std::string_view catalogStatusToString(DataSourceCatalogStatus status)
{
    switch (status) {
    case DataSourceCatalogStatus::Initializing:
        return "initializing";
    case DataSourceCatalogStatus::Ready:
        return "ready";
    case DataSourceCatalogStatus::Failed:
        return "failed";
    }
    return "failed";
}

constexpr int64_t DEFAULT_PULL_WAIT_MS = 25000;
constexpr int64_t MAX_PULL_WAIT_MS = 30000;
constexpr int64_t MAX_PULL_BATCH_BYTES = 64 * 1024 * 1024;
constexpr size_t OUTGOING_FRAME_ADMISSION_WATERMARK = 4096;
constexpr size_t OUTGOING_BYTE_ADMISSION_WATERMARK = 256 * 1024 * 1024;
constexpr int64_t LOWEST_TILE_PRIORITY = std::numeric_limits<int64_t>::max();
constexpr bool EMIT_LOAD_STATE_FRAMES = false;

/** Preserve caller order while restricting scheduling hints to retained work. */
std::vector<TileId> prioritiesWithin(
    std::vector<TileId> const& tileIds,
    std::vector<TileId> const& priorityTileIds)
{
    std::set<TileId> const membership(
        tileIds.begin(),
        tileIds.end());
    std::vector<TileId> result;
    result.reserve(
        std::min(tileIds.size(), priorityTileIds.size()));
    for (auto const& tileId : priorityTileIds) {
        if (membership.contains(tileId)) {
            result.push_back(tileId);
        }
    }
    return result;
}

[[nodiscard]] bool isTileDataMessage(TileLayerStream::MessageType type)
{
    return type == TileLayerStream::MessageType::TileFeatureLayer
        || type == TileLayerStream::MessageType::TileSourceDataLayer
        || type == TileLayerStream::MessageType::TileSubsetLayer;
}

/** Clamp an atomic metric value to zero to avoid exposing negative snapshots. */
[[nodiscard]] int64_t nonNegative(std::atomic<int64_t> const& value)
{
    const auto v = value.load(std::memory_order_relaxed);
    return v < 0 ? 0 : v;
}

/**
 * Per-client streaming state for `/interactive` and `/interactive/payload`.
 *
 * The session owns active backend requests, queued tile frames, string-pool
 * writer offsets, and long-poll waiters for one logical websocket connection.
 */
class TilesWsSession : public std::enable_shared_from_this<TilesWsSession>
{
public:
    /** Construct one websocket session object bound 1:1 to a websocket connection. */
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

    /** Destroy the session and abort any in-flight backend work. */
    ~TilesWsSession()
    {
        try {
            {
                std::lock_guard lock(gSessionRegistryMutex);
                gSessionRegistry.erase(clientId_);
            }
            gTilesWsMetrics.activeSessions.fetch_sub(1, std::memory_order_relaxed);
            // Best-effort cleanup: abort any in-flight requests if the session is destroyed.
            cancelNoStatus();
        }
        catch (std::exception const&) {
        }
        catch (...) {
        }
    }

    TilesWsSession(TilesWsSession const&) = delete;
    TilesWsSession& operator=(TilesWsSession const&) = delete;

    /** Subscribe after shared ownership exists so callbacks can safely capture weak_from_this(). */
    void startSourceCatalogSubscription()
    {
        auto weak = weak_from_this();
        sourceCatalogSubscription_ = service_.subscribeToSourceCatalogChanges(
            [weak](DataSourceCatalogChange const& change) {
                if (auto self = weak.lock()) {
                    self->queueSourceCatalogChangeMessage(change);
                }
            });
    }

    /** Register this session in the global weak list used for `/status-data` snapshots. */
    void registerForMetrics()
    {
        std::lock_guard lock(gTrackedSessionsMutex);
        gTrackedSessions.push_back(weak_from_this());
    }

    /** Return currently queued controller frames/bytes. */
    [[nodiscard]] std::pair<int64_t, int64_t> pendingSnapshot()
    {
        std::lock_guard lock(mutex_);
        int64_t pendingFrames = static_cast<int64_t>(outgoing_.size());
        int64_t pendingBytes = static_cast<int64_t>(queuedOutgoingBytes_);
        return {pendingFrames, pendingBytes};
    }

    /** Return current active, queued, and handoff output ownership counts. */
    [[nodiscard]] std::tuple<int64_t, int64_t, int64_t> ownershipSnapshot()
    {
        std::lock_guard lock(mutex_);
        return {
            static_cast<int64_t>(activeTileOwners_.size() + activeFilterOwners_.size()),
            static_cast<int64_t>(queuedTileFrameRefCount_.size()),
            static_cast<int64_t>(handoffRecords_.size()),
        };
    }

    /** Return numeric client id used by `/interactive/payload` pull requests. */
    [[nodiscard]] int64_t clientId() const
    {
        return clientId_;
    }

    /** Return current number of blocked `/interactive/payload` long-poll requests. */
    [[nodiscard]] int64_t pendingPullRequestCount() const
    {
        std::lock_guard lock(mutex_);
        return static_cast<int64_t>(pendingPullWaiters_.size());
    }

    /** Result delivered to one pending `/interactive/payload` long-poll request. */
    struct PullFrameResult
    {
        /** Distinguishes payload delivery, timeout, and closed-session responses. */
        enum class Status {
            Frame,
            Timeout,
            Closed,
        };

        Status status = Status::Timeout;
        std::string frameBytes;
    };

    using PullResultCallback = std::function<void(PullFrameResult)>;

    /** Resolve one `/interactive/payload` request immediately or register an async waiter. */
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

    /** Patch per-connection string-pool offsets supplied by the client request. */
    [[nodiscard]] bool applyStringPoolOffsetsPatch(const nlohmann::json& offsetsJson, std::string& errorMessage)
    {
        try {
            auto parsedOffsets = parseStringPoolOffsetsJson(offsetsJson);
            std::lock_guard lock(mutex_);
            for (auto const& [stringPoolId, offset] : parsedOffsets) {
                committedStringPoolOffsets_[stringPoolId] = offset;
                writerOffsets_[stringPoolId] = offset;
            }
            return true;
        }
        catch (const std::exception& e) {
            errorMessage = fmt::format("Invalid stringPoolOffsets: {}", e.what());
            return false;
        }
    }

    /** Allocate a request id while respecting optional client-provided request ids. */
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

    /** Stage indexed chunks and reconcile only one complete logical snapshot. */
    void updateFromClientRequestMessage(nlohmann::json j, uint64_t requestId)
    {
        if (j.contains("renewals")) {
            {
                std::lock_guard lock(mutex_);
                stagedRequest_.reset();
            }
            rejectClientRequest(requestId, "renewals is not supported by protocol 4.0");
            return;
        }
        ClientRequestChunk chunk;
        try {
            chunk = parseClientRequestChunk(j);
        }
        catch (const std::exception& e) {
            {
                std::lock_guard lock(mutex_);
                stagedRequest_.reset();
            }
            rejectClientRequest(requestId, fmt::format("Invalid request chunk: {}", e.what()));
            return;
        }

        if (!chunk.chunked) {
            {
                std::lock_guard lock(mutex_);
                stagedRequest_.reset();
            }
            queueCompletedClientRequest(std::move(j), requestId);
            return;
        }

        std::optional<nlohmann::json> completedEnvelope;
        std::optional<std::string> errorMessage;
        {
            std::lock_guard lock(mutex_);
            auto requestsIt = j.find("requests");
            if (requestsIt == j.end() || !requestsIt->is_array()) {
                stagedRequest_.reset();
                errorMessage = "Missing or invalid 'requests' array in chunk.";
            } else if (chunk.index == 0) {
                auto envelope = j;
                envelope.erase("chunk");
                envelope["requests"] = nlohmann::json::array();
                for (auto requestJson : *requestsIt) {
                    detail::inheritFilterFields(requestJson, j);
                    envelope["requests"].push_back(std::move(requestJson));
                }
                if (chunk.isLast) {
                    completedEnvelope = std::move(envelope);
                    stagedRequest_.reset();
                } else {
                    stagedRequest_ = StagedRequest{
                        .requestId = requestId,
                        .nextChunkIndex = 1,
                        .envelope = std::move(envelope),
                    };
                }
            } else if (!stagedRequest_ || stagedRequest_->requestId != requestId ||
                       stagedRequest_->nextChunkIndex != chunk.index) {
                const auto expectedRequestId = stagedRequest_ ? stagedRequest_->requestId : 0;
                const auto expectedChunkIndex = stagedRequest_ ? stagedRequest_->nextChunkIndex : 0;
                stagedRequest_.reset();
                errorMessage = fmt::format(
                    "Invalid request chunk sequence: expected chunk {} for request {}, got chunk {} for request {}.",
                    expectedChunkIndex,
                    expectedRequestId,
                    chunk.index,
                    requestId);
            } else {
                for (auto requestJson : *requestsIt) {
                    detail::inheritFilterFields(requestJson, j);
                    stagedRequest_->envelope["requests"].push_back(std::move(requestJson));
                }
                if (chunk.isLast) {
                    completedEnvelope = std::move(stagedRequest_->envelope);
                    stagedRequest_.reset();
                } else {
                    stagedRequest_->nextChunkIndex = chunk.index + 1;
                }
            }
        }

        if (errorMessage) {
            rejectClientRequest(requestId, std::move(*errorMessage));
            return;
        }
        if (completedEnvelope) {
            queueCompletedClientRequest(std::move(*completedEnvelope), requestId);
        }
    }

    /** Publish the newest complete snapshot to the per-session reconciliation mailbox. */
    void queueCompletedClientRequest(nlohmann::json requestJson, uint64_t requestId)
    {
        bool scheduleTask = false;
        {
            std::lock_guard lock(mutex_);
            if (cancelled_) {
                return;
            }
            if (pendingReconciliation_) {
                // Complete snapshots are replacement state. Only the newest
                // unapplied candidate can affect backend demand.
                gTilesWsMetrics.supersededSnapshots.fetch_add(1, std::memory_order_relaxed);
            }
            auto const sequence = ++reconciliationSequence_;
            latestReconciliationSequence_.store(sequence, std::memory_order_release);
            pendingReconciliation_ = PendingReconciliation{
                .requestId = requestId,
                .sequence = sequence,
                .envelope = std::move(requestJson),
            };
            if (!reconciliationTaskScheduled_) {
                reconciliationTaskScheduled_ = true;
                scheduleTask = true;
            }
        }
        if (!scheduleTask) {
            return;
        }

        auto weak = weak_from_this();
        if (service_.enqueueInteractiveControlTask([weak] {
                if (auto self = weak.lock()) {
                    self->drainCompletedClientRequests();
                }
            }))
        {
            return;
        }

        // Service shutdown can reject the task after this session published it.
        std::lock_guard lock(mutex_);
        reconciliationTaskScheduled_ = false;
        pendingReconciliation_.reset();
    }

    /** Reconcile one running candidate and then only the newest snapshot that replaced it. */
    void drainCompletedClientRequests()
    {
        while (true) {
            std::optional<PendingReconciliation> candidate;
            {
                std::lock_guard lock(mutex_);
                if (cancelled_) {
                    pendingReconciliation_.reset();
                    reconciliationTaskScheduled_ = false;
                    return;
                }
                if (!pendingReconciliation_) {
                    reconciliationTaskScheduled_ = false;
                    return;
                }
                candidate = std::move(pendingReconciliation_);
                pendingReconciliation_.reset();
            }
            try {
                updateFromClientRequest(
                    candidate->envelope,
                    candidate->requestId,
                    candidate->sequence);
            }
            catch (std::exception const& e) {
                // One malformed or failing candidate must not strand the
                // mailbox's scheduled bit and block all later snapshots.
                if (!shouldAbandonReconciliation(candidate->sequence)) {
                    rejectClientRequest(
                        candidate->requestId,
                        fmt::format("Request reconciliation failed: {}", e.what()));
                }
            }
            catch (...) {
                if (!shouldAbandonReconciliation(candidate->sequence)) {
                    rejectClientRequest(
                        candidate->requestId,
                        "Request reconciliation failed with an unknown exception.");
                }
            }
        }
    }

    /** Report an invalid candidate snapshot without disturbing the active one. */
    void rejectClientRequest(uint64_t requestId, std::string message)
    {
        sendControlMessage(
            TileLayerStream::MessageType::Status,
            nlohmann::json::object({
                {"type", "mapget.tiles.status"},
                {"requestId", requestId},
                {"allDone", true},
                {"requests", nlohmann::json::array()},
                {"message", std::move(message)},
            }).dump());
    }

    /** Stop canceled or superseded request expansion before it can mutate active state. */
    [[nodiscard]] bool shouldAbandonReconciliation(uint64_t sequence) const
    {
        if (cancelled_) {
            return true;
        }
        if (latestReconciliationSequence_.load(std::memory_order_acquire) == sequence) {
            return false;
        }
        gTilesWsMetrics.supersededSnapshots.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /** Parse and atomically reconcile one complete pending-output snapshot. */
    void updateFromClientRequest(
        const nlohmann::json& j,
        uint64_t requestId,
        uint64_t sequence)
    {
        if (shouldAbandonReconciliation(sequence)) {
            return;
        }
        auto requestsIt = j.find("requests");
        if (requestsIt == j.end() || !requestsIt->is_array()) {
            rejectClientRequest(requestId, "Missing or invalid 'requests' array");
            return;
        }

        std::vector<SnapshotRequest> snapshotRequests;
        std::set<MapTileKey> nextPendingTileKeys;
        std::map<MapTileKey, int64_t> nextTilePriorityRanks;
        FilterRegistrationState filterRegistrations;
        try {
            snapshotRequests.reserve(requestsIt->size());
            for (auto requestJson : *requestsIt) {
                if (shouldAbandonReconciliation(sequence)) {
                    return;
                }
                detail::inheritFilterFields(requestJson, j);
                auto parsed = detail::parseLayerTilesRequestJson(requestJson);
                if (shouldAbandonReconciliation(sequence)) {
                    return;
                }
                registerFilterRequest(parsed, filterRegistrations);
                auto context = service_.resolveLayerRequest(
                    parsed.mapId,
                    parsed.layerId,
                    authHeaders_,
                    parsed.sourceId);
                auto pendingKeys = requestedTileKeys(parsed);
                if (shouldAbandonReconciliation(sequence)) {
                    return;
                }
                int64_t priorityRank = 0;
                for (auto const& key : pendingKeys) {
                    nextPendingTileKeys.insert(key);
                    nextTilePriorityRanks.try_emplace(key, priorityRank++);
                }
                snapshotRequests.push_back(SnapshotRequest{
                    .request = std::move(parsed),
                    .context = context,
                    .pendingKeys = std::move(pendingKeys),
                });
            }
        }
        catch (std::exception const& e) {
            rejectClientRequest(requestId, fmt::format("Invalid request JSON: {}", e.what()));
            return;
        }

        using RetainedLayerOutputs = std::map<
            LayerTilesRequest::Ptr,
            std::set<TileId>,
            std::owner_less<LayerTilesRequest::Ptr>>;
        using RetainedFilterOutputs = std::map<
            FeatureLayerFilterTilesRequest::Ptr,
            std::set<TileId>,
            std::owner_less<FeatureLayerFilterTilesRequest::Ptr>>;
        RetainedLayerOutputs retainedLayerOutputs;
        RetainedFilterOutputs retainedFilterOutputs;
        std::vector<LayerTilesRequest::Ptr> requestsToAbort;
        std::vector<FeatureLayerFilterTilesRequest::Ptr> filterRequestsToAbort;
        std::vector<LayerTilesRequest::Ptr> serviceRequests;
        std::vector<FeatureLayerFilterTilesRequest::Ptr> filterServiceRequests;

        {
            std::unique_lock lock(mutex_);
            if (shouldAbandonReconciliation(sequence)) {
                return;
            }
            expireHandoffRecordsLocked(std::chrono::system_clock::now());
            if (auto error = validateFilterRegistrationOverlapLocked(filterRegistrations)) {
                // A generation identifies one immutable filter definition for
                // every overlapping output. Reject before touching the active
                // snapshot so the client can retry with a new generation.
                lock.unlock();
                rejectClientRequest(requestId, std::move(*error));
                return;
            }
            std::erase_if(
                handoffRecords_,
                [&](auto const& item) { return !nextPendingTileKeys.contains(item.first); });

            pendingTileKeys_ = std::move(nextPendingTileKeys);
            tilePriorityRanks_ = std::move(nextTilePriorityRanks);
            terminalOutputStatuses_.clear();
            filterOutgoingByPendingLocked();

            for (auto owner = activeTileOwners_.begin(); owner != activeTileOwners_.end();) {
                if (pendingTileKeys_.contains(owner->first)) {
                    retainedLayerOutputs[owner->second].insert(owner->first.tileId_);
                    gTilesWsMetrics.retainedOutputs.fetch_add(1, std::memory_order_relaxed);
                    ++owner;
                }
                else {
                    gTilesWsMetrics.prunedOutputs.fetch_add(1, std::memory_order_relaxed);
                    owner = activeTileOwners_.erase(owner);
                }
            }
            for (auto owner = activeFilterOwners_.begin(); owner != activeFilterOwners_.end();) {
                if (pendingTileKeys_.contains(owner->first)) {
                    retainedFilterOutputs[owner->second].insert(owner->first.tileId_);
                    gTilesWsMetrics.retainedOutputs.fetch_add(1, std::memory_order_relaxed);
                    ++owner;
                }
                else {
                    gTilesWsMetrics.prunedOutputs.fetch_add(1, std::memory_order_relaxed);
                    owner = activeFilterOwners_.erase(owner);
                }
            }

            for (auto const& request : activeRequests_) {
                if (!request || request->isDone()) {
                    continue;
                }
                if (!retainedLayerOutputs.contains(request)) {
                    requestsToAbort.push_back(request);
                }
            }
            for (auto const& request : activeFilterRequests_) {
                if (!request || request->isDone()) {
                    continue;
                }
                if (!retainedFilterOutputs.contains(request)) {
                    filterRequestsToAbort.push_back(request);
                }
            }
            std::erase_if(
                activeRequests_,
                [&](LayerTilesRequest::Ptr const& request)
                { return !request || request->isDone() || !retainedLayerOutputs.contains(request); });
            std::erase_if(
                activeFilterRequests_,
                [&](FeatureLayerFilterTilesRequest::Ptr const& request)
                { return !request || request->isDone() || !retainedFilterOutputs.contains(request); });

            requestId_ = requestId;
            requestInfos_.clear();
            requestStatuses_.clear();
            requestInfos_.reserve(snapshotRequests.size());
            requestStatuses_.resize(snapshotRequests.size(), RequestStatus::Success);

            for (auto const& snapshot : snapshotRequests) {
                requestInfos_.push_back(RequestInfo{
                    .mapId = snapshot.request.mapId,
                    .layerId = snapshot.request.layerId,
                    .noDataSourceReason = snapshot.context.noDataSourceReason_,
                    .admissionStatus = snapshot.context.status_,
                    .pendingKeys = snapshot.pendingKeys,
                });
                if (snapshot.context.status_ != RequestStatus::Success) {
                    continue;
                }

                std::vector<TileId> additions;
                additions.reserve(snapshot.pendingKeys.size());
                for (auto const& key : snapshot.pendingKeys) {
                    if (activeTileOwners_.contains(key) || activeFilterOwners_.contains(key)) {
                        gTilesWsMetrics.suppressedActiveOutputs.fetch_add(
                            1,
                            std::memory_order_relaxed);
                        continue;
                    }
                    if (queuedTileFrameRefCount_.contains(key)) {
                        gTilesWsMetrics.suppressedQueuedOutputs.fetch_add(
                            1,
                            std::memory_order_relaxed);
                        continue;
                    }
                    if (handoffRecords_.contains(key)) {
                        gTilesWsMetrics.suppressedHandoffOutputs.fetch_add(
                            1,
                            std::memory_order_relaxed);
                        continue;
                    }
                    // requestedTileKeys() has already removed duplicate IDs
                    // within this logical request.
                    additions.push_back(key.tileId_);
                }
                if (additions.empty()) {
                    continue;
                }

                if (snapshot.request.filterRequest) {
                    auto request = makeFilterBackendRequest(snapshot.request, additions);
                    for (auto const& tileId : additions) {
                        auto key = filterRequestedTileKey(snapshot.request, tileId);
                        activeFilterOwners_[std::move(key)] = request;
                    }
                    activeFilterRequests_.push_back(request);
                    filterServiceRequests.push_back(std::move(request));
                }
                else {
                    auto request = makeLayerBackendRequest(snapshot.request, additions);
                    for (auto const& tileId : additions) {
                        auto key = makeCanonicalRequestedTileKey(
                            snapshot.request.mapId,
                            snapshot.request.layerId,
                            tileId);
                        activeTileOwners_[std::move(key)] = request;
                    }
                    activeRequests_.push_back(request);
                    serviceRequests.push_back(std::move(request));
                }
            }

            refreshRequestStatusesLocked();
            reprioritizeOutgoingLocked();
            filterRegistrations_ = std::move(filterRegistrations);
            statusEmissionEnabled_ = true;
            gTilesWsMetrics.reconciledSnapshots.fetch_add(1, std::memory_order_relaxed);
        }

        // Service pruning can synchronously invoke completion callbacks and is
        // therefore deliberately kept outside the session mutex.
        for (auto const& [request, retained] : retainedLayerOutputs) {
            service_.retainOutputs(request, retained);
        }
        for (auto const& [request, retained] : retainedFilterOutputs) {
            service_.retainOutputs(request, retained);
        }
        abortRequests(std::move(requestsToAbort));
        abortFilterRequests(std::move(filterRequestsToAbort));

        queueRequestContextMessage();
        submitBackendRequests(serviceRequests, filterServiceRequests);
        queueStatusMessage({});
    }

    /** Cancel current requests, clear queued frames, and emit a terminal status. */
    void cancel(std::string reason)
    {
        cancelled_ = true;
        workAdmissionOpen_->store(false, std::memory_order_release);
        std::vector<LayerTilesRequest::Ptr> requestsToAbort;
        std::vector<FeatureLayerFilterTilesRequest::Ptr> filterRequestsToAbort;
        std::vector<PullDispatch> pullDispatches;

        // Stop sending any queued tile frames from this session.
        {
            std::lock_guard lock(mutex_);
            clearOutgoingLocked();
            requestsToAbort = std::move(activeRequests_);
            activeRequests_.clear();
            filterRequestsToAbort = std::move(activeFilterRequests_);
            activeFilterRequests_.clear();
            activeTileOwners_.clear();
            activeFilterOwners_.clear();
            terminalOutputStatuses_.clear();
            pendingTileKeys_.clear();
            handoffRecords_.clear();
            filterRegistrations_.clear();
            stagedRequest_.reset();
            pendingReconciliation_.reset();
            collectAllPullWaitersLocked(PullFrameResult::Status::Closed, pullDispatches);
        }

        // Abort in-flight requests (best-effort).
        abortRequests(std::move(requestsToAbort));
        abortFilterRequests(std::move(filterRequestsToAbort));

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
    /** Immutable filter semantics retained only for transient output overlap checks. */
    struct FilterRegistration
    {
        std::string mapId;
        std::string layerId;
        std::optional<std::string> sourceId;
        FeatureLayerFilterRequest definition;
        std::map<TileId, std::vector<FeatureLayerFilterRoot>> rootsByTile;
        std::set<TileId> pendingTileIds;
    };
    using FilterRegistrationState =
        std::map<std::string, FilterRegistration>;

    /** Parsed request plus admission and canonical output identity. */
    struct SnapshotRequest
    {
        detail::ParsedLayerTilesRequest request;
        LayerRequestContext context;
        std::vector<MapTileKey> pendingKeys;
    };

    /** Lightweight metadata emitted in status payloads for each logical request. */
    struct RequestInfo
    {
        std::string mapId;
        std::string layerId;
        NoDataSourceReason noDataSourceReason = NoDataSourceReason::None;
        RequestStatus admissionStatus = RequestStatus::Success;
        std::vector<MapTileKey> pendingKeys;
    };

    /** Short-lived suppression after bytes leave the bounded payload queue. */
    struct HandoffRecord
    {
        std::optional<std::chrono::system_clock::time_point> expiresAt;
    };

    /** Incomplete indexed request chunks staged without mutating active state. */
    struct StagedRequest
    {
        uint64_t requestId = 0;
        uint64_t nextChunkIndex = 0;
        nlohmann::json envelope;
    };

    /** Newest complete replacement snapshot awaiting control-thread reconciliation. */
    struct PendingReconciliation
    {
        uint64_t requestId = 0;
        uint64_t sequence = 0;
        nlohmann::json envelope;
    };

    /** One queued websocket frame plus metadata used for bookkeeping. */
    struct OutgoingFrame
    {
        std::string bytes;
        TileLayerStream::MessageType type{TileLayerStream::MessageType::None};
        std::optional<std::pair<std::string, simfil::StringId>> stringPoolCommit;
        std::optional<MapTileKey> requestedTileKey;
        std::optional<std::chrono::system_clock::time_point> handoffExpiry;
        int64_t priorityRank = LOWEST_TILE_PRIORITY;
        uint64_t trackedCapacityBytes = 0;
    };

    /** Batched writer output captured while serializing one tile layer. */
    struct WriterMessage
    {
        std::string bytes;
        TileLayerStream::MessageType type{TileLayerStream::MessageType::None};
    };

    /** One pending `/interactive/payload` callback waiting for a frame or timeout. */
    struct PullWaiter
    {
        uint64_t waiterId = 0;
        size_t maxBatchBytes = 0;
        PullResultCallback callback;
    };

    /** One completed pull waiter callback plus the result to emit. */
    struct PullDispatch
    {
        PullResultCallback callback;
        PullFrameResult result;
    };

    /** Compare exact-root identity without transport-local request ordinals. */
    [[nodiscard]] static bool sameFilterRoot(
        FeatureLayerFilterRoot const& lhs,
        FeatureLayerFilterRoot const& rhs)
    {
        return lhs.tileId_ == rhs.tileId_ && lhs.typeId_ == rhs.typeId_ &&
            lhs.featureId_ == rhs.featureId_ &&
            lhs.canonicalFeatureId_ == rhs.canonicalFeatureId_;
    }

    /** Compare the ordered exact-root set affecting one output tile. */
    [[nodiscard]] static bool sameFilterRoots(
        std::vector<FeatureLayerFilterRoot> const& lhs,
        std::vector<FeatureLayerFilterRoot> const& rhs)
    {
        return lhs.size() == rhs.size() &&
            std::ranges::equal(lhs, rhs, sameFilterRoot);
    }

    /** Compare the non-coverage semantics of two filter registrations. */
    [[nodiscard]] static bool sameFilterDefinition(
        FilterRegistration const& lhs,
        FilterRegistration const& rhs)
    {
        return lhs.mapId == rhs.mapId && lhs.layerId == rhs.layerId &&
            lhs.sourceId == rhs.sourceId &&
            detail::filterRequestToJson(lhs.definition, false) ==
                detail::filterRequestToJson(rhs.definition, false);
    }

    /** Return whether one registered filtered output still has transient ownership. */
    [[nodiscard]] bool ownsRegisteredFilterOutputLocked(
        FilterRegistration const& registration,
        TileId tileId) const
    {
        auto const filterKey = detail::filterRequestKey(
            registration.definition.filterId_,
            registration.definition.generation_);
        auto const key = makeFilterRequestedTileKey(
            MapTileKey(
                REQUEST_TILE_LAYER_TYPE,
                registration.mapId,
                registration.layerId,
                tileId),
            filterKey);
        return activeFilterOwners_.contains(key) ||
            queuedTileFrameRefCount_.contains(key) || handoffRecords_.contains(key);
    }

    /**
     * Reject a same-generation semantic mutation while an output overlaps.
     *
     * Disjoint pending sets may reuse a client identity because no active,
     * queued, or handed-off output can be confused with the new definition.
     */
    [[nodiscard]] std::optional<std::string> validateFilterRegistrationOverlapLocked(
        FilterRegistrationState const& nextState) const
    {
        for (auto const& [subscriptionKey, next] : nextState) {
            auto const previous = filterRegistrations_.find(subscriptionKey);
            if (previous == filterRegistrations_.end()) {
                continue;
            }
            auto const firstOverlap = std::ranges::find_if(
                next.pendingTileIds,
                [&](TileId tileId)
                {
                    return previous->second.pendingTileIds.contains(tileId) &&
                        ownsRegisteredFilterOutputLocked(previous->second, tileId);
                });
            if (firstOverlap == next.pendingTileIds.end()) {
                continue;
            }
            if (!sameFilterDefinition(previous->second, next)) {
                return fmt::format(
                    "filter subscription {} changed while tile {} remained pending; advance generation",
                    subscriptionKey,
                    firstOverlap->value());
            }
            for (auto const& tileId : next.pendingTileIds) {
                if (!previous->second.pendingTileIds.contains(tileId) ||
                    !ownsRegisteredFilterOutputLocked(previous->second, tileId))
                {
                    continue;
                }
                auto const emptyRoots = std::vector<FeatureLayerFilterRoot>{};
                auto const previousRoots = previous->second.rootsByTile.find(tileId);
                auto const nextRoots = next.rootsByTile.find(tileId);
                auto const& previousValues = previousRoots == previous->second.rootsByTile.end()
                    ? emptyRoots
                    : previousRoots->second;
                auto const& nextValues = nextRoots == next.rootsByTile.end()
                    ? emptyRoots
                    : nextRoots->second;
                if (!sameFilterRoots(previousValues, nextValues)) {
                    return fmt::format(
                        "filter subscription {} changed while tile {} remained pending; advance generation",
                        subscriptionKey,
                        tileId.value());
                }
            }
        }
        return std::nullopt;
    }

    /** Register one immutable definition and exact-root set per pending output. */
    void registerFilterRequest(
        detail::ParsedLayerTilesRequest const& request,
        FilterRegistrationState& nextState)
    {
        if (!request.filterRequest) {
            return;
        }
        auto const subscriptionKey = detail::filterSubscriptionKey(
            request.filterRequest->filterId_,
            request.filterRequest->generation_);
        auto [registration, inserted] = nextState.try_emplace(
            subscriptionKey,
            FilterRegistration{
                request.mapId,
                request.layerId,
                request.sourceId,
                *request.filterRequest,
                {},
                {}});
        if (!inserted) {
            auto const sameDefinition =
                detail::filterRequestToJson(registration->second.definition, false) ==
                detail::filterRequestToJson(*request.filterRequest, false);
            if (registration->second.mapId != request.mapId ||
                registration->second.layerId != request.layerId ||
                registration->second.sourceId != request.sourceId || !sameDefinition)
            {
                throw std::runtime_error(
                    "one filter subscription generation must use one map, layer, source, and definition");
            }
        }
        std::map<TileId, std::vector<FeatureLayerFilterRoot>> requestRoots;
        for (auto const& root : request.exactRoots) {
            requestRoots[root.tileId_].push_back(root);
        }
        for (auto const& tileId : collectFilterTileIds(request)) {
            auto roots = requestRoots.find(tileId);
            auto const emptyRoots = std::vector<FeatureLayerFilterRoot>{};
            auto const& values = roots == requestRoots.end() ? emptyRoots : roots->second;
            auto [knownRoots, rootsInserted] =
                registration->second.rootsByTile.try_emplace(tileId, values);
            if (!rootsInserted && !sameFilterRoots(knownRoots->second, values)) {
                throw std::runtime_error(
                    "one filter subscription generation must use one exact-root set per output tile");
            }
            registration->second.pendingTileIds.insert(tileId);
        }
    }

    /** Build the canonical output identity for one filtered tile. */
    [[nodiscard]] MapTileKey filterRequestedTileKey(
        detail::ParsedLayerTilesRequest const& request,
        TileId tileId) const
    {
        auto const filterKey = detail::filterRequestKey(
            request.filterRequest->filterId_,
            request.filterRequest->generation_);
        return makeRequestedTileKey(
            MapTileKey(
                REQUEST_TILE_LAYER_TYPE,
                request.mapId,
                request.layerId,
                tileId),
            std::optional<std::string_view>(filterKey));
    }

    /** Expand one parsed request into the output keys owned by the transport. */
    [[nodiscard]] std::vector<MapTileKey> requestedTileKeys(
        detail::ParsedLayerTilesRequest const& request) const
    {
        std::vector<MapTileKey> result;
        if (request.filterRequest) {
            auto const tileIds = collectFilterTileIds(request);
            result.reserve(tileIds.size());
            for (auto const& tileId : tileIds) {
                result.push_back(filterRequestedTileKey(request, tileId));
            }
            return result;
        }
        auto expanded = detail::expandLayerTilesRequestKeys(
            request,
            REQUEST_TILE_LAYER_TYPE);
        result.reserve(expanded.size());
        for (auto& key : expanded) {
            result.push_back(makeRequestedTileKey(std::move(key), std::nullopt));
        }
        return result;
    }

    /** Remove semantic-TTL-expired handoffs before classifying a new snapshot. */
    void expireHandoffRecordsLocked(std::chrono::system_clock::time_point now)
    {
        auto const previousSize = handoffRecords_.size();
        std::erase_if(
            handoffRecords_,
            [&](auto const& item) { return item.second.expiresAt && *item.second.expiresAt <= now; });
        gTilesWsMetrics.expiredHandoffRecords.fetch_add(
            static_cast<int64_t>(previousSize - handoffRecords_.size()),
            std::memory_order_relaxed);
    }

    /** Recompute logical request status from current per-output ownership. */
    void refreshRequestStatusesLocked()
    {
        requestStatuses_.resize(requestInfos_.size(), RequestStatus::Success);
        for (size_t index = 0; index < requestInfos_.size(); ++index) {
            auto const& info = requestInfos_[index];
            if (info.admissionStatus != RequestStatus::Success) {
                requestStatuses_[index] = info.admissionStatus;
                continue;
            }

            auto status = RequestStatus::Success;
            for (auto const& key : info.pendingKeys) {
                if (activeTileOwners_.contains(key) || activeFilterOwners_.contains(key)) {
                    status = RequestStatus::Open;
                    break;
                }
                if (queuedTileFrameRefCount_.contains(key) || handoffRecords_.contains(key)) {
                    continue;
                }
                if (auto terminal = terminalOutputStatuses_.find(key);
                    terminal != terminalOutputStatuses_.end())
                {
                    if (terminal->second != RequestStatus::Success) {
                        status = terminal->second;
                    }
                    continue;
                }
                // A key with no owner has not reached a backend terminal state.
                status = RequestStatus::Open;
                break;
            }
            requestStatuses_[index] = status;
        }
    }

    /** Return whether a filter request still belongs to the current snapshot. */
    [[nodiscard]] bool isCurrentFilterRequestLocked(
        FeatureLayerFilterTilesRequest::Ptr const& request) const
    {
        return std::ranges::find(activeFilterRequests_, request) != activeFilterRequests_.end();
    }

    /** Attach output-owner-aware callbacks to one backend filter request. */
    void attachFilterRequestCallbacks(FeatureLayerFilterTilesRequest::Ptr const& request)
    {
        auto const weak = weak_from_this();
        auto const weakRequest = std::weak_ptr<FeatureLayerFilterTilesRequest>(request);
        request->onFilterResult([weak, weakRequest](TileSubsetLayer::Ptr layer) {
            if (auto self = weak.lock()) {
                if (auto owner = weakRequest.lock()) {
                    self->onTileLayer(std::move(layer), owner);
                }
            }
        });
        request->onStatus([weak, weakRequest](nlohmann::json const& status) {
            if (auto self = weak.lock()) {
                if (auto owner = weakRequest.lock()) {
                    self->onFilterProgress(owner, status);
                }
            }
        });
        request->onDone_ = [weak, weakRequest](RequestStatus status) {
            if (auto self = weak.lock()) {
                if (auto owner = weakRequest.lock()) {
                    self->onFilterRequestDone(owner, status);
                }
            }
        };
    }

    /** Construct finite filter work for additions in one snapshot request. */
    [[nodiscard]] FeatureLayerFilterTilesRequest::Ptr makeFilterBackendRequest(
        detail::ParsedLayerTilesRequest const& parsedRequest,
        std::vector<TileId> const& tileIds)
    {
        auto priorityTileIds = prioritiesWithin(tileIds, parsedRequest.priorityTileIds);
        auto const membership = std::set<TileId>(tileIds.begin(), tileIds.end());
        auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
            parsedRequest.mapId,
            parsedRequest.layerId,
            tileIds,
            *parsedRequest.filterRequest,
            priorityTileIds);
        request->sourceId_ = parsedRequest.sourceId;
        request->setWorkAdmissionGate(workAdmissionOpen_);
        request->exactRoots_ = parsedRequest.exactRoots;
        std::erase_if(
            request->exactRoots_,
            [&](FeatureLayerFilterRoot const& root) { return !membership.contains(root.tileId_); });
        attachFilterRequestCallbacks(request);
        return request;
    }

    /** Attach output-owner-aware callbacks to one ordinary backend request. */
    void attachLayerRequestCallbacks(LayerTilesRequest::Ptr const& request)
    {
        auto const weak = weak_from_this();
        auto const weakRequest = std::weak_ptr<LayerTilesRequest>(request);
        request->onFeatureLayer([weak, weakRequest](TileFeatureLayer::Ptr layer) {
            if (auto self = weak.lock()) {
                if (auto owner = weakRequest.lock()) {
                    self->onTileLayer(std::move(layer), owner);
                }
            }
        });
        request->onSourceDataLayer([weak, weakRequest](TileSourceDataLayer::Ptr layer) {
            if (auto self = weak.lock()) {
                if (auto owner = weakRequest.lock()) {
                    self->onTileLayer(std::move(layer), owner);
                }
            }
        });
        if (EMIT_LOAD_STATE_FRAMES) {
            request->onLayerLoadStateChanged([weak](MapTileKey const& key, TileLayer::LoadState state) {
                if (auto self = weak.lock()) {
                    self->onLoadStateChanged(key, state);
                }
            });
        }
        request->onDone_ = [weak, weakRequest](RequestStatus status) {
            if (auto self = weak.lock()) {
                if (auto owner = weakRequest.lock()) {
                    self->onRequestDone(owner, status);
                }
            }
        };
    }

    /** Construct finite ordinary tile work for additions in one snapshot request. */
    [[nodiscard]] LayerTilesRequest::Ptr makeLayerBackendRequest(
        detail::ParsedLayerTilesRequest const& parsedRequest,
        std::vector<TileId> const& tileIds)
    {
        auto priorityTileIds = prioritiesWithin(tileIds, parsedRequest.priorityTileIds);
        auto const membership = std::set<TileId>(tileIds.begin(), tileIds.end());
        auto request = std::make_shared<LayerTilesRequest>(
            parsedRequest.mapId,
            parsedRequest.layerId,
            tileIds,
            priorityTileIds);
        request->sourceId_ = parsedRequest.sourceId;
        request->setWorkAdmissionGate(workAdmissionOpen_);
        request->featureIdsByTile_ = parsedRequest.featureIdsByTile;
        std::erase_if(
            request->featureIdsByTile_,
            [&](auto const& item) { return !membership.contains(item.first); });
        attachLayerRequestCallbacks(request);
        return request;
    }

    /** Submit newly created backend work and abort it if the service rejects the batch. */
    void submitBackendRequests(
        const std::vector<LayerTilesRequest::Ptr>& serviceRequests,
        const std::vector<FeatureLayerFilterTilesRequest::Ptr>& filterServiceRequests)
    {
        bool serviceRequestsAccepted = true;
        if (!serviceRequests.empty()) {
            serviceRequestsAccepted = service_.request(serviceRequests, authHeaders_);
        }
        if (serviceRequestsAccepted && !filterServiceRequests.empty()) {
            serviceRequestsAccepted = service_.request(filterServiceRequests, authHeaders_);
        }
        if (!serviceRequestsAccepted) {
            abortRequests(serviceRequests);
            abortFilterRequests(filterServiceRequests);
        }
    }

    /** Increment queued/sent reference counters for one canonical tile key. */
    void incrementFrameRefCount(std::map<MapTileKey, int64_t>& counts, const MapTileKey& key)
    {
        counts[key] += 1;
    }

    /** Decrement queued/sent reference counters and erase exhausted entries. */
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

    /** Mark a frame as queued so request updates can avoid duplicate backend fetches. */
    void trackQueuedFrameLocked(OutgoingFrame& frame)
    {
        queuedOutgoingBytes_ += frame.bytes.size();
        frame.trackedCapacityBytes = frame.bytes.capacity();
        auto const allocated = static_cast<int64_t>(frame.trackedCapacityBytes);
        auto const current = gTilesWsMetrics.pendingAllocatedBytes.fetch_add(
            allocated,
            std::memory_order_relaxed) + allocated;
        auto peak = gTilesWsMetrics.peakPendingAllocatedBytes.load(std::memory_order_relaxed);
        while (peak < current &&
               !gTilesWsMetrics.peakPendingAllocatedBytes.compare_exchange_weak(
                   peak,
                   current,
                   std::memory_order_relaxed)) {
        }
        if (frame.requestedTileKey) {
            incrementFrameRefCount(queuedTileFrameRefCount_, *frame.requestedTileKey);
        }
    }

    /** Remove a frame from queued bookkeeping once it is dequeued or dropped. */
    void untrackQueuedFrameLocked(const OutgoingFrame& frame)
    {
        const auto frameBytes = frame.bytes.size();
        queuedOutgoingBytes_ = frameBytes > queuedOutgoingBytes_ ? 0 : queuedOutgoingBytes_ - frameBytes;
        gTilesWsMetrics.pendingAllocatedBytes.fetch_sub(
            static_cast<int64_t>(frame.trackedCapacityBytes),
            std::memory_order_relaxed);
        if (frame.requestedTileKey) {
            decrementFrameRefCount(queuedTileFrameRefCount_, *frame.requestedTileKey);
        }
    }

    /** Pop the highest-priority queued tile frame and account forwarding metrics. */
    [[nodiscard]] PullFrameResult popNextFrameLocked()
    {
        if (outgoing_.empty()) {
            return PullFrameResult{.status = PullFrameResult::Status::Timeout};
        }

        auto frame = std::move(outgoing_.front());
        outgoing_.pop_front();
        untrackQueuedFrameLocked(frame);
        refreshWorkAdmissionLocked();
        if (frame.stringPoolCommit) {
            committedStringPoolOffsets_[frame.stringPoolCommit->first] = frame.stringPoolCommit->second;
        }
        if (frame.requestedTileKey && isTileDataMessage(frame.type)) {
            // The first dequeue establishes the immutable semantic handoff
            // deadline. Repeated snapshots must never extend it.
            handoffRecords_.try_emplace(
                *frame.requestedTileKey,
                HandoffRecord{frame.handoffExpiry});
        }

        const auto frameBytes = static_cast<int64_t>(frame.bytes.size());
        gTilesWsMetrics.totalForwardedFrames.fetch_add(1, std::memory_order_relaxed);
        gTilesWsMetrics.totalForwardedBytes.fetch_add(frameBytes, std::memory_order_relaxed);

        return PullFrameResult{
            .status = PullFrameResult::Status::Frame,
            .frameBytes = std::move(frame.bytes),
        };
    }

    /** Pop and concatenate queued frames up to one batch byte budget. */
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

    /** Pop the next valid waiter in arrival order, skipping stale order entries. */
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

    /** Remove one waiter id from the FIFO order list. */
    void erasePullWaiterOrderEntryLocked(uint64_t waiterId)
    {
        pendingPullWaiterOrder_.erase(
            std::remove(pendingPullWaiterOrder_.begin(), pendingPullWaiterOrder_.end(), waiterId),
            pendingPullWaiterOrder_.end());
    }

    /** Complete queued pull waiters while both waiters and frames are available. */
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

    /** Match one backend-produced tile key against the current pending snapshot. */
    [[nodiscard]] std::optional<MapTileKey> matchPendingTileKeyLocked(
        MapTileKey key,
        std::optional<std::string_view> filterKey = std::nullopt) const
    {
        auto requestedTileKey = makeRequestedTileKey(std::move(key), filterKey);
        if (pendingTileKeys_.contains(requestedTileKey)) {
            return requestedTileKey;
        }

        return std::nullopt;
    }

    /** Complete all currently pending pull waiters with one terminal status. */
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

    /** Dispatch one completed pull callback on Drogon's loop. */
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

    /** Dispatch a batch of completed pull callbacks on Drogon's loop. */
    static void dispatchPullResults(std::vector<PullDispatch> dispatches)
    {
        for (auto& dispatch : dispatches) {
            dispatchPullResult(std::move(dispatch.callback), std::move(dispatch.result));
        }
    }

    /** Resolve one waiter with timeout if it is still pending. */
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

    /** Look up the current priority rank for one tile key, defaulting to lowest priority. */
    [[nodiscard]] int64_t tilePriorityRankLocked(const MapTileKey& tileKey) const
    {
        const auto it = tilePriorityRanks_.find(tileKey);
        if (it == tilePriorityRanks_.end()) {
            return LOWEST_TILE_PRIORITY;
        }
        return it->second;
    }

    /** Refresh one queued frame's cached priority rank against the latest request priorities. */
    void refreshFramePriorityLocked(OutgoingFrame& frame) const
    {
        if (!frame.requestedTileKey) {
            frame.priorityRank = LOWEST_TILE_PRIORITY;
            return;
        }
        frame.priorityRank = tilePriorityRankLocked(*frame.requestedTileKey);
    }

    /** Compare two frames for queue order; returns true if lhs should be sent before rhs. */
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

    /** Return whether the outbox is below its soft backend-work admission watermark. */
    [[nodiscard]] bool hasBackendWorkCapacityLocked() const
    {
        return outgoing_.size() < OUTGOING_FRAME_ADMISSION_WATERMARK &&
            queuedOutgoingBytes_ < OUTGOING_BYTE_ADMISSION_WATERMARK;
    }

    /** Publish a changed admission state and wake workers when this session becomes writable. */
    void refreshWorkAdmissionLocked()
    {
        auto const open = !cancelled_.load(std::memory_order_acquire) &&
            hasBackendWorkCapacityLocked();
        auto const wasOpen = workAdmissionOpen_->exchange(open, std::memory_order_release);
        if (!wasOpen && open) {
            service_.notifyWorkAvailable();
        }
    }

    /** Drop queued tile data frames omitted from the latest pending snapshot. */
    void filterOutgoingByPendingLocked()
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
                && !pendingTileKeys_.contains(*frame.requestedTileKey);
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
            refreshWorkAdmissionLocked();
        }
    }

    /** Reorder queued frames according to string-pool and tile-priority policy. */
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

    /** Append one frame to the websocket controller queue and update counters. */
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
        refreshWorkAdmissionLocked();
        gTilesWsMetrics.totalQueuedFrames.fetch_add(1, std::memory_order_relaxed);
        gTilesWsMetrics.totalQueuedBytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    /** Drop all queued frames and account them as controller-side drops. */
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
        refreshWorkAdmissionLocked();
    }

    /** Internal cancel path used by destructor/connection tear-down (no status emission). */
    void cancelNoStatus()
    {
        if (cancelled_.exchange(true))
            return;
        workAdmissionOpen_->store(false, std::memory_order_release);
        std::vector<LayerTilesRequest::Ptr> requestsToAbort;
        std::vector<FeatureLayerFilterTilesRequest::Ptr> filterRequestsToAbort;
        std::vector<PullDispatch> pullDispatches;

        // Ensure we stop emitting any further frames.
        {
            std::lock_guard lock(mutex_);
            clearOutgoingLocked();
            requestsToAbort = std::move(activeRequests_);
            activeRequests_.clear();
            filterRequestsToAbort = std::move(activeFilterRequests_);
            activeFilterRequests_.clear();
            activeTileOwners_.clear();
            activeFilterOwners_.clear();
            terminalOutputStatuses_.clear();
            pendingTileKeys_.clear();
            handoffRecords_.clear();
            filterRegistrations_.clear();
            stagedRequest_.reset();
            pendingReconciliation_.reset();
            collectAllPullWaitersLocked(PullFrameResult::Status::Closed, pullDispatches);
        }

        abortRequests(std::move(requestsToAbort));
        abortFilterRequests(std::move(filterRequestsToAbort));
        dispatchPullResults(std::move(pullDispatches));
    }

    /** Abort a batch of backend requests outside `mutex_` to avoid lock inversion. */
    void abortRequests(std::vector<LayerTilesRequest::Ptr> requests)
    {
        for (auto const& request : requests) {
            if (!request || request->isDone()) {
                continue;
            }
            service_.abort(request);
        }
    }

    /** Abort a batch of backend filter requests outside `mutex_` to avoid lock inversion. */
    void abortFilterRequests(std::vector<FeatureLayerFilterTilesRequest::Ptr> requests)
    {
        for (auto const& request : requests) {
            if (!request || request->isDone()) {
                continue;
            }
            service_.abort(request);
        }
    }

    /** Collect writer callbacks generated while serializing one tile layer. */
    void onWriterMessage(std::string msg, TileLayerStream::MessageType type)
    {
        // Writer messages are only generated from within onTileLayer under mutex_.
        if (!currentWriteBatch_.has_value()) {
            raise("TilesWsSession writer callback used out-of-band");
        }
        currentWriteBatch_->push_back(WriterMessage{std::move(msg), type});
    }

    /** Convert one owner-current backend result into queued transport frames. */
    template<typename Request>
    void onTileLayer(TileLayer::Ptr const& layer, std::shared_ptr<Request> const& owner)
    {
        if (cancelled_ || !layer || !owner) {
            return;
        }

        try {
            std::optional<std::pair<std::string, simfil::StringId>> stringPoolCommit;
            std::vector<PullDispatch> pullDispatches;
            {
                std::lock_guard lock(mutex_);
                auto filterKey = filterRequestKey(layer);
                auto requestedTileKey = matchPendingTileKeyLocked(
                    layer->id(),
                    filterKey ? std::optional<std::string_view>(*filterKey) : std::nullopt);
                if (!requestedTileKey) {
                    gTilesWsMetrics.obsoleteOwnerCallbacks.fetch_add(1, std::memory_order_relaxed);
                    return;
                }

                auto ownerIsCurrent = [&]() {
                    if constexpr (std::is_same_v<Request, LayerTilesRequest>) {
                        auto current = activeTileOwners_.find(*requestedTileKey);
                        return current != activeTileOwners_.end() && current->second == owner;
                    }
                    else {
                        auto current = activeFilterOwners_.find(*requestedTileKey);
                        return current != activeFilterOwners_.end() && current->second == owner;
                    }
                };
                if (!ownerIsCurrent()) {
                    gTilesWsMetrics.obsoleteOwnerCallbacks.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (queuedTileFrameRefCount_.contains(*requestedTileKey) ||
                    handoffRecords_.contains(*requestedTileKey))
                {
                    return;
                }
                if (currentWriteBatch_) {
                    raise("TilesWsSession writer callback re-entered");
                }
                currentWriteBatch_.emplace();
                writer_->write(layer);
                auto batch = std::move(*currentWriteBatch_);
                currentWriteBatch_.reset();

                auto const stringPoolId = layer->stringPoolId();
                if (auto offset = writerOffsets_.find(stringPoolId); offset != writerOffsets_.end()) {
                    for (auto const& message : batch) {
                        if (message.type == TileLayerStream::MessageType::StringPool) {
                            stringPoolCommit = std::make_pair(stringPoolId, offset->second);
                            break;
                        }
                    }
                }

                std::optional<std::chrono::system_clock::time_point> handoffExpiry;
                if (auto const ttl = layer->ttl(); ttl && ttl->count() > 0) {
                    handoffExpiry = layer->timestamp() + *ttl;
                }
                for (auto& message : batch) {
                    OutgoingFrame frame;
                    frame.bytes = std::move(message.bytes);
                    frame.type = message.type;
                    if (message.type == TileLayerStream::MessageType::StringPool) {
                        frame.stringPoolCommit = stringPoolCommit;
                    }
                    if (isTileDataMessage(message.type)) {
                        frame.requestedTileKey = *requestedTileKey;
                        frame.handoffExpiry = handoffExpiry;
                    }
                    enqueueOutgoingLocked(std::move(frame));
                }

                if constexpr (std::is_same_v<Request, LayerTilesRequest>) {
                    activeTileOwners_.erase(*requestedTileKey);
                }
                else {
                    activeFilterOwners_.erase(*requestedTileKey);
                }
                // Logical status is observed only through status frames, which
                // are emitted when a backend request becomes terminal. A full
                // coverage rescan here would serialize every result callback
                // behind the session mutex and turn O(outputs) delivery into
                // O(outputs * logical-coverage).
                drainReadyPullWaitersLocked(pullDispatches);
            }
            dispatchPullResults(std::move(pullDispatches));
        }
        catch (std::exception const& e) {
            log().error("Failed to stream tile layer: {}", e.what());
            cancelNoStatus();
        }
    }

    /** Retire one ordinary backend owner without touching reassigned outputs. */
    void onRequestDone(
        LayerTilesRequest::Ptr const& completedRequest,
        RequestStatus status)
    {
        if (cancelled_) {
            return;
        }
        bool shouldEmit = false;
        {
            std::lock_guard lock(mutex_);
            auto const removedRequests = std::erase_if(
                activeRequests_,
                [&](LayerTilesRequest::Ptr const& request)
                { return !request || request == completedRequest || request->isDone(); });
            shouldEmit = removedRequests > 0;
            for (auto owner = activeTileOwners_.begin(); owner != activeTileOwners_.end();) {
                if (owner->second != completedRequest) {
                    ++owner;
                    continue;
                }
                terminalOutputStatuses_[owner->first] = status;
                owner = activeTileOwners_.erase(owner);
                shouldEmit = true;
            }
            refreshRequestStatusesLocked();
            shouldEmit = shouldEmit && statusEmissionEnabled_;
        }
        if (shouldEmit) {
            queueStatusMessage({});
        }
    }

    /** Forward filter progress only while that backend request owns current work. */
    void onFilterProgress(
        FeatureLayerFilterTilesRequest::Ptr const& owner,
        nlohmann::json const& status)
    {
        {
            std::lock_guard lock(mutex_);
            if (cancelled_ || !isCurrentFilterRequestLocked(owner)) {
                return;
            }
        }
        sendControlMessage(
            TileLayerStream::MessageType::Status,
            status.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore));
    }

    /** Retire one filter backend owner without touching reassigned outputs. */
    void onFilterRequestDone(
        FeatureLayerFilterTilesRequest::Ptr const& completedRequest,
        RequestStatus status)
    {
        if (cancelled_) {
            return;
        }
        bool shouldEmit = false;
        {
            std::lock_guard lock(mutex_);
            auto const removedRequests = std::erase_if(
                activeFilterRequests_,
                [&](FeatureLayerFilterTilesRequest::Ptr const& request)
                { return !request || request == completedRequest || request->isDone(); });
            shouldEmit = removedRequests > 0;
            for (auto owner = activeFilterOwners_.begin(); owner != activeFilterOwners_.end();) {
                if (owner->second != completedRequest) {
                    ++owner;
                    continue;
                }
                terminalOutputStatuses_[owner->first] = status;
                owner = activeFilterOwners_.erase(owner);
                shouldEmit = true;
            }
            refreshRequestStatusesLocked();
            shouldEmit = shouldEmit && statusEmissionEnabled_;
        }
        if (shouldEmit) {
            queueStatusMessage({});
        }
    }

    /** Send an already encoded control payload on Drogon's event loop. */
    void sendControlMessageOnLoop(TileLayerStream::MessageType type, std::string payload)
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

    /** Queue websocket control frames so backend worker callbacks never send concurrently. */
    void sendControlMessage(TileLayerStream::MessageType type, std::string payload)
    {
        drogon::app().getLoop()->queueInLoop(
            [weak = weak_from_this(), type, payload = std::move(payload)]() mutable {
                if (auto self = weak.lock()) {
                    self->sendControlMessageOnLoop(type, std::move(payload));
                }
            });
    }

    /** Send a status frame describing the current request statuses. */
    void queueStatusMessage(std::string message)
    {
        sendControlMessage(TileLayerStream::MessageType::Status, buildStatusPayload(std::move(message)));
    }

    /** Send a request-context frame so the client can track the active request id + client id. */
    void queueRequestContextMessage()
    {
        sendControlMessage(TileLayerStream::MessageType::RequestContext, buildRequestContextPayload());
    }

    /** Send a datasource-catalog invalidation frame for this interactive session. */
    void queueSourceCatalogChangeMessage(DataSourceCatalogChange const& change)
    {
        sendControlMessage(TileLayerStream::MessageType::SourceCatalogChange, buildSourceCatalogChangePayload(change));
    }

    /** Forward backend tile load-state changes for tiles still requested by the client. */
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
            if (!pendingTileKeys_.contains(requestedTileKey)) {
                return;
            }
        }

        sendControlMessage(
            TileLayerStream::MessageType::LoadStateChange,
            buildLoadStatePayload(key, state));
    }

    /** Build the JSON payload for `mapget.tiles.status`. */
    [[nodiscard]] std::string buildStatusPayload(std::string message)
    {
        nlohmann::json requestsJson = nlohmann::json::array();
        bool allDone = true;
        uint64_t requestId = 0;

        {
            std::lock_guard lock(mutex_);
            requestId = requestId_;
            for (size_t i = 0; i < requestInfos_.size(); ++i) {
                const auto status = (i < requestStatuses_.size()) ? requestStatuses_[i] : RequestStatus::Open;
                allDone &= (status != RequestStatus::Open);

                nlohmann::json reqJson = nlohmann::json::object();
                reqJson["index"] = i;
                reqJson["mapId"] = requestInfos_[i].mapId;
                reqJson["layerId"] = requestInfos_[i].layerId;
                reqJson["status"] = static_cast<std::underlying_type_t<RequestStatus>>(status);
                reqJson["statusText"] = std::string(requestStatusToString(status));
                if (status == RequestStatus::NoDataSource) {
                    auto reason = noDataSourceReasonToString(requestInfos_[i].noDataSourceReason);
                    if (!reason.empty()) {
                        reqJson["noDataSourceReason"] = std::string(reason);
                    }
                }
                requestsJson.push_back(std::move(reqJson));
            }
        }

        return nlohmann::json::object({
            {"type", "mapget.tiles.status"},
            {"requestId", requestId},
            {"allDone", allDone},
            {"requests", std::move(requestsJson)},
            {"message", std::move(message)},
        }).dump();
    }

    /** Build the JSON payload for `mapget.tiles.load-state`. */
    [[nodiscard]] std::string buildLoadStatePayload(MapTileKey const& key, TileLayer::LoadState state) const
    {
        uint64_t requestId = 0;
        {
            std::lock_guard lock(mutex_);
            requestId = requestId_;
        }
        return nlohmann::json::object({
            {"type", "mapget.tiles.load-state"},
            {"requestId", requestId},
            {"mapId", key.mapId_},
            {"layerId", key.layerId_},
            {"tileId", key.tileId_.value()},
            {"state", static_cast<uint8_t>(state)},
            {"stateText", std::string(loadStateToString(state))},
        }).dump();
    }

    /** Build the JSON payload for `mapget.tiles.request-context`. */
    [[nodiscard]] std::string buildRequestContextPayload() const
    {
        uint64_t requestId = 0;
        {
            std::lock_guard lock(mutex_);
            requestId = requestId_;
        }
        return nlohmann::json::object({
            {"type", "mapget.tiles.request-context"},
            {"requestId", requestId},
            {"clientId", clientId_},
            {"sourcesRevision", service_.sourceCatalogRevision()},
        }).dump();
    }

    /** Build the JSON payload for `mapget.sources.changed`. */
    [[nodiscard]] std::string buildSourceCatalogChangePayload(DataSourceCatalogChange const& change) const
    {
        auto payload = nlohmann::json::object({
            {"type", "mapget.sources.changed"},
            {"revision", change.revision},
            {"reason", change.reason},
        });

        // Only expose per-source details when the websocket client could also
        // see the same catalog row through `/sources`; otherwise keep the old
        // generic invalidation shape and avoid leaking auth-scoped metadata.
        if (change.sourceUpdate
            && service_.isSourceCatalogChangeVisible(change, std::optional<AuthHeaders>{authHeaders_}))
        {
            auto const& update = *change.sourceUpdate;
            auto source = nlohmann::json::object({
                {"configIndex", update.descriptor.configIndex},
                {"sourceId", update.descriptor.sourceId},
                {"type", update.descriptor.type},
                {"status", std::string(catalogStatusToString(update.status))},
                {"statusMessage", update.statusMessage},
                {"addOn", update.descriptor.addOn},
                {"progress", update.progress ? nlohmann::json(*update.progress) : nlohmann::json(nullptr)},
            });
            payload["source"] = std::move(source);
        }
        return payload.dump();
    }

    HttpService& service_;
    std::weak_ptr<drogon::WebSocketConnection> conn_;
    int64_t clientId_ = gNextClientId.fetch_add(1, std::memory_order_relaxed);
    uint64_t requestId_ = 0;
    uint64_t nextRequestId_ = 1;

    AuthHeaders authHeaders_;

    mutable std::mutex mutex_;
    std::shared_ptr<std::atomic_bool> workAdmissionOpen_ = std::make_shared<std::atomic_bool>(true);
    uint64_t nextPullWaiterId_ = 1;
    std::deque<uint64_t> pendingPullWaiterOrder_;
    std::unordered_map<uint64_t, PullWaiter> pendingPullWaiters_;
    std::deque<OutgoingFrame> outgoing_;
    size_t queuedOutgoingBytes_ = 0;
    std::vector<RequestInfo> requestInfos_;
    std::vector<RequestStatus> requestStatuses_;
    std::vector<LayerTilesRequest::Ptr> activeRequests_;
    std::vector<FeatureLayerFilterTilesRequest::Ptr> activeFilterRequests_;
    std::map<MapTileKey, LayerTilesRequest::Ptr> activeTileOwners_;
    std::map<MapTileKey, FeatureLayerFilterTilesRequest::Ptr> activeFilterOwners_;
    std::map<MapTileKey, RequestStatus> terminalOutputStatuses_;
    std::set<MapTileKey> pendingTileKeys_;
    std::map<MapTileKey, int64_t> tilePriorityRanks_;
    std::map<MapTileKey, int64_t> queuedTileFrameRefCount_;
    std::map<MapTileKey, HandoffRecord> handoffRecords_;
    FilterRegistrationState filterRegistrations_;
    bool statusEmissionEnabled_ = false;
    std::optional<StagedRequest> stagedRequest_;
    std::optional<PendingReconciliation> pendingReconciliation_;
    bool reconciliationTaskScheduled_ = false;
    uint64_t reconciliationSequence_ = 0;
    std::atomic_uint64_t latestReconciliationSequence_{0};

    TileLayerStream::StringPoolOffsetMap committedStringPoolOffsets_;
    TileLayerStream::StringPoolOffsetMap writerOffsets_;
    std::unique_ptr<TileLayerStream::Writer> writer_;
    std::optional<std::vector<WriterMessage>> currentWriteBatch_;
    std::optional<Service::DataSourceCatalogSubscription> sourceCatalogSubscription_;

    std::atomic_bool cancelled_{false};
};


namespace
{

/** Look up a live session by client id and prune expired registry entries. */
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

/** Parse an integer query parameter, returning a bounded default on absence or parse failure. */
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

/** Handle one `/interactive/payload` long-poll request for queued websocket tile frames. */
void handleTilesNextRequest(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    gTilesWsMetrics.totalPullRequests.fetch_add(1, std::memory_order_relaxed);

    const auto clientId = parseClampedInt64Parameter(req, "clientId", 0, 0, std::numeric_limits<int64_t>::max());
    if (clientId <= 0) {
        // The pull endpoint cannot infer a session without the websocket-provided id.
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k400BadRequest);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody("Missing or invalid clientId parameter.");
        callback(resp);
        return;
    }

    auto session = findSessionByClientId(clientId);
    if (!session) {
        // The websocket was closed or expired; clients must reconnect and start a new session.
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


/** Forward auth-header extraction through the opaque-session adapter boundary. */
AuthHeaders tilesWsAuthHeadersFromRequest(const drogon::HttpRequestPtr& req)
{
    AuthHeaders headers;
    for (auto const& [k, v] : req->headers()) {
        headers.emplace(k, v);
    }
    return headers;
}

/** Encode a stream frame without exposing session internals to the Drogon controller. */
std::string tilesWsEncodeStreamMessage(TileLayerStream::MessageType type, std::string_view payload)
{
    return encodeStreamMessage(type, payload);
}

/** Create the concrete session hidden behind the controller-facing opaque type. */
std::shared_ptr<TilesWsSession> tilesWsCreateSession(
    HttpService& service,
    std::weak_ptr<drogon::WebSocketConnection> conn,
    AuthHeaders authHeaders)
{
    auto session = std::make_shared<TilesWsSession>(service, std::move(conn), std::move(authHeaders));
    session->startSourceCatalogSubscription();
    return session;
}

/** Add one session to the weak metrics list used by status snapshots. */
void tilesWsRegisterForMetrics(const std::shared_ptr<TilesWsSession>& session)
{
    session->registerForMetrics();
}

/** Add one session to the client-id registry used by `/interactive/payload`. */
void tilesWsRegisterSession(const std::shared_ptr<TilesWsSession>& session)
{
    std::lock_guard lock(gSessionRegistryMutex);
    gSessionRegistry[session->clientId()] = session;
}

/** Remove one session from the client-id registry after websocket close. */
void tilesWsUnregisterSession(int64_t clientId)
{
    std::lock_guard lock(gSessionRegistryMutex);
    gSessionRegistry.erase(clientId);
}

/** Return the numeric id assigned to one session, or zero for a missing session. */
int64_t tilesWsSessionClientId(const std::shared_ptr<TilesWsSession>& session)
{
    return session ? session->clientId() : 0;
}

/** Apply a reconnect/resume string-pool offset patch to one session. */
bool tilesWsApplyStringPoolOffsetsPatch(
    const std::shared_ptr<TilesWsSession>& session,
    const nlohmann::json& offsetsJson,
    std::string& errorMessage)
{
    return session && session->applyStringPoolOffsetsPatch(offsetsJson, errorMessage);
}

/** Allocate the request id used to correlate status frames with client updates. */
uint64_t tilesWsAllocateRequestId(
    const std::shared_ptr<TilesWsSession>& session,
    const nlohmann::json& requestJson)
{
    return session ? session->allocateRequestId(requestJson) : 0;
}

/** Apply a parsed websocket request message to one session if it is still alive. */
void tilesWsUpdateFromClientRequestMessage(
    const std::shared_ptr<TilesWsSession>& session,
    nlohmann::json requestJson,
    uint64_t requestId)
{
    if (session) {
        session->updateFromClientRequestMessage(std::move(requestJson), requestId);
    }
}

/** Cancel a session from the controller boundary without exposing the class definition. */
void tilesWsCancel(const std::shared_ptr<TilesWsSession>& session, std::string message)
{
    if (session) {
        session->cancel(std::move(message));
    }
}

/** Increment active websocket connection metrics. */
void tilesWsRecordConnectionOpened()
{
    gTilesWsMetrics.activeConnections.fetch_add(1, std::memory_order_relaxed);
}

/** Decrement active websocket connection metrics. */
void tilesWsRecordConnectionClosed()
{
    gTilesWsMetrics.activeConnections.fetch_sub(1, std::memory_order_relaxed);
}

/** Forward `/interactive/payload` handling through the opaque-session adapter boundary. */
void tilesWsHandleNextRequest(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    handleTilesNextRequest(req, std::move(callback));
}

/** Build the status-data object for websocket queues and pull endpoint counters. */
nlohmann::json tilesWebSocketMetricsSnapshotImpl()
{
    int64_t pendingControllerFrames = 0;
    int64_t pendingControllerBytes = 0;
    int64_t pendingPullRequests = 0;
    int64_t activeOutputKeys = 0;
    int64_t queuedOutputKeys = 0;
    int64_t handoffOutputKeys = 0;
    {
        std::lock_guard lock(gTrackedSessionsMutex);
        auto out = gTrackedSessions.begin();
        for (auto it = gTrackedSessions.begin(); it != gTrackedSessions.end(); ++it) {
            if (auto session = it->lock()) {
                auto [frames, bytes] = session->pendingSnapshot();
                pendingControllerFrames += frames;
                pendingControllerBytes += bytes;
                pendingPullRequests += session->pendingPullRequestCount();
                auto const [active, queued, handoff] = session->ownershipSnapshot();
                activeOutputKeys += active;
                queuedOutputKeys += queued;
                handoffOutputKeys += handoff;
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
        {"pending-controller-allocated-bytes", nonNegative(gTilesWsMetrics.pendingAllocatedBytes)},
        {"peak-pending-controller-allocated-bytes", nonNegative(gTilesWsMetrics.peakPendingAllocatedBytes)},
        {"pending-pull-requests", pendingPullRequests},
        {"active-output-keys", activeOutputKeys},
        {"queued-output-keys", queuedOutputKeys},
        {"handoff-output-keys", handoffOutputKeys},
        {"total-queued-frames", nonNegative(gTilesWsMetrics.totalQueuedFrames)},
        {"total-queued-bytes", nonNegative(gTilesWsMetrics.totalQueuedBytes)},
        {"total-forwarded-frames", nonNegative(gTilesWsMetrics.totalForwardedFrames)},
        {"total-forwarded-bytes", nonNegative(gTilesWsMetrics.totalForwardedBytes)},
        {"total-dropped-frames", nonNegative(gTilesWsMetrics.totalDroppedFrames)},
        {"total-dropped-bytes", nonNegative(gTilesWsMetrics.totalDroppedBytes)},
        {"total-pull-requests", nonNegative(gTilesWsMetrics.totalPullRequests)},
        {"total-pull-timeouts", nonNegative(gTilesWsMetrics.totalPullTimeouts)},
        {"total-pull-session-misses", nonNegative(gTilesWsMetrics.totalPullSessionMisses)},
        {"reconciled-snapshots", nonNegative(gTilesWsMetrics.reconciledSnapshots)},
        {"superseded-snapshots", nonNegative(gTilesWsMetrics.supersededSnapshots)},
        {"suppressed-active-outputs", nonNegative(gTilesWsMetrics.suppressedActiveOutputs)},
        {"suppressed-queued-outputs", nonNegative(gTilesWsMetrics.suppressedQueuedOutputs)},
        {"suppressed-handoff-outputs", nonNegative(gTilesWsMetrics.suppressedHandoffOutputs)},
        {"expired-handoff-records", nonNegative(gTilesWsMetrics.expiredHandoffRecords)},
        {"retained-overlapping-outputs", nonNegative(gTilesWsMetrics.retainedOutputs)},
        {"pruned-outputs", nonNegative(gTilesWsMetrics.prunedOutputs)},
        {"obsolete-owner-callbacks", nonNegative(gTilesWsMetrics.obsoleteOwnerCallbacks)},
    });
}

}  // namespace mapget::detail
