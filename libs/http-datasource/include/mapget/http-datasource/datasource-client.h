#pragma once

#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/featurelayer.h"
#include "mapget/service/datasource.h"

#include <memory>
#include <atomic>
#include <condition_variable>
#include <vector>

namespace TinyProcessLib {
    class Process;
}

namespace drogon {
class HttpClient;
}

namespace trantor {
class EventLoopThread;
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
     * Construct from joint host:port string.
     */
    static std::shared_ptr<RemoteDataSource> fromHostPort(std::string const& hostPort);

    /**
     * Construct a DataSource with the host and port of
     * a running DataSourceServer. Throws if the connection
     * fails for any reason.
     */
    RemoteDataSource(std::string const& host, uint16_t port);
    ~RemoteDataSource();

    // DataSource method overrides
    DataSourceInfo info() override;
    void fill(TileFeatureLayer::Ptr const& featureTile) override;
    void fill(TileSourceDataLayer::Ptr const& blobTile) override;
    TileLayer::Ptr get(
        MapTileKey const& k,
        Cache::Ptr& cache,
        DataSourceInfo const& info,
        TileLayer::LoadStateCallback loadStateCallback = {}) override;
    std::vector<LocateCandidate> locate(
        mapget::LocateRequest const& req) override;
    std::optional<AttachmentResponse> attachment(
        AttachmentRequest const& request) override;
    void onCacheExpired(
        MapTileKey const& tileKey,
        std::chrono::system_clock::time_point expiredAt) override;

private:
    // DataSourceInfo is fetched in the constructor
    DataSourceInfo info_;

    // Error string, written in get() and set in fill().
    std::string error_;

    // Multiple http clients allow parallel GET requests
    std::unique_ptr<trantor::EventLoopThread> httpClientLoop_;
    std::vector<std::shared_ptr<drogon::HttpClient>> httpClients_;
    std::atomic_uint64_t nextClient_{0};
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
    void fill(TileSourceDataLayer::Ptr const& sourceDataLayer) override;
    TileLayer::Ptr get(
        MapTileKey const& k,
        Cache::Ptr& cache,
        DataSourceInfo const& info,
        TileLayer::LoadStateCallback loadStateCallback = {}) override;
    std::vector<LocateCandidate> locate(
        mapget::LocateRequest const& req) override;
    std::optional<AttachmentResponse> attachment(
        AttachmentRequest const& request) override;
    void onCacheExpired(
        MapTileKey const& tileKey,
        std::chrono::system_clock::time_point expiredAt) override;

private:
    std::unique_ptr<RemoteDataSource> remoteSource_;
    std::unique_ptr<TinyProcessLib::Process> process_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

}
