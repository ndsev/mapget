#pragma once

#include "httplib.h"
#include "mapget/detail/http-server.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/stream.h"
#include "mapget/service/service.h"

#include <mutex>
#include <utility>

namespace mapget {

/**
 * Configuration for HttpService
 */
struct HttpServiceConfig {
    bool watchConfig = false;
    
    /**
     * Memory trim intervals - control when to explicitly trim the allocator
     * to return unused memory to the OS (e.g., malloc_trim on Linux).
     * 
     * Some allocators are 'lazy' and keep freed memory for future reuse,
     * which can lead to high memory peaks. This option allows explicit
     * trimming to avoid this situation (if supported by the platform).
     * 
     * Different intervals for different response types since JSON/GeoJSON
     * responses cause significantly more allocations than binary responses.
     * 
     * 0 = disabled (never trim)
     * 1 = after every request of that type
     * N = after every N requests of that type
     * 
     * Note: Only has effect on platforms that support explicit memory trimming.
     */
#ifdef __linux__
    uint64_t memoryTrimIntervalBinary = 1000;  // Default: trim every 1000 binary requests on Linux
    uint64_t memoryTrimIntervalJson = 5;       // Default: trim every 5 JSON requests on Linux (more aggressive)
#else
    uint64_t memoryTrimIntervalBinary = 0;     // Default: disabled on other platforms
    uint64_t memoryTrimIntervalJson = 0;       // Default: disabled on other platforms  
#endif
    
    // Future configuration options can be added here
};

class HttpService : public HttpServer, public Service
{
public:
    explicit HttpService(
        Cache::Ptr cache = std::make_shared<MemCache>(),
        const HttpServiceConfig& config = HttpServiceConfig{});
    ~HttpService() override;

protected:
    void setup(httplib::Server& server) override;

private:
    struct Impl;
    friend struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mapget
