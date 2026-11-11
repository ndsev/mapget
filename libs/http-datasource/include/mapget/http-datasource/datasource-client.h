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
 * DataSource which connects to a running DataSourceServer. Initialization fetches
 * compact /info metadata and resolves /schema before publishing usable metadata.
 * Immutable decoded schemas are shared by exact validated content identity.
 */
class RemoteDataSource : public DataSource
{
public:
    /**
     * Construct from joint host:port string.
     */
    static std::shared_ptr<RemoteDataSource> fromHostPort(
        std::string const& hostPort, TileCancellationCheck const& isCancelled = {});

    /**
     * Construct a DataSource with the host and port of
     * a running DataSourceServer. Throws if the connection
     * fails for any reason. Cancellation is checked around initialization I/O
     * and shared schema waits; it never cancels another source's schema build.
     */
    RemoteDataSource(
        std::string const& host, uint16_t port, TileCancellationCheck const& isCancelled = {});
    ~RemoteDataSource();

    // DataSource method overrides
    DataSourceInfo info() override;
    void fill(PartitionFeatureLayer::Ptr const& featureTile) override;
    void fill(PartitionSourceDataLayer::Ptr const& blobTile) override;
    PartitionLayer::Ptr
    get(MapPartitionKey const& k,
        Cache::Ptr& cache,
        DataSourceInfo const& info,
        PartitionLayer::LoadStateCallback loadStateCallback = {},
        TileCancellationCheck const& isCancelled = {}) override;
    std::vector<LocateCandidate> locate(
        mapget::LocateRequest const& req) override;

    /** Forward spatial discovery without fetching object payloads. */
    ObjectDiscoveryResult discoverObjects(ObjectDiscoveryRequest const& request) override;
    std::optional<AttachmentResponse> attachment(
        AttachmentRequest const& request) override;
    void
    onCacheExpired(MapPartitionKey const& tileKey, std::chrono::system_clock::time_point expiredAt)
        override;

private:
    // DataSourceInfo is fetched in the constructor
    DataSourceInfo info_;

    // Failed request description set by get() and copied into the fallback tile by fill().
    std::string error_;

    // Clients share one event loop and allow parallel GET requests. A schema
    // download can be shared by several RemoteDataSource initializers. If one
    // initializer is cancelled, the download retains this loop until the other
    // waiters have received the result, even if that initializer is destroyed.
    std::shared_ptr<trantor::EventLoopThread> httpClientLoop_;
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
     * Release builds throw if initialization is not ready within 10 seconds.
     * Debug builds wait indefinitely to allow the child process to be debugged.
     */
    RemoteDataSourceProcess(std::string const& commandLine);

    /**
     * Destructor ensures that the server process is terminated.
     */
    ~RemoteDataSourceProcess();

    // DataSource method overrides
    DataSourceInfo info() override;
    void fill(PartitionFeatureLayer::Ptr const& featureTile) override;
    void fill(PartitionSourceDataLayer::Ptr const& sourceDataLayer) override;
    PartitionLayer::Ptr
    get(MapPartitionKey const& k,
        Cache::Ptr& cache,
        DataSourceInfo const& info,
        PartitionLayer::LoadStateCallback loadStateCallback = {},
        TileCancellationCheck const& isCancelled = {}) override;
    std::vector<LocateCandidate> locate(
        mapget::LocateRequest const& req) override;

    /** Forward spatial discovery without fetching object payloads. */
    ObjectDiscoveryResult discoverObjects(ObjectDiscoveryRequest const& request) override;
    std::optional<AttachmentResponse> attachment(
        AttachmentRequest const& request) override;
    void
    onCacheExpired(MapPartitionKey const& tileKey, std::chrono::system_clock::time_point expiredAt)
        override;

private:
    std::unique_ptr<RemoteDataSource> remoteSource_;
    std::unique_ptr<TinyProcessLib::Process> process_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

}
