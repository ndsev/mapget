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
     * endpoint, resolves its schema references, and caches complete metadata for
     * the lifetime of this object. No model constructor performs network I/O.
     * @param enableCompression Enable gzip compression for responses (default: true)
     */
    explicit HttpClient(std::string const& host, uint16_t port, AuthHeaders headers = {}, bool enableCompression = true);
    ~HttpClient();

    /**
     * Get the sources as they were retrieved when the Client was instantiated.
     */
    [[nodiscard]] std::vector<DataSourceInfo> sources() const;

    /** Complete source metadata with its actual catalog sourceId; preserves same-map sources. */
    [[nodiscard]] nlohmann::json sourceCatalog() const;

    /**
     * Fetch an exact schema artifact for a current authorized source. Blocks until ready.
     * Revalidates the source binding before reusing a shared decoded content-cache entry;
     * removed/rebound/unauthorized sources and invalid binary artifacts throw.
     */
    [[nodiscard]] std::shared_ptr<MapSchema const> getSchema(
        std::string const& sourceId, std::string const& schemaId);

    /**
     * Post a Request for a number of tiles from a particular map layer.
     */
    LayerTilesRequest::Ptr request(LayerTilesRequest::Ptr const& request);

    /** Post a one-shot server-side multi-channel filter request. */
    FeatureLayerFilterTilesRequest::Ptr filter(
        FeatureLayerFilterTilesRequest::Ptr const& request);

    /**
     * Discover associations through the public batch endpoint, without fetching payloads.
     * Blocks until completion. Transport/protocol errors throw; backend failures and unavailable
     * associations are returned as ObjectDiscoveryResult statuses and must be checked by callers.
     */
    ObjectDiscoveryResult discoverObjects(ObjectDiscoveryRequest const& request);

    /** Fetch one separately transferred named tile attachment. */
    std::optional<AttachmentResponse> attachment(
        AttachmentRequest const& request);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mapget
