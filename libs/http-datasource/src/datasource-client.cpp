#include "datasource-client.h"
#include "process.hpp"
#include <chrono>
#include <regex>
#include "mapget/log.h"

namespace mapget
{

RemoteDataSource::RemoteDataSource(const std::string& host, uint16_t port)
{
    // Fetch data source info.
    httplib::Client client(host, port);
    auto fetchedInfoJson = client.Get("/info");
    if (!fetchedInfoJson || fetchedInfoJson->status >= 300)
        throw logRuntimeError("Failed to fetch datasource info.");
    info_ = DataSourceInfo::fromJson(nlohmann::json::parse(fetchedInfoJson->body));

    if (info_.nodeId_.empty()) {
        // Unique node IDs are required for the field offsets.
        throw logRuntimeError(
            fmt::format("Remote data source is missing node ID! Source info: {}",
                fetchedInfoJson->body));
    }

    // Create as many clients as parallel requests are allowed.
    for (auto i = 0; i < std::max(info_.maxParallelJobs_, 1); ++i)
        httpClients_.emplace_back(host, port);
}

DataSourceInfo RemoteDataSource::info()
{
    return info_;
}

void RemoteDataSource::fill(const TileFeatureLayer::Ptr& featureTile)
{
    // If we get here, an error occurred.
    featureTile->setError(fmt::format("Error while contacting remote data source: {}", error_));
}

TileFeatureLayer::Ptr
RemoteDataSource::get(const MapTileKey& k, Cache::Ptr& cache, const DataSourceInfo& info)
{
    // Round-robin usage of http clients to facilitate parallel requests.
    auto& client = httpClients_[(nextClient_++) % httpClients_.size()];

    // Send a GET tile request.
    auto tileResponse = client.Get(fmt::format(
        "/tile?layer={}&tileId={}&fieldsOffset={}",
        k.layerId_,
        k.tileId_.value_,
        cachedFieldsOffset(info.nodeId_, cache)));

    // Check that the response is OK.
    if (!tileResponse || tileResponse->status >= 300) {
        // Forward to base class get(). This will instantiate a
        // default TileFeatureLayer and call fill(). In our implementation
        // of fill, we set an error.
        if (tileResponse->has_header("HTTPLIB_ERROR")) {
            error_ = tileResponse->get_header_value("HTTPLIB_ERROR");
        }
        else if (tileResponse->has_header("EXCEPTION_WHAT")) {
            error_ = tileResponse->get_header_value("EXCEPTION_WHAT");
        }
        else {
            error_ = fmt::format("Code {}", tileResponse->status);
        }

        // Use tile instantiation logic of the base class,
        // the error is then set in fill().
        return DataSource::get(k, cache, info);
    }

    // Check the response body for expected content.
    TileFeatureLayer::Ptr result;
    TileLayerStream::Reader reader(
        [&](auto&& mapId, auto&& layerId) { return info.getLayer(std::string(layerId)); },
        [&](auto&& tile) { result = tile; },
        cache);
    reader.read(tileResponse->body);

    return result;
}

std::vector<LocateResponse> RemoteDataSource::locate(const LocateRequest& req)
{
    // Round-robin usage of http clients to facilitate parallel requests.
    auto& client = httpClients_[(nextClient_++) % httpClients_.size()];

    // Send a GET tile request.
    auto locateResponse = client.Post(
        fmt::format("/locate"), req.serialize().dump(), "application/json");

    // Check that the response is OK.
    if (!locateResponse || locateResponse->status >= 300) {
        // Forward to base class get(). This will instantiate a
        // default TileFeatureLayer and call fill(). In our implementation
        // of fill, we set an error.
        // TODO: Read HTTPLIB_ERROR header, more log output.
        return {};
    }

    // Check the response body for expected content.
    auto responseJson = nlohmann::json::parse(locateResponse->body);
    if (responseJson.is_null()) {
        return {};
    }

    // Parse the resulting responses.
    std::vector<LocateResponse> responseVector;
    for (auto const& responseJsonAlternative : responseJson) {
        responseVector.emplace_back(responseJsonAlternative);
    }
    return responseVector;
}

RemoteDataSourceProcess::RemoteDataSourceProcess(std::string const& commandLine)
{
    auto stderrCallback = [this](const char* bytes, size_t n)
    {
        auto output = std::string(bytes, n);
        // Trim trailing newline/whitespace.
        output.erase(output.find_last_not_of(" \n\r\t")+1);
        std::cerr << output << std::endl;
    };

    auto stdoutCallback = [this](const char* bytes, size_t n)
    {
        auto output = std::string(bytes, n);
        // Trim trailing newline/whitespace.
        output.erase(output.find_last_not_of(" \n\r\t")+1);
        if (!remoteSource_) {
            // Extract port number from the message "Running on port <port>".
            std::regex port_regex(R"(Running on port (\d+))");
            std::smatch matches;
            if (std::regex_search(output, matches, port_regex)) {
                if (matches.size() > 1) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    uint16_t port = std::stoi(matches.str(1));
                    remoteSource_ = std::make_unique<RemoteDataSource>("127.0.0.1", port);
                    cv_.notify_all();
                }
                return;
            }
        }

        log().debug("datasource stdout: {}", output);
    };

    process_ = std::make_unique<TinyProcessLib::Process>(
        commandLine,
        "",
        stdoutCallback,
        stderrCallback,
        true);

    std::unique_lock<std::mutex> lock(mutex_);
#if defined(NDEBUG)
    if (!cv_.wait_for(lock, std::chrono::seconds(10), [this] { return remoteSource_ != nullptr; }))
    {
        throw logRuntimeError(
            "Timeout waiting for the child process to initialize the remote data source.");
    }
#else
    cv_.wait(lock, [this] { return remoteSource_ != nullptr; });
#endif
}

RemoteDataSourceProcess::~RemoteDataSourceProcess()
{
    if (process_) {
        process_->kill(true);
        process_->get_exit_status();
    }
}

// DataSource method overrides
DataSourceInfo RemoteDataSourceProcess::info()
{
    if (!remoteSource_)
        throw logRuntimeError("Remote data source is not initialized.");
    return remoteSource_->info();
}

void RemoteDataSourceProcess::fill(TileFeatureLayer::Ptr const& featureTile)
{
    if (!remoteSource_)
        throw logRuntimeError("Remote data source is not initialized.");
    remoteSource_->fill(featureTile);
}

TileFeatureLayer::Ptr
RemoteDataSourceProcess::get(MapTileKey const& k, Cache::Ptr& cache, DataSourceInfo const& info)
{
    if (!remoteSource_)
        throw logRuntimeError("Remote data source is not initialized.");
    return remoteSource_->get(k, cache, info);
}

std::vector<LocateResponse> RemoteDataSourceProcess::locate(const LocateRequest& req)
{
    if (!remoteSource_)
        throw logRuntimeError("Remote data source is not initialized.");
    return remoteSource_->locate(req);
}

}
