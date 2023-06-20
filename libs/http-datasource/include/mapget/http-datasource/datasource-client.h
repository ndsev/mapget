#pragma once

#include "mapget/service/datasource.h"
#include "httplib.h"

namespace mapget
{

/**
 * DataSource which connects to a running DataSourceServer.
 */
class RemoteDataSource : public DataSource
{
public:
    /**
     * Construct a DataSource with the host and port of
     * a running DataSourceServer. Throws if the connection
     * fails for any reason.
     */
    RemoteDataSource(std::string const& host, uint16_t port);

    // DataSource method overrides
    DataSourceInfo info() override;
    void fill(TileFeatureLayer::Ptr const& featureTile) override;
    TileFeatureLayer::Ptr get(MapTileKey const& k, Cache::Ptr& cache, DataSourceInfo const& info) override;

private:
    httplib::Client httpClient_;
};

}