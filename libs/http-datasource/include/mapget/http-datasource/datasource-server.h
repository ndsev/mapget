#pragma once

#include <chrono>
#include <functional>
#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/featurelayer.h"
#include "mapget/detail/http-server.h"
#include "mapget/service/datasource.h"
#include "mapget/service/locate.h"

namespace mapget
{

/**
 * Small server, which implements the data source protocol expected by a mapget server.
 * Construct it with DataSourceInfo metadata describing the map data it provides.
 * The metadata is fixed and bound to a finalized schema.
 *
 * Configure callbacks before go(); callbacks can overlap and must synchronize shared
 * state. The adapter constructs partition models and serializes the data they fill.
 */
class DataSourceServer : public HttpServer
{
public:
    /**
     * Construct a server with complete, schema-bound DataSourceInfo.
     * GET /info advertises the small catalog; GET /schema?schemaId=... serves the
     * exact immutable binary artifact off the I/O thread. RemoteDataSource and
     * RemoteDataSourceProcess use these endpoints to initialize a datasource
     * connection. The public mapget service performs source authorization before
     * serving that datasource's schema to its own clients.
     */
    explicit DataSourceServer(DataSourceInfo const& info);

    /** Destructor */
    ~DataSourceServer() override;

    /**
     * Set the callbacks which will be invoked when a `/tile`-request is received.
     * The callback receives a fresh partition layer to fill according to its
     * metadata and tagged tile/object partition ID.
     * If an error occurs while filling the tile, the callback can use
     * PartitionLayer::setError(...) to signal the error downstream.
     */
    DataSourceServer& onTileFeatureRequest(std::function<void(PartitionFeatureLayer::Ptr)> const&);
    DataSourceServer&
    onTileSourceDataRequest(std::function<void(PartitionSourceDataLayer::Ptr)> const&);

    /**
     * Set the callback which will be invoked when a `/locate`-request is received.
     * The callback argument is a LocateRequest, which the callback
     * must process according to its available data.
     */
    DataSourceServer&
    onLocateRequest(
        std::function<
            std::vector<LocateCandidate>(
                LocateRequest const&)> const&);

    /**
     * Install lookup for the remote single-query /objects/discover endpoint ({layerId, tileId}).
     * Return associations only, not object payloads; successful empty lists differ from
     * Unavailable. The callback may overlap fill/other requests, so synchronize shared state
     * and use bounded I/O. This is not the public service's map-scoped batch endpoint.
     */
    DataSourceServer& onObjectDiscoveryRequest(
        std::function<ObjectDiscoveryResult(ObjectDiscoveryRequest const&)> const&);

    /** Set the callback which produces separately transferred tile attachments. */
    DataSourceServer& onAttachmentRequest(
        std::function<std::optional<AttachmentResponse>(
            AttachmentRequest const&)> const&);

    /**
     * Set the callback which will be invoked when a service reports that a
     * cached tile for this remote datasource expired and is being refreshed.
     */
    DataSourceServer& onCacheExpired(
        std::function<void(MapPartitionKey const&, std::chrono::system_clock::time_point)> const&);

    /**
     * Get the DataSourceInfo metadata which this instance was constructed with.
     */
    DataSourceInfo const& info();

private:
    void setup(drogon::HttpAppFramework&) override;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
