#pragma once

#include "mapget/detail/http-server.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/stream.h"
#include "mapget/service/service.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace mapget::detail
{
class TilesWsSession;
}

namespace mapget
{

/**
 * Configuration for HttpService
 */
struct HttpServiceConfig
{
    bool watchConfig = false;
    std::chrono::milliseconds defaultTtl{0};
    /** Maximum number of jobs executing across the homogeneous service worker pool. */
    size_t workerCount = Service::defaultWorkerCount();
    /** Enable POST /cache/reset. Disabled by default. */
    bool cacheResetEnabled = false;
    /** Global caller gate for cache reset; required whenever the endpoint is enabled. */
    AuthHeaderRegexMap cacheResetAuthHeaderAlternatives;
    /** Enable the /location endpoint and initialize the configured lookup backend. */
    bool locationLookupEnabled = true;
    /** Optional SQLite database path for /location; defaults beside the mapget binary module. */
    std::optional<std::filesystem::path> locationDatabasePath;
    /** Server-side cap for accepted /location limit values. */
    uint32_t locationResultMaxLimit = 50;

    /**
     * Period between allocator trims which return unused heap pages to the OS.
     *
     * A periodic worker covers datasource initialization and interactive traffic,
     * neither of which necessarily completes a REST response. Zero disables it.
     */
#if defined(__linux__) && defined(__GLIBC__)
    std::chrono::seconds memoryTrimPeriod{10};
#else
    std::chrono::seconds memoryTrimPeriod{0};
#endif
};

class HttpService : public HttpServer, public Service
{
public:
    explicit HttpService(
        Cache::Ptr cache = std::make_shared<MemCache>(),
        const HttpServiceConfig& config = HttpServiceConfig{});
    ~HttpService() override;

protected:
    void setup(drogon::HttpAppFramework& app) override;

private:
    friend class detail::TilesWsSession;

    /** Queue one interactive reconciliation away from Drogon's I/O threads. */
    [[nodiscard]] bool enqueueInteractiveControlTask(std::function<void()> task);

    struct Impl;
    friend struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mapget
