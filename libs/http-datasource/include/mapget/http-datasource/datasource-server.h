#pragma once

#include "mapget/model/featurelayer.h"
#include "mapget/detail/http-server.h"
#include "mapget/service/locate.h"

namespace mapget
{

/**
 * Small server, which implements the data source protocol expected
 * by a mapget server. The DataSource must be constructed with a
 * DataSourceInfo metadata instance, which describes the map data that
 * is provided by the source.
 */
class DataSourceServer : public HttpServer
{
public:
    /**
     * Construct a DataSource with a DataSourceInfo metadata instance.
     */
    explicit DataSourceServer(DataSourceInfo const& info);

    /** Destructor */
    ~DataSourceServer() override;

    /**
     * Set the callback which will be invoked when a `/tile`-request is received.
     * The callback argument is a fresh TileFeatureLayer, which the callback
     * must fill according to the set TileFeatureLayer's layer info and tile id.
     * If an error occurs while filling the tile, the callback can use
     * TileFeatureLayer::setError(...) to signal the error downstream.
     */
    DataSourceServer& onTileRequest(std::function<void(TileFeatureLayer::Ptr)> const&);

    /**
     * Set the callback which will be invoked when a `/locate`-request is received.
     * The callback argument is a LocateRequest, which the callback
     * must process according to its available data.
     */
    DataSourceServer&
    onLocateRequest(std::function<std::optional<LocateResponse>(LocateRequest const&)> const&);

    /**
     * Get the DataSourceInfo metadata which this instance was constructed with.
     */
    DataSourceInfo const& info();

private:
    void setup(httplib::Server&) override;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
