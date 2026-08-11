#include "http-service-impl.h"

#include "mapget/log.h"

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

namespace mapget
{

HttpService::Impl::Impl(HttpService& self, const HttpServiceConfig& config) : self_(self), config_(config)
{
    if (config_.locationLookupEnabled) {
        auto locationDbPath = config_.locationDatabasePath.value_or(defaultLocationDatabasePath());
        locationLookup_ = std::make_unique<SqliteLocationLookup>(locationDbPath);
        if (!locationLookup_->available()) {
            log().info("Location database unavailable at {}", locationDbPath.string());
        }
    }

#if defined(__linux__) && defined(__GLIBC__)
    if (config_.memoryTrimPeriod > std::chrono::seconds::zero()) {
        memoryTrimThread_ = std::thread([this] { runMemoryTrimLoop(); });
    }
#endif
}

HttpService::Impl::~Impl()
{
    {
        std::lock_guard lock(statusCacheReportMutex_);
        stopStatusCacheReport_ = true;
    }
    statusCacheReportCv_.notify_all();

    {
        std::lock_guard lock(memoryTrimMutex_);
        stopMemoryTrim_ = true;
    }
    memoryTrimCv_.notify_all();
    if (memoryTrimThread_.joinable()) {
        memoryTrimThread_.join();
    }
    if (statusCacheReportThread_.joinable()) {
        statusCacheReportThread_.join();
    }
}

void HttpService::Impl::runMemoryTrimLoop()
{
#if defined(__linux__) && defined(__GLIBC__)
    std::unique_lock lock(memoryTrimMutex_);
    while (!memoryTrimCv_.wait_for(
        lock,
        config_.memoryTrimPeriod,
        [this] { return stopMemoryTrim_; }))
    {
        // malloc_trim may walk every arena. Keep it off the Drogon event loop and
        // outside our wait mutex so destruction only waits for the active trim.
        lock.unlock();
        auto const before = mallinfo2();
        auto const started = std::chrono::steady_clock::now();
        memoryTrimAttempts_.fetch_add(1, std::memory_order_relaxed);
        auto const released = malloc_trim(0) != 0;
        auto const elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started);
        auto const after = mallinfo2();
        memoryTrimLastFreeArenaBefore_.store(before.fordblks, std::memory_order_relaxed);
        memoryTrimLastFreeArenaAfter_.store(after.fordblks, std::memory_order_relaxed);
        memoryTrimLastDurationMicros_.store(
            static_cast<uint64_t>(elapsed.count()),
            std::memory_order_relaxed);
        if (released) {
            memoryTrimSuccesses_.fetch_add(1, std::memory_order_relaxed);
        }
        lock.lock();
    }
#endif
}

nlohmann::json HttpService::Impl::memoryTrimStatistics() const
{
    auto result = nlohmann::json{
        {"period-seconds", config_.memoryTrimPeriod.count()},
        {"attempts", memoryTrimAttempts_.load(std::memory_order_relaxed)},
        {"successful-trims", memoryTrimSuccesses_.load(std::memory_order_relaxed)},
        {"last-duration-microseconds", memoryTrimLastDurationMicros_.load(std::memory_order_relaxed)},
        {"last-free-arena-before-bytes", memoryTrimLastFreeArenaBefore_.load(std::memory_order_relaxed)},
        {"last-free-arena-after-bytes", memoryTrimLastFreeArenaAfter_.load(std::memory_order_relaxed)},
    };
#if defined(__linux__) && defined(__GLIBC__)
    result["supported"] = true;
#else
    result["supported"] = false;
#endif
    result["enabled"] = result["supported"].get<bool>() &&
        config_.memoryTrimPeriod > std::chrono::seconds::zero();
    return result;
}

}  // namespace mapget
