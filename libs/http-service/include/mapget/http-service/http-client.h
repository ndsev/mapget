#pragma once

#include "http-service.h"
#include "mapget/service/service.h"

#include <vector>

namespace mapget {

/**
 * Client class, which implements asynchronous fetching from a mapget HTTP service.
 */
class HttpClient
{
public:
    /**
     * Connect to a running mapget HTTP service. Immediately calls the /sources
     * endpoint, and caches the result for the lifetime of this object.
     * @param enableCompression Enable gzip compression for responses (default: true)
     */
    explicit HttpClient(std::string const& host, uint16_t port, AuthHeaders headers = {}, bool enableCompression = true);
    ~HttpClient();

    /**
     * Get the sources as they were retrieved when the Client was instantiated.
     */
    [[nodiscard]] std::vector<DataSourceInfo> sources() const;

    /**
     * Post a Request for a number of tiles from a particular map layer.
     */
    LayerTilesRequest::Ptr request(LayerTilesRequest::Ptr const& request);

    /** Post a one-shot server-side multi-channel filter request. */
    FeatureLayerFilterTilesRequest::Ptr filter(
        FeatureLayerFilterTilesRequest::Ptr const& request);

    /** Fetch one separately transferred named tile attachment. */
    std::optional<AttachmentResponse> attachment(
        AttachmentRequest const& request);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mapget
