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
    std::atomic<int64_t> replacedRequests{0};
    std::atomic<int64_t> totalPullRequests{0};
    std::atomic<int64_t> totalPullTimeouts{0};
    std::atomic<int64_t> totalPullSessionMisses{0};
    std::atomic<int64_t> pendingAllocatedBytes{0};
    std::atomic<int64_t> peakPendingAllocatedBytes{0};
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
constexpr size_t MAX_QUEUED_OUTGOING_FRAMES = 4096;
constexpr size_t MAX_QUEUED_OUTGOING_BYTES = 256 * 1024 * 1024;
constexpr size_t MAX_SPARSE_RENEWAL_TILES_PER_ENTRY = 512;
constexpr size_t MAX_SPARSE_RENEWAL_TILES_PER_ENVELOPE = 2048;
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

    /** Parse a possibly chunked request message and apply each chunk immediately. */
    void updateFromClientRequestMessage(const nlohmann::json& j, uint64_t requestId)
    {
        if (j.contains("renewals")) {
            renewFromClientRequest(j);
            return;
        }
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

    /** Parse and apply a full logical tile request update from the client. */
    void updateFromClientRequest(
        const nlohmann::json& j,
        uint64_t requestId,
        ClientRequestUpdateMode updateMode,
        bool requestChunksComplete)
    {
        auto requestsIt = j.find("requests");
        if (requestsIt == j.end() || !requestsIt->is_array()) {
            rejectClientRequest(requestId, "Missing or invalid 'requests' array");
            return;
        }

        size_t requestIndexBase = 0;
        if (auto appendError = validateAppendUpdate(updateMode, requestId, requestIndexBase)) {
            rejectClientRequest(requestId, std::move(*appendError));
            return;
        }

        std::vector<LayerTilesRequest::Ptr> serviceRequests;
        std::vector<FeatureLayerFilterTilesRequest::Ptr> filterServiceRequests;
        std::vector<LayerTilesRequest::Ptr> nextActiveRequests;
        std::vector<FeatureLayerFilterTilesRequest::Ptr> nextActiveFilterRequests;
        std::vector<RequestInfo> nextRequestInfos;
        std::vector<RequestStatus> nextRequestStatuses;
        std::set<MapTileKey> desiredTileKeys;
        std::map<MapTileKey, int64_t> nextTilePriorityRanks;
        FilterDeliveryEpochState nextFilterDeliveryEpochs;
        FilterAdmissibleEpochState nextFilterAdmissibleEpochs;
        FilterRegistrationState nextFilterRegistrations;
        if (updateMode == ClientRequestUpdateMode::Append) {
            std::lock_guard lock(mutex_);
            nextFilterDeliveryEpochs = filterDeliveryEpochs_;
            nextFilterAdmissibleEpochs = filterAdmissibleEpochs_;
            nextFilterRegistrations = filterRegistrations_;
        }

        try {
            nextRequestInfos.reserve(requestsIt->size());
            nextRequestStatuses.reserve(requestsIt->size());

            for (size_t index = 0; index < requestsIt->size(); ++index) {
                auto effectiveRequestJson = requestsIt->at(index);
                detail::inheritFilterFields(effectiveRequestJson, j);
                auto parsedRequest = detail::parseLayerTilesRequestJson(effectiveRequestJson);
                if (parsedRequest.filterRequest) {
                    registerFilterRequest(
                        parsedRequest,
                        nextFilterRegistrations);
                    reconcileFilterDeliveryEpochs(
                        parsedRequest,
                        nextFilterDeliveryEpochs,
                        nextFilterAdmissibleEpochs);
                }
                auto layerContext = service_.resolveLayerRequest(
                    parsedRequest.mapId,
                    parsedRequest.layerId,
                    authHeaders_,
                    parsedRequest.sourceId);

                nextRequestInfos.push_back(RequestInfo{
                    .mapId = parsedRequest.mapId,
                    .layerId = parsedRequest.layerId,
                    .noDataSourceReason = layerContext.noDataSourceReason_,
                });
                nextRequestStatuses.push_back(RequestStatus::Success);
                addDesiredTileKeys(
                    parsedRequest,
                    nextFilterAdmissibleEpochs,
                    desiredTileKeys,
                    nextTilePriorityRanks);

                const auto statusIndex = requestIndexBase + index;
                if (parsedRequest.filterRequest) {
                    auto request = makeFilterBackendRequestIfNeeded(parsedRequest, requestId, statusIndex);
                    if (request) {
                        filterServiceRequests.push_back(request);
                        nextActiveFilterRequests.push_back(std::move(request));
                        nextRequestStatuses[index] = RequestStatus::Open;
                    }
                    continue;
                }

                auto request = makeLayerBackendRequestIfNeeded(parsedRequest, requestId, statusIndex);
                if (request) {
                    serviceRequests.push_back(request);
                    nextActiveRequests.push_back(std::move(request));
                    nextRequestStatuses[index] = RequestStatus::Open;
                }
            }
        }
        catch (const std::exception& e) {
            rejectClientRequest(requestId, fmt::format("Invalid request JSON: {}", e.what()));
            return;
        }

        std::vector<LayerTilesRequest::Ptr> replacedRequests;
        std::vector<FeatureLayerFilterTilesRequest::Ptr> replacedFilterRequests;
        {
            std::lock_guard lock(mutex_);
            if (updateMode == ClientRequestUpdateMode::Append && requestId_ == requestId) {
                appendActiveRequestStateLocked(
                    nextActiveRequests,
                    nextActiveFilterRequests,
                    nextRequestInfos,
                    nextRequestStatuses,
                    desiredTileKeys,
                    nextTilePriorityRanks,
                    nextFilterDeliveryEpochs,
                    nextFilterAdmissibleEpochs,
                    nextFilterRegistrations);
            } else {
                replaceActiveRequestStateLocked(
                    requestId,
                    nextActiveRequests,
                    nextActiveFilterRequests,
                    nextRequestInfos,
                    nextRequestStatuses,
                    desiredTileKeys,
                    nextTilePriorityRanks,
                    nextFilterDeliveryEpochs,
                    nextFilterAdmissibleEpochs,
                    nextFilterRegistrations,
                    replacedRequests,
                    replacedFilterRequests);
            }
            requestChunksComplete_ = requestChunksComplete;
            // Refresh ordering so queued tiles follow the latest request priority.
            reprioritizeOutgoingLocked();
            statusEmissionEnabled_ = true;
        }

        abortReplacedRequests(std::move(replacedRequests), std::move(replacedFilterRequests));
        queueRequestContextMessage();
        submitBackendRequests(serviceRequests, filterServiceRequests);
        queueStatusMessage({});
    }

    /** Apply sparse per-tile renewals without replacing or aborting unrelated subscription work. */
    void renewFromClientRequest(nlohmann::json const& envelope)
    {
        auto renewals = envelope.find("renewals");
        if (renewals == envelope.end() || !renewals->is_array()) {
            queueStatusMessage("Missing or invalid 'renewals' array");
            return;
        }

        std::vector<detail::ParsedLayerTilesRequest> accepted;
        try {
            accepted.reserve(renewals->size());
            size_t acceptedTileCount = 0;
            for (auto const& renewalJson : *renewals) {
                auto effectiveJson = renewalJson;
                detail::inheritFilterFields(effectiveJson, envelope);
                auto parsed = detail::parseLayerTilesRequestJson(effectiveJson);
                if (!parsed.filterRequest) {
                    throw std::runtime_error(
                        "renewal entries require a filter definition");
                }
                auto const renewalTileCount = collectFilterTileIds(parsed).size();
                if (renewalTileCount > MAX_SPARSE_RENEWAL_TILES_PER_ENTRY) {
                    throw std::runtime_error(fmt::format(
                        "renewal entry has {} tiles; maximum is {}",
                        renewalTileCount,
                        MAX_SPARSE_RENEWAL_TILES_PER_ENTRY));
                }
                if (acceptedTileCount >
                    MAX_SPARSE_RENEWAL_TILES_PER_ENVELOPE - renewalTileCount)
                {
                    throw std::runtime_error(fmt::format(
                        "renewal envelope has more than {} tiles",
                        MAX_SPARSE_RENEWAL_TILES_PER_ENVELOPE));
                }
                acceptedTileCount += renewalTileCount;
                accepted.push_back(std::move(parsed));
            }
        }
        catch (std::exception const& e) {
            queueStatusMessage(fmt::format(
                "Invalid filter renewal JSON: {}",
                e.what()));
            return;
        }

        {
            std::lock_guard lock(mutex_);
            for (auto& renewal : accepted) {
                auto const subscriptionKey = detail::filterSubscriptionKey(
                    renewal.filterRequest->filterId_,
                    renewal.filterRequest->generation_);
                auto subscription = filterDeliveryEpochs_.find(subscriptionKey);
                auto const registration =
                    filterRegistrations_.find(subscriptionKey);
                std::vector<TileId> acceptedTileIds;
                std::map<TileId, uint64_t> acceptedEpochs;
                if (subscription == filterDeliveryEpochs_.end() ||
                    registration == filterRegistrations_.end() ||
                    renewal.mapId != registration->second.mapId ||
                    renewal.layerId != registration->second.layerId ||
                    renewal.sourceId != registration->second.sourceId)
                {
                    renewal.tileIds.clear();
                    renewal.deliveryEpochs.clear();
                    continue;
                }
                // Definition and roots are authoritative full-snapshot state.
                // A sparse operation may advance delivery identity, not mutate
                // the semantic filter or its authorization-scoped source.
                auto const requestedDefaultEpoch =
                    renewal.filterRequest->deliveryEpoch_;
                renewal.filterRequest = registration->second.definition;
                renewal.filterRequest->deliveryEpoch_ =
                    requestedDefaultEpoch;
                renewal.exactRoots.clear();
                for (auto const& tileId : collectFilterTileIds(renewal)) {
                    auto const canonicalKey = makeCanonicalRequestedTileKey(
                        renewal.mapId,
                        renewal.layerId,
                        tileId);
                    auto currentEpoch = subscription->second.find(canonicalKey);
                    if (currentEpoch == subscription->second.end()) {
                        continue;
                    }
                    auto const explicitEpoch = renewal.deliveryEpochs.find(tileId);
                    auto const requestedEpoch =
                        explicitEpoch != renewal.deliveryEpochs.end()
                        ? explicitEpoch->second
                        : renewal.filterRequest->deliveryEpoch_;
                    if (requestedEpoch <= currentEpoch->second) {
                        continue;
                    }

                    auto const oldFilterKey = detail::filterRequestKey(
                        renewal.filterRequest->filterId_,
                        renewal.filterRequest->generation_,
                        currentEpoch->second);
                    auto const newFilterKey = detail::filterRequestKey(
                        renewal.filterRequest->filterId_,
                        renewal.filterRequest->generation_,
                        requestedEpoch);
                    auto const oldRequestedKey = makeRequestedTileKey(
                        canonicalKey,
                        std::optional<std::string_view>(oldFilterKey));
                    auto const newRequestedKey = makeRequestedTileKey(
                        canonicalKey,
                        std::optional<std::string_view>(newFilterKey));
                    auto const rank = tilePriorityRankLocked(oldRequestedKey);
                    // Keep the previous epoch admissible while the renewal is
                    // merely in flight. It is retired only when the newer
                    // subset itself reaches onTileLayer().
                    desiredTileKeys_.insert(newRequestedKey);
                    tilePriorityRanks_[newRequestedKey] = rank;
                    filterAdmissibleEpochs_[subscriptionKey][canonicalKey].insert(
                        requestedEpoch);
                    currentEpoch->second = requestedEpoch;
                    acceptedTileIds.push_back(tileId);
                    acceptedEpochs.emplace(tileId, requestedEpoch);
                    if (auto const roots =
                            registration->second.rootsByTile.find(tileId);
                        roots != registration->second.rootsByTile.end())
                    {
                        renewal.exactRoots.insert(
                            renewal.exactRoots.end(),
                            roots->second.begin(),
                            roots->second.end());
                    }
                }
                renewal.tileIds = std::move(acceptedTileIds);
                renewal.deliveryEpochs = std::move(acceptedEpochs);
                renewal.priorityTileIds = prioritiesWithin(
                    renewal.tileIds,
                    renewal.priorityTileIds);
            }
            filterOutgoingByDesiredLocked();
            reprioritizeOutgoingLocked();
        }

        std::vector<FeatureLayerFilterTilesRequest::Ptr> serviceRequests;
        for (auto const& renewal : accepted) {
            if (renewal.tileIds.empty()) {
                continue;
            }
            auto request = makeRenewalBackendRequest(renewal);
            if (request) {
                serviceRequests.push_back(std::move(request));
            }
        }
        if (serviceRequests.empty()) {
            return;
        }
        {
            std::lock_guard lock(mutex_);
            activeFilterRequests_.insert(
                activeFilterRequests_.end(),
                serviceRequests.begin(),
                serviceRequests.end());
        }
        submitBackendRequests({}, serviceRequests);
    }

    /** Cancel current requests, clear queued frames, and emit a terminal status. */
    void cancel(std::string reason)
    {
        cancelled_ = true;
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
            collectAllPullWaitersLocked(PullFrameResult::Status::Closed, pullDispatches);
            outgoingCapacityChanged_.notify_all();
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
    using FilterDeliveryEpochState =
        std::map<std::string, std::map<MapTileKey, uint64_t>>;
    using FilterAdmissibleEpochState =
        std::map<std::string, std::map<MapTileKey, std::set<uint64_t>>>;

    struct FilterRegistration
    {
        std::string mapId;
        std::string layerId;
        std::optional<std::string> sourceId;
        FeatureLayerFilterRequest definition;
        std::map<TileId, std::vector<FeatureLayerFilterRoot>> rootsByTile;
    };
    using FilterRegistrationState =
        std::map<std::string, FilterRegistration>;

    /** Lightweight metadata emitted in status payloads for each logical request. */
    struct RequestInfo
    {
        std::string mapId;
        std::string layerId;
        NoDataSourceReason noDataSourceReason = NoDataSourceReason::None;
    };

    /** One queued websocket frame plus metadata used for bookkeeping. */
    struct OutgoingFrame
    {
        std::string bytes;
        TileLayerStream::MessageType type{TileLayerStream::MessageType::None};
        std::optional<std::pair<std::string, simfil::StringId>> stringPoolCommit;
        std::optional<MapTileKey> requestedTileKey;
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

    /** Validate append chunks against the active request and return their status-index base. */
    [[nodiscard]] std::optional<std::string> validateAppendUpdate(
        ClientRequestUpdateMode updateMode,
        uint64_t requestId,
        size_t& requestIndexBase)
    {
        if (updateMode != ClientRequestUpdateMode::Append) {
            requestIndexBase = 0;
            return std::nullopt;
        }

        std::lock_guard lock(mutex_);
        if (requestId_ != requestId) {
            return fmt::format(
                "Cannot append request chunk {} to active request {}.",
                requestId,
                requestId_);
        }
        requestIndexBase = requestInfos_.size();
        return std::nullopt;
    }

    /** Register one immutable filter definition while accumulating chunk-local exact roots. */
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
                {}});
        if (!inserted) {
            auto const sameDefinition =
                detail::filterRequestToJson(
                    registration->second.definition,
                    false) ==
                detail::filterRequestToJson(
                    *request.filterRequest,
                    false);
            if (registration->second.mapId != request.mapId ||
                registration->second.layerId != request.layerId ||
                registration->second.sourceId != request.sourceId ||
                !sameDefinition)
            {
                throw std::runtime_error(
                    "one filter subscription generation must use one map, layer, source, and definition");
            }
        }
        for (auto const& root : request.exactRoots) {
            registration->second.rootsByTile[root.tileId_].push_back(root);
        }
    }

    /** Preserve overlap epochs across complete coverage replacement and apply explicit resume overrides. */
    void reconcileFilterDeliveryEpochs(
        detail::ParsedLayerTilesRequest& request,
        FilterDeliveryEpochState& nextState,
        FilterAdmissibleEpochState& nextAdmissibleState)
    {
        if (!request.filterRequest) {
            return;
        }
        auto const subscriptionKey = detail::filterSubscriptionKey(
            request.filterRequest->filterId_,
            request.filterRequest->generation_);
        std::lock_guard lock(mutex_);
        auto const previousSubscription =
            filterDeliveryEpochs_.find(subscriptionKey);
        auto const previousAdmissibleSubscription =
            filterAdmissibleEpochs_.find(subscriptionKey);
        auto& nextSubscription = nextState[subscriptionKey];
        auto& nextAdmissibleSubscription =
            nextAdmissibleState[subscriptionKey];
        for (auto const& tileId : collectFilterTileIds(request)) {
            auto const tileKey = makeCanonicalRequestedTileKey(
                request.mapId,
                request.layerId,
                tileId);
            auto epoch = request.filterRequest->deliveryEpoch_;
            auto const explicitEpoch = request.deliveryEpochs.find(tileId);
            if (explicitEpoch != request.deliveryEpochs.end()) {
                epoch = explicitEpoch->second;
            }
            if (previousSubscription != filterDeliveryEpochs_.end()) {
                auto const previousEpoch =
                    previousSubscription->second.find(tileKey);
                if (previousEpoch != previousSubscription->second.end()) {
                    epoch = explicitEpoch != request.deliveryEpochs.end()
                        ? std::max(epoch, previousEpoch->second)
                        : previousEpoch->second;
                }
            }
            request.deliveryEpochs[tileId] = epoch;
            nextSubscription[tileKey] = epoch;
            auto& admissibleEpochs = nextAdmissibleSubscription[tileKey];
            if (previousAdmissibleSubscription !=
                filterAdmissibleEpochs_.end())
            {
                auto const previousAdmissibleEpochs =
                    previousAdmissibleSubscription->second.find(tileKey);
                if (previousAdmissibleEpochs !=
                    previousAdmissibleSubscription->second.end())
                {
                    admissibleEpochs.insert(
                        previousAdmissibleEpochs->second.begin(),
                        previousAdmissibleEpochs->second.end());
                }
            }
            admissibleEpochs.insert(epoch);
        }
    }

    /** Add one requested tile key to the next desired-set and preserve its first priority rank. */
    void addDesiredTileKey(
        MapTileKey requestedTileKey,
        std::set<MapTileKey>& desiredTileKeys,
        std::map<MapTileKey, int64_t>& tilePriorityRanks,
        int64_t& nextPriorityRank) const
    {
        desiredTileKeys.insert(requestedTileKey);
        if (tilePriorityRanks.find(requestedTileKey) == tilePriorityRanks.end()) {
            tilePriorityRanks.emplace(std::move(requestedTileKey), nextPriorityRank++);
        }
    }

    /** Compute all queue keys expected by one parsed request and their layer-local priorities. */
    void addDesiredTileKeys(
        const detail::ParsedLayerTilesRequest& request,
        FilterAdmissibleEpochState const& filterAdmissibleEpochs,
        std::set<MapTileKey>& desiredTileKeys,
        std::map<MapTileKey, int64_t>& tilePriorityRanks) const
    {
        int64_t nextPriorityRank = 0;
        if (request.filterRequest) {
            auto const subscriptionKey = detail::filterSubscriptionKey(
                request.filterRequest->filterId_,
                request.filterRequest->generation_);
            auto const subscription =
                filterAdmissibleEpochs.find(subscriptionKey);
            for (auto const& tileId : collectFilterTileIds(request)) {
                auto const requestedEpoch = request.deliveryEpochs.contains(tileId)
                    ? request.deliveryEpochs.at(tileId)
                    : request.filterRequest->deliveryEpoch_;
                auto const canonicalKey = MapTileKey(
                    REQUEST_TILE_LAYER_TYPE,
                    request.mapId,
                    request.layerId,
                    tileId);
                auto const tilePriorityRank = nextPriorityRank++;
                auto const addEpoch = [&](uint64_t epoch) {
                    auto const key = detail::filterRequestKey(
                        request.filterRequest->filterId_,
                        request.filterRequest->generation_,
                        epoch);
                    auto requestedKey = makeRequestedTileKey(
                        canonicalKey,
                        std::optional<std::string_view>(key));
                    desiredTileKeys.insert(requestedKey);
                    tilePriorityRanks.emplace(
                        std::move(requestedKey),
                        tilePriorityRank);
                };
                if (subscription != filterAdmissibleEpochs.end()) {
                    auto const admissible =
                        subscription->second.find(canonicalKey);
                    if (admissible != subscription->second.end()) {
                        for (auto const epoch : admissible->second) {
                            addEpoch(epoch);
                        }
                        continue;
                    }
                }
                addEpoch(requestedEpoch);
            }
            return;
        }

        auto expandedTileKeys = detail::expandLayerTilesRequestKeys(
            request,
            REQUEST_TILE_LAYER_TYPE);
        for (auto const& tileKey : expandedTileKeys) {
            addDesiredTileKey(
                makeRequestedTileKey(tileKey, std::nullopt),
                desiredTileKeys,
                tilePriorityRanks,
                nextPriorityRank);
        }
    }

    /** Return filter tile ids whose result frames are not already queued for this session. */
    [[nodiscard]] std::vector<TileId> filterTileIdsMissingFromQueue(
        const detail::ParsedLayerTilesRequest& request)
    {
        std::vector<TileId> tileIdsToFilter;
        std::lock_guard lock(mutex_);
        for (auto const& tileId : collectFilterTileIds(request)) {
            auto const epoch = request.deliveryEpochs.contains(tileId)
                ? request.deliveryEpochs.at(tileId)
                : request.filterRequest->deliveryEpoch_;
            auto const filterRequestKey = detail::filterRequestKey(
                request.filterRequest->filterId_,
                request.filterRequest->generation_,
                epoch);
            auto requestedTileKey = makeRequestedTileKey(
                MapTileKey(
                    REQUEST_TILE_LAYER_TYPE,
                    request.mapId,
                    request.layerId,
                    tileId),
                filterRequestKey);
            // Reconnect/update requests may repeat tiles whose frames are still buffered.
            const bool alreadyQueued =
                queuedTileFrameRefCount_.find(requestedTileKey) != queuedTileFrameRefCount_.end();
            const bool alreadyForwarded =
                forwardedTileKeys_.find(requestedTileKey) != forwardedTileKeys_.end();
            if (!alreadyQueued && !alreadyForwarded) {
                tileIdsToFilter.push_back(tileId);
            }
        }
        return tileIdsToFilter;
    }

    /** Return feature tile ids whose frames are not already queued. */
    [[nodiscard]] std::vector<TileId> tileIdsMissingFromQueue(
        const detail::ParsedLayerTilesRequest& request)
    {
        std::vector<TileId> tileIdsToFetch;

        std::lock_guard lock(mutex_);
        for (auto const& tileId : request.tileIds) {
            auto requestedTileKey = makeRequestedTileKey(
                MapTileKey(
                    REQUEST_TILE_LAYER_TYPE,
                    request.mapId,
                    request.layerId,
                    tileId),
                std::nullopt);
            const bool alreadyQueued =
                queuedTileFrameRefCount_.find(requestedTileKey) != queuedTileFrameRefCount_.end();
            const bool alreadyForwarded =
                forwardedTileKeys_.find(requestedTileKey) != forwardedTileKeys_.end();
            if (!alreadyQueued && !alreadyForwarded) {
                tileIdsToFetch.push_back(tileId);
            }
        }
        return tileIdsToFetch;
    }

    /** Attach websocket callbacks to one backend filter request. */
    void attachFilterRequestCallbacks(
        const FeatureLayerFilterTilesRequest::Ptr& request,
        uint64_t expectedRequestId,
        size_t statusIndex)
    {
        const auto weak = weak_from_this();
        const std::weak_ptr<FeatureLayerFilterTilesRequest> weakRequest = request;
        request->onFilterResult([weak](TileSubsetLayer::Ptr layer) {
            if (auto self = weak.lock()) {
                self->onTileLayer(std::move(layer));
            }
        });
        request->onStatus([weak, expectedRequestId](nlohmann::json const& status) {
            if (auto self = weak.lock()) {
                if (self->isCurrentRequest(expectedRequestId)) {
                    self->sendControlMessage(
                        TileLayerStream::MessageType::Status,
                        status.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore));
                }
            }
        });
        request->onDone_ = [weak, statusIndex, expectedRequestId, weakRequest](RequestStatus status) {
            if (auto self = weak.lock()) {
                if (auto request = weakRequest.lock()) {
                    self->onFilterRequestDone(statusIndex, expectedRequestId, request, status);
                }
            }
        };
    }

    /** Create a backend filter request unless all requested filter results are already queued. */
    [[nodiscard]] FeatureLayerFilterTilesRequest::Ptr makeFilterBackendRequestIfNeeded(
        const detail::ParsedLayerTilesRequest& parsedRequest,
        uint64_t expectedRequestId,
        size_t statusIndex)
    {
        if (!parsedRequest.filterRequest) {
            return {};
        }

        auto filterRequest = *parsedRequest.filterRequest;
        auto tileIdsToFilter =
            filterTileIdsMissingFromQueue(parsedRequest);
        if (tileIdsToFilter.empty()) {
            return {};
        }
        auto priorityTileIds = prioritiesWithin(
            tileIdsToFilter,
            parsedRequest.priorityTileIds);
        std::set<TileId> const missingTileIds(
            tileIdsToFilter.begin(),
            tileIdsToFilter.end());

        auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
            parsedRequest.mapId,
            parsedRequest.layerId,
            std::move(tileIdsToFilter),
            std::move(filterRequest),
            priorityTileIds);
        request->sourceId_ = parsedRequest.sourceId;
        for (auto const& tileId : request->tileIds_) {
            auto const epoch = parsedRequest.deliveryEpochs.find(tileId);
            request->deliveryEpochs_[tileId] =
                epoch != parsedRequest.deliveryEpochs.end()
                ? epoch->second
                : request->filter_.deliveryEpoch_;
        }
        request->exactRoots_ = parsedRequest.exactRoots;
        std::erase_if(
            request->exactRoots_,
            [&](FeatureLayerFilterRoot const& root) {
                return !missingTileIds.contains(root.tileId_);
            });
        attachFilterRequestCallbacks(request, expectedRequestId, statusIndex);
        return request;
    }

    /** Build one sparse renewal request; its completion never changes full-request status. */
    [[nodiscard]] FeatureLayerFilterTilesRequest::Ptr makeRenewalBackendRequest(
        detail::ParsedLayerTilesRequest const& renewal)
    {
        if (!renewal.filterRequest || renewal.tileIds.empty()) {
            return {};
        }
        auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
            renewal.mapId,
            renewal.layerId,
            renewal.tileIds,
            *renewal.filterRequest,
            renewal.priorityTileIds);
        request->sourceId_ = renewal.sourceId;
        request->exactRoots_ = renewal.exactRoots;
        request->deliveryEpochs_ = renewal.deliveryEpochs;

        auto const weak = weak_from_this();
        auto const weakRequest =
            std::weak_ptr<FeatureLayerFilterTilesRequest>(request);
        request->onFilterResult([weak](TileSubsetLayer::Ptr layer) {
            if (auto self = weak.lock()) {
                self->onTileLayer(std::move(layer));
            }
        });
        request->onDone_ = [weak, weakRequest](RequestStatus) {
            if (auto self = weak.lock()) {
                if (auto completed = weakRequest.lock()) {
                    self->onRenewalRequestDone(completed);
                }
            }
        };
        return request;
    }

    /** Attach websocket callbacks to one backend tile request. */
    void attachLayerRequestCallbacks(
        const LayerTilesRequest::Ptr& request,
        uint64_t expectedRequestId,
        size_t statusIndex)
    {
        const auto weak = weak_from_this();
        const std::weak_ptr<LayerTilesRequest> weakRequest = request;
        request->onFeatureLayer([weak](TileFeatureLayer::Ptr layer) {
            if (auto self = weak.lock()) {
                self->onTileLayer(std::move(layer));
            }
        });
        request->onSourceDataLayer([weak](TileSourceDataLayer::Ptr layer) {
            if (auto self = weak.lock()) {
                self->onTileLayer(std::move(layer));
            }
        });
        if (EMIT_LOAD_STATE_FRAMES) {
            request->onLayerLoadStateChanged([weak](MapTileKey const& key, TileLayer::LoadState state) {
                if (auto self = weak.lock()) {
                    self->onLoadStateChanged(key, state);
                }
            });
        }
        request->onDone_ = [weak, statusIndex, expectedRequestId, weakRequest](RequestStatus status) {
            if (auto self = weak.lock()) {
                if (auto request = weakRequest.lock()) {
                    self->onRequestDone(statusIndex, expectedRequestId, request, status);
                }
            }
        };
    }

    /** Create a backend feature-tile request unless all requested tile frames are already queued. */
    [[nodiscard]] LayerTilesRequest::Ptr makeLayerBackendRequestIfNeeded(
        const detail::ParsedLayerTilesRequest& parsedRequest,
        uint64_t expectedRequestId,
        size_t statusIndex)
    {
        auto tileIdsToFetch = tileIdsMissingFromQueue(parsedRequest);
        if (tileIdsToFetch.empty()) {
            return {};
        }
        auto priorityTileIds = prioritiesWithin(
            tileIdsToFetch,
            parsedRequest.priorityTileIds);
        std::set<TileId> const missingTileIds(
            tileIdsToFetch.begin(),
            tileIdsToFetch.end());
        auto request = std::make_shared<LayerTilesRequest>(
            parsedRequest.mapId,
            parsedRequest.layerId,
            std::move(tileIdsToFetch),
            priorityTileIds);
        request->sourceId_ = parsedRequest.sourceId;
        request->featureIdsByTile_ = parsedRequest.featureIdsByTile;
        std::erase_if(
            request->featureIdsByTile_,
            [&](auto const& item) {
                return !missingTileIds.contains(item.first);
            });

        attachLayerRequestCallbacks(request, expectedRequestId, statusIndex);
        return request;
    }

    /** Append one chunk's request state to the currently active logical request. */
    void appendActiveRequestStateLocked(
        std::vector<LayerTilesRequest::Ptr>& nextActiveRequests,
        std::vector<FeatureLayerFilterTilesRequest::Ptr>& nextActiveFilterRequests,
        std::vector<RequestInfo>& nextRequestInfos,
        std::vector<RequestStatus>& nextRequestStatuses,
        const std::set<MapTileKey>& desiredTileKeys,
        const std::map<MapTileKey, int64_t>& tilePriorityRanks,
        FilterDeliveryEpochState const& filterDeliveryEpochs,
        FilterAdmissibleEpochState const& filterAdmissibleEpochs,
        FilterRegistrationState const& filterRegistrations)
    {
        for (auto& request : nextActiveRequests) {
            activeRequests_.push_back(std::move(request));
        }
        for (auto& request : nextActiveFilterRequests) {
            activeFilterRequests_.push_back(std::move(request));
        }
        for (auto& info : nextRequestInfos) {
            requestInfos_.push_back(std::move(info));
        }
        for (auto status : nextRequestStatuses) {
            requestStatuses_.push_back(status);
        }
        desiredTileKeys_.insert(desiredTileKeys.begin(), desiredTileKeys.end());
        filterDeliveryEpochs_ = filterDeliveryEpochs;
        filterAdmissibleEpochs_ = filterAdmissibleEpochs;
        filterRegistrations_ = filterRegistrations;
        for (auto const& [tileKey, priorityRank] : tilePriorityRanks) {
            tilePriorityRanks_.emplace(tileKey, priorityRank);
        }
    }

    /** Replace the active logical request and return previously active backend work. */
    void replaceActiveRequestStateLocked(
        uint64_t requestId,
        std::vector<LayerTilesRequest::Ptr>& nextActiveRequests,
        std::vector<FeatureLayerFilterTilesRequest::Ptr>& nextActiveFilterRequests,
        std::vector<RequestInfo>& nextRequestInfos,
        std::vector<RequestStatus>& nextRequestStatuses,
        std::set<MapTileKey>& desiredTileKeys,
        std::map<MapTileKey, int64_t>& tilePriorityRanks,
        FilterDeliveryEpochState& filterDeliveryEpochs,
        FilterAdmissibleEpochState& filterAdmissibleEpochs,
        FilterRegistrationState& filterRegistrations,
        std::vector<LayerTilesRequest::Ptr>& replacedRequests,
        std::vector<FeatureLayerFilterTilesRequest::Ptr>& replacedFilterRequests)
    {
        replacedRequests = std::move(activeRequests_);
        replacedFilterRequests = std::move(activeFilterRequests_);
        activeRequests_ = std::move(nextActiveRequests);
        activeFilterRequests_ = std::move(nextActiveFilterRequests);
        requestId_ = requestId;
        requestInfos_ = std::move(nextRequestInfos);
        requestStatuses_ = std::move(nextRequestStatuses);
        desiredTileKeys_ = std::move(desiredTileKeys);
        filterDeliveryEpochs_ = std::move(filterDeliveryEpochs);
        filterAdmissibleEpochs_ = std::move(filterAdmissibleEpochs);
        filterRegistrations_ = std::move(filterRegistrations);
        tilePriorityRanks_ = std::move(tilePriorityRanks);
        for (auto it = forwardedTileKeys_.begin();
             it != forwardedTileKeys_.end();)
        {
            if (desiredTileKeys_.find(*it) == desiredTileKeys_.end()) {
                it = forwardedTileKeys_.erase(it);
            }
            else {
                ++it;
            }
        }
        // When request scope shrinks, remove stale tile data already queued for send.
        filterOutgoingByDesiredLocked();
    }

    /** Abort requests replaced by a newer logical request and update replacement metrics. */
    void abortReplacedRequests(
        std::vector<LayerTilesRequest::Ptr> replacedRequests,
        std::vector<FeatureLayerFilterTilesRequest::Ptr> replacedFilterRequests)
    {
        if (!replacedRequests.empty()) {
            gTilesWsMetrics.replacedRequests.fetch_add(
                static_cast<int64_t>(replacedRequests.size()),
                std::memory_order_relaxed);
            abortRequests(std::move(replacedRequests));
        }
        if (!replacedFilterRequests.empty()) {
            gTilesWsMetrics.replacedRequests.fetch_add(
                static_cast<int64_t>(replacedFilterRequests.size()),
                std::memory_order_relaxed);
            abortFilterRequests(std::move(replacedFilterRequests));
        }
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
        outgoingCapacityChanged_.notify_all();
        if (frame.stringPoolCommit) {
            committedStringPoolOffsets_[frame.stringPoolCommit->first] = frame.stringPoolCommit->second;
        }
        if (frame.requestedTileKey && isTileDataMessage(frame.type)) {
            forwardedTileKeys_.insert(*frame.requestedTileKey);
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

    /** Match one backend-produced tile key against the currently desired request set. */
    [[nodiscard]] std::optional<MapTileKey> matchDesiredTileKeyLocked(
        MapTileKey key,
        std::optional<std::string_view> filterKey = std::nullopt) const
    {
        auto requestedTileKey = makeRequestedTileKey(std::move(key), filterKey);
        if (desiredTileKeys_.find(requestedTileKey) != desiredTileKeys_.end()) {
            return requestedTileKey;
        }

        return std::nullopt;
    }

    /** Retire older admissible epochs only after the newest requested subset has arrived. */
    void retireOlderFilterEpochsLocked(
        TileSubsetLayer const& subset,
        MapTileKey const& newestRequestedKey)
    {
        auto const subscriptionKey = detail::filterSubscriptionKey(
            subset.filterId(),
            subset.generation());
        auto subscription = filterDeliveryEpochs_.find(subscriptionKey);
        auto const canonicalKey = makeCanonicalRequestedTileKey(subset.id());
        if (subscription == filterDeliveryEpochs_.end()) {
            return;
        }
        auto const requestedEpoch = subscription->second.find(canonicalKey);
        if (requestedEpoch == subscription->second.end() ||
            requestedEpoch->second != subset.deliveryEpoch()) {
            return;
        }
        auto admissibleSubscription =
            filterAdmissibleEpochs_.find(subscriptionKey);
        if (admissibleSubscription != filterAdmissibleEpochs_.end()) {
            auto admissibleEpochs =
                admissibleSubscription->second.find(canonicalKey);
            if (admissibleEpochs != admissibleSubscription->second.end()) {
                admissibleEpochs->second = {subset.deliveryEpoch()};
            }
        }

        auto const prefix = canonicalKey.layerId_ +
            "#filter:" + subscriptionKey + ":";
        for (auto it = desiredTileKeys_.begin();
             it != desiredTileKeys_.end();)
        {
            if (*it != newestRequestedKey &&
                it->mapId_ == canonicalKey.mapId_ &&
                it->tileId_ == canonicalKey.tileId_ &&
                it->layerId_.starts_with(prefix))
            {
                tilePriorityRanks_.erase(*it);
                it = desiredTileKeys_.erase(it);
            }
            else {
                ++it;
            }
        }
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

    /** Return true when another tile layer may be serialized without unbounded queue growth. */
    [[nodiscard]] bool hasOutgoingCapacityLocked() const
    {
        // Always allow an empty queue to accept the next layer, even if that
        // single layer is larger than the nominal byte cap.
        return outgoing_.empty()
            || (outgoing_.size() < MAX_QUEUED_OUTGOING_FRAMES
                && queuedOutgoingBytes_ < MAX_QUEUED_OUTGOING_BYTES);
    }

    /** Block backend producer callbacks until `/interactive/payload` drains queued frames. */
    [[nodiscard]] bool waitForOutgoingCapacityLocked(std::unique_lock<std::mutex>& lock)
    {
        outgoingCapacityChanged_.wait(lock, [this] {
            return cancelled_ || hasOutgoingCapacityLocked();
        });
        return !cancelled_;
    }

    /** Drop queued tile data frames that no longer belong to the latest request set. */
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
            outgoingCapacityChanged_.notify_all();
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
        outgoingCapacityChanged_.notify_all();
    }

    /** Internal cancel path used by destructor/connection tear-down (no status emission). */
    void cancelNoStatus()
    {
        if (cancelled_.exchange(true))
            return;
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
            collectAllPullWaitersLocked(PullFrameResult::Status::Closed, pullDispatches);
            outgoingCapacityChanged_.notify_all();
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

    /** Convert one backend tile layer into outgoing websocket frames. */
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
                std::unique_lock lock(mutex_);
                if (cancelled_)
                    return;
                auto filterKey = filterRequestKey(layer);
                auto requestedTileKey = matchDesiredTileKeyLocked(
                    layer->id(),
                    filterKey ? std::optional<std::string_view>(*filterKey) : std::nullopt);
                // Late-arriving tile for an outdated request: drop before serialization work.
                if (!requestedTileKey.has_value()) {
                    return;
                }
                if (auto subset =
                        std::dynamic_pointer_cast<TileSubsetLayer>(layer))
                {
                    retireOlderFilterEpochsLocked(
                        *subset,
                        *requestedTileKey);
                }
                if (queuedTileFrameRefCount_.find(*requestedTileKey) !=
                        queuedTileFrameRefCount_.end() ||
                    forwardedTileKeys_.find(*requestedTileKey) !=
                        forwardedTileKeys_.end())
                {
                    return;
                }
                if (!waitForOutgoingCapacityLocked(lock)) {
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
                const auto stringPoolId = layer->stringPoolId();
                const auto it = writerOffsets_.find(stringPoolId);
                if (it != writerOffsets_.end()) {
                    const auto newOffset = it->second;
                    for (auto const& m : batch) {
                        if (m.type == TileLayerStream::MessageType::StringPool) {
                            stringPoolCommit = std::make_pair(stringPoolId, newOffset);
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
                    if (isTileDataMessage(m.type)) {
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

    /** Update per-request completion state and emit status when it changes. */
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

    /** Update per-filter completion state and emit status when it changes. */
    void onFilterRequestDone(
        size_t requestIndex,
        uint64_t expectedRequestId,
        const FeatureLayerFilterTilesRequest::Ptr& completedRequest,
        RequestStatus status)
    {
        if (cancelled_)
            return;

        bool shouldEmit = false;
        {
            std::lock_guard lock(mutex_);
            if (cancelled_)
                return;
            activeFilterRequests_.erase(
                std::remove_if(
                    activeFilterRequests_.begin(),
                    activeFilterRequests_.end(),
                    [&](const FeatureLayerFilterTilesRequest::Ptr& req) {
                        return !req || req == completedRequest || req->isDone();
                    }),
                activeFilterRequests_.end());
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

    /** Forget terminal sparse work without mutating the full subscription's status array. */
    void onRenewalRequestDone(
        FeatureLayerFilterTilesRequest::Ptr const& completedRequest)
    {
        std::lock_guard lock(mutex_);
        activeFilterRequests_.erase(
            std::remove_if(
                activeFilterRequests_.begin(),
                activeFilterRequests_.end(),
                [&](FeatureLayerFilterTilesRequest::Ptr const& request) {
                    return !request ||
                        request == completedRequest ||
                        request->isDone();
                }),
            activeFilterRequests_.end());
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

    /** True while a backend callback still belongs to the active logical request. */
    [[nodiscard]] bool isCurrentRequest(uint64_t expectedRequestId) const
    {
        std::lock_guard lock(mutex_);
        return !cancelled_ && requestId_ == expectedRequestId;
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
            if (desiredTileKeys_.find(requestedTileKey) == desiredTileKeys_.end()) {
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
            {"requestId", requestId_},
            {"allDone", allDone},
            {"requests", std::move(requestsJson)},
            {"message", std::move(message)},
        }).dump();
    }

    /** Build the JSON payload for `mapget.tiles.load-state`. */
    [[nodiscard]] std::string buildLoadStatePayload(MapTileKey const& key, TileLayer::LoadState state) const
    {
        return nlohmann::json::object({
            {"type", "mapget.tiles.load-state"},
            {"requestId", requestId_},
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
        return nlohmann::json::object({
            {"type", "mapget.tiles.request-context"},
            {"requestId", requestId_},
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
    std::condition_variable outgoingCapacityChanged_;
    uint64_t nextPullWaiterId_ = 1;
    std::deque<uint64_t> pendingPullWaiterOrder_;
    std::unordered_map<uint64_t, PullWaiter> pendingPullWaiters_;
    std::deque<OutgoingFrame> outgoing_;
    size_t queuedOutgoingBytes_ = 0;
    std::vector<RequestInfo> requestInfos_;
    std::vector<RequestStatus> requestStatuses_;
    std::vector<LayerTilesRequest::Ptr> activeRequests_;
    std::vector<FeatureLayerFilterTilesRequest::Ptr> activeFilterRequests_;
    FilterDeliveryEpochState filterDeliveryEpochs_;
    FilterAdmissibleEpochState filterAdmissibleEpochs_;
    FilterRegistrationState filterRegistrations_;
    std::set<MapTileKey> desiredTileKeys_;
    std::map<MapTileKey, int64_t> tilePriorityRanks_;
    std::map<MapTileKey, int64_t> queuedTileFrameRefCount_;
    std::set<MapTileKey> forwardedTileKeys_;
    bool statusEmissionEnabled_ = false;
    uint64_t pendingChunkedRequestId_ = 0;
    uint64_t pendingChunkedNextIndex_ = 0;
    bool requestChunksComplete_ = true;

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
    const nlohmann::json& requestJson,
    uint64_t requestId)
{
    if (session) {
        session->updateFromClientRequestMessage(requestJson, requestId);
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
        {"pending-controller-allocated-bytes", nonNegative(gTilesWsMetrics.pendingAllocatedBytes)},
        {"peak-pending-controller-allocated-bytes", nonNegative(gTilesWsMetrics.peakPendingAllocatedBytes)},
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
