#pragma once

#include "mapget/service/datasource.h"
#include "httplib.h"

#include <memory>
#include <condition_variable>

namespace TinyProcessLib {
    class Process;
}

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

/**
 * Remote data source which manages the lifetime of the associated data source
 * server process. Starts the server executable, waits until the server is running
 * and ready to serve data, and stops it when it is deleted.
 * Parses the server's "Running on port <port>" message to determine the port.
 */
class RemoteDataSourceProcess : public DataSource
{
public:
    /**
     * Construct a remote data source with a command-line command.
     * Throws if the connection fails for any reason or times out after 10 seconds.
     */
    RemoteDataSourceProcess(std::string const& commandLine);

    /**
     * Destructor ensures that the server process is terminated.
     */
    ~RemoteDataSourceProcess();

    // DataSource method overrides
    DataSourceInfo info() override;
    void fill(TileFeatureLayer::Ptr const& featureTile) override;
    TileFeatureLayer::Ptr get(MapTileKey const& k, Cache::Ptr& cache, DataSourceInfo const& info) override;

private:
    std::unique_ptr<RemoteDataSource> remoteSource_;
    std::unique_ptr<TinyProcessLib::Process> process_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

}