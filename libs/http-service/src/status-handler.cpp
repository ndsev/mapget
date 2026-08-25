#include "http-service-impl.h"

#include "status-page.h"
#include "tiles-ws-controller.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>

#include <chrono>
#include <exception>
#include <string>
#include <utility>

namespace mapget
{
namespace
{

/** Return the current wall-clock timestamp in milliseconds. */
[[nodiscard]] int64_t timestampMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/** Build a non-cacheable JSON response for live operational data. */
[[nodiscard]] drogon::HttpResponsePtr jsonResponse(
    std::string body,
    drogon::HttpStatusCode status = drogon::k200OK)
{
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(status);
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->addHeader("Cache-Control", "no-store");
    response->setBody(std::move(body));
    return response;
}

/** Completed cache-report response shared by callers coalesced onto one run. */
struct CacheReportResult
{
    drogon::HttpStatusCode status = drogon::k200OK;
    std::string body;
};

/** Traverse the cache once and package the resulting point-in-time report. */
[[nodiscard]] CacheReportResult generateCacheReport(Service& service)
{
    auto const started = std::chrono::steady_clock::now();
    try {
        auto serviceStats = service.getStatistics(true, true);
        auto const durationMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
                                    .count();

        auto payload = nlohmann::json{
            {"generatedAtMs", timestampMs()},
            {"durationMs", durationMs},
            {"cache", service.cache()->getStatistics()},
            {"featureTree", nullptr},
            {"tileSizeDistribution", nullptr},
        };
        if (auto found = serviceStats.find("cached-feature-tree-bytes");
            found != serviceStats.end())
        {
            payload["featureTree"] = std::move(*found);
        }
        if (auto found = serviceStats.find("cached-feature-tile-size-distribution");
            found != serviceStats.end())
        {
            payload["tileSizeDistribution"] = std::move(*found);
        }
        return {.body = payload.dump()};
    }
    catch (std::exception const& error) {
        return {
            .status = drogon::k500InternalServerError,
            .body = nlohmann::json{{"error", error.what()}}.dump(),
        };
    }
    catch (...) {
        return {
            .status = drogon::k500InternalServerError,
            .body = nlohmann::json{{"error", "Unknown cache report failure."}}.dump(),
        };
    }
}

}  // namespace

void HttpService::Impl::handleStatusDataRequest(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
{
    auto serviceMemory = self_.getMemoryStatistics();
    auto cache = self_.cache()->getStatistics();
    auto websocket = detail::tilesWebSocketMetricsSnapshot();
    auto httpStreams = detail::tilesHttpMetricsSnapshot();

    uint64_t cacheBytes = 0;
    if (auto memory = cache.find("memory"); memory != cache.end() && memory->is_object()) {
        cacheBytes += memory->value("allocated-bytes", uint64_t{0});
        if (auto tileBlobs = memory->find("tile-blobs");
            tileBlobs != memory->end() && tileBlobs->is_object())
        {
            cacheBytes += tileBlobs->value("allocated-bytes", uint64_t{0});
        }
        cacheBytes += memory->value("sqlite-owned-bytes", uint64_t{0});
    }
    auto const transportBytes =
        websocket.value("pending-controller-allocated-bytes", uint64_t{0}) +
        httpStreams.value("pending-capacity-bytes", uint64_t{0});
    auto const knownBytes =
        serviceMemory.value("known-current-bytes", uint64_t{0}) +
        cacheBytes + transportBytes;
    serviceMemory["cache"] = cache.value("memory", nlohmann::json::object());
    serviceMemory["cache-current-bytes"] = cacheBytes;
    serviceMemory["transport"] = {
        {"interactive", websocket},
        {"rest-streams", httpStreams},
        {"current-bytes", transportBytes},
    };
    serviceMemory["allocator-trim"] = memoryTrimStatistics();
    serviceMemory["known-current-bytes"] = knownBytes;

    // Ownership estimates describe retained allocation capacity, whereas RSS
    // describes resident pages. Keep their diagnostic residuals explicit
    // instead of presenting one misleading "unattributed RSS" total.
    auto reconciliation = nlohmann::json{
        {"measurement", "diagnostic-residuals"},
        {"known-ownership-bytes", knownBytes},
    };
    uint64_t allocatorLiveBytes = 0;
    if (auto allocator = serviceMemory.find("allocator");
        allocator != serviceMemory.end() && allocator->is_object())
    {
        allocatorLiveBytes =
            allocator->value("in-use-arena-bytes", uint64_t{0}) +
            allocator->value("mmap-bytes", uint64_t{0});
        reconciliation["allocator-live-bytes"] = allocatorLiveBytes;
        reconciliation["allocator-live-outside-known-ownership-bytes"] =
            allocatorLiveBytes > knownBytes
                ? allocatorLiveBytes - knownBytes
                : uint64_t{0};
    }
    if (auto process = serviceMemory.find("process");
        process != serviceMemory.end() && process->is_object())
    {
        auto const anonymous =
            process->value("resident-anonymous-bytes", uint64_t{0});
        reconciliation["anonymous-resident-outside-allocator-live-bytes"] =
            anonymous > allocatorLiveBytes
                ? anonymous - allocatorLiveBytes
                : uint64_t{0};
        reconciliation["file-and-shared-resident-bytes"] =
            process->value("resident-file-bytes", uint64_t{0}) +
            process->value("resident-shared-bytes", uint64_t{0});
    }
    serviceMemory["reconciliation"] = std::move(reconciliation);

    const auto payload = nlohmann::json::object({
        {"timestampMs", timestampMs()},
        {"service", self_.getStatistics(false, false)},
        {"cache", std::move(cache)},
        {"tilesWebsocket", std::move(websocket)},
        {"tilesHttp", std::move(httpStreams)},
        {"memory", std::move(serviceMemory)},
    });
    callback(jsonResponse(payload.dump()));
}

void HttpService::Impl::handleStatusCacheReportRequest(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    bool stopping = false;
    {
        std::lock_guard lock(statusCacheReportMutex_);
        if (stopStatusCacheReport_) {
            stopping = true;
        } else {
            if (!statusCacheReportThread_.joinable()) {
                statusCacheReportThread_ = std::thread([this] { runStatusCacheReportLoop(); });
            }
            statusCacheReportCallbacks_.push_back(std::move(callback));
        }
    }
    if (stopping) {
        callback(jsonResponse(
            nlohmann::json{{"error", "Cache report worker is stopping."}}.dump(),
            drogon::k503ServiceUnavailable));
        return;
    }
    statusCacheReportCv_.notify_one();
}

void HttpService::Impl::runStatusCacheReportLoop()
{
    std::unique_lock lock(statusCacheReportMutex_);
    while (true) {
        statusCacheReportCv_.wait(lock, [this] {
            return stopStatusCacheReport_ || !statusCacheReportCallbacks_.empty();
        });
        if (stopStatusCacheReport_) {
            return;
        }

        // Keep the callback list in place while traversing so callers arriving
        // during this run receive the same point-in-time result.
        lock.unlock();
        auto result = generateCacheReport(self_);
        lock.lock();

        auto callbacks = std::move(statusCacheReportCallbacks_);
        statusCacheReportCallbacks_.clear();
        if (stopStatusCacheReport_) {
            return;
        }

        // HTTP response callbacks return to Drogon's loop after the blocking
        // traversal, keeping cache analysis away from request processing.
        lock.unlock();
        drogon::app().getLoop()->queueInLoop(
            [callbacks = std::move(callbacks), result = std::move(result)]() mutable {
                for (auto& callback : callbacks) {
                    callback(jsonResponse(result.body, result.status));
                }
            });
        lock.lock();
    }
}

void HttpService::Impl::handleStatusRequest(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
{
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k200OK);
    response->setContentTypeCode(drogon::CT_TEXT_HTML);
    response->addHeader("Cache-Control", "no-store");
    response->setBody(std::string(detail::statusPageHtml()));
    callback(response);
}

}  // namespace mapget
