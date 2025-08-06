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
     * Memory trim interval - controls when to explicitly trim the allocator
     * to return unused memory to the OS (e.g., malloc_trim on Linux).
     * 
     * Some allocators are 'lazy' and keep freed memory for future reuse,
     * which can lead to high memory peaks. This option allows explicit
     * trimming to avoid this situation (if supported by the platform).
     * 
     * 0 = disabled (never trim)
     * 1 = after every request
     * N = after every N requests
     * 
     * Note: Only has effect on platforms that support explicit memory trimming.
     */
#ifdef __linux__
    uint64_t memoryTrimInterval = 100;  // Default: trim every 100 requests on Linux
#else
    uint64_t memoryTrimInterval = 0;    // Default: disabled on other platforms
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
