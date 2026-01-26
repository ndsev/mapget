#include "http-service-impl.h"

#include "mapget/log.h"

#ifdef __linux__
#include <malloc.h>
#endif

namespace mapget
{

HttpService::Impl::Impl(HttpService& self, const HttpServiceConfig& config) : self_(self), config_(config) {}

void HttpService::Impl::tryMemoryTrim(ResponseType responseType) const
{
    uint64_t interval =
        (responseType == ResponseType::Binary) ? config_.memoryTrimIntervalBinary : config_.memoryTrimIntervalJson;

    if (interval == 0) {
        return;
    }

    auto& counter = (responseType == ResponseType::Binary) ? binaryRequestCounter_ : jsonRequestCounter_;
    auto count = counter.fetch_add(1, std::memory_order_relaxed);
    if ((count % interval) != 0) {
        return;
    }

#ifdef __linux__
#ifndef NDEBUG
    const char* typeStr = (responseType == ResponseType::Binary) ? "binary" : "JSON";
    log().debug("Trimming memory after {} {} requests (interval: {})", count, typeStr, interval);
#endif
    malloc_trim(0);
#endif
}

}  // namespace mapget

