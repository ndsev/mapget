#pragma once

#include "http-service.h"
#include "mapget/location/location.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mapget
{

namespace detail
{

/** Snapshot REST stream pending-buffer ownership and high-water marks. */
[[nodiscard]] nlohmann::json tilesHttpMetricsSnapshot();

[[nodiscard]] inline AuthHeaders authHeadersFromRequest(const drogon::HttpRequestPtr& req)
{
    AuthHeaders headers;
    for (auto const& [k, v] : req->headers()) {
        headers.emplace(k, v);
    }
    return headers;
}

}  // namespace detail

struct HttpService::Impl
{
    HttpService& self_;
    HttpServiceConfig config_;
    /** Lookup backend used by GET /location when location search is enabled. */
    std::unique_ptr<SqliteLocationLookup> locationLookup_;

    /** Interruptible wait state for the allocator-maintenance worker. */
    std::mutex memoryTrimMutex_;
    std::condition_variable memoryTrimCv_;
    bool stopMemoryTrim_ = false;
    std::thread memoryTrimThread_;

    /** Lock-free diagnostics published after each periodic trim attempt. */
    std::atomic<uint64_t> memoryTrimAttempts_{0};
    std::atomic<uint64_t> memoryTrimSuccesses_{0};
    std::atomic<uint64_t> memoryTrimLastDurationMicros_{0};
    std::atomic<uint64_t> memoryTrimLastFreeArenaBefore_{0};
    std::atomic<uint64_t> memoryTrimLastFreeArenaAfter_{0};

    /** Serialized request state for explicit, expensive cache reports. */
    std::mutex statusCacheReportMutex_;
    std::condition_variable statusCacheReportCv_;
    bool stopStatusCacheReport_ = false;
    std::thread statusCacheReportThread_;
    std::vector<std::function<void(const drogon::HttpResponsePtr&)>> statusCacheReportCallbacks_;

    /** Shared bounded-thread executor for latest-wins interactive reconciliation. */
    std::mutex interactiveControlMutex_;
    std::condition_variable interactiveControlCv_;
    bool stopInteractiveControl_ = false;
    std::deque<std::function<void()>> interactiveControlTasks_;
    std::vector<std::thread> interactiveControlThreads_;

    explicit Impl(HttpService& self, const HttpServiceConfig& config);
    ~Impl();

    /** Wait for each configured period and trim outside Drogon's event loop. */
    void runMemoryTrimLoop();

    /** Return periodic allocator-maintenance state for `/status-data`. */
    [[nodiscard]] nlohmann::json memoryTrimStatistics() const;

    struct TilesStreamState;

    void handleTilesRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    void handleFilterRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    /** Handle GET /attachment without blocking the HTTP event loop. */
    void handleAttachmentRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    void handleTilesLikeRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        bool filterEndpoint) const;

    void handleSourcesRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    /** Serve the self-contained operational status dashboard. */
    void handleStatusRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    /** Return the lightweight live metrics consumed by the status dashboard. */
    void handleStatusDataRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    /** Generate one detailed cache report outside Drogon's event loop. */
    void handleStatusCacheReportRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /** Coalesce report callers and serialize expensive cache traversal. */
    void runStatusCacheReportLoop();

    /** Queue one control task unless HTTP service shutdown has begun. */
    [[nodiscard]] bool enqueueInteractiveControlTask(std::function<void()> task);

    /** Execute interactive reconciliation without occupying Drogon's I/O loop. */
    void runInteractiveControlLoop();

    void handleLocateRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    /** Handle GET /location by querying the configured location lookup backend. */
    void handleLocationRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    static drogon::HttpResponsePtr openConfigFile(std::ifstream& configFile);

    void handleGetConfigRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    void handlePostConfigRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    void handlePutMapPresetsConfigRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    /** Handle the guarded, map-scoped tile-cache reset endpoint. */
    void handleCacheResetRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;
};

}  // namespace mapget
