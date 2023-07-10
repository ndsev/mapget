#include "datasource-client.h"
#include "process.hpp"
#include <chrono>
#include <regex>

namespace mapget
{

RemoteDataSource::RemoteDataSource(const std::string& host, uint16_t port)
    : httpClient_(host, port)
{
}

DataSourceInfo RemoteDataSource::info()
{
    auto fetchedInfoJson = httpClient_.Get("/info");
    auto fetchedInfo = DataSourceInfo::fromJson(nlohmann::json::parse(fetchedInfoJson->body));
    return fetchedInfo;
}

void RemoteDataSource::fill(const TileFeatureLayer::Ptr& featureTile)
{
    // If we get here, an error occurred.
    featureTile->setError("Error while contacting remote data source.");
}

TileFeatureLayer::Ptr
RemoteDataSource::get(const MapTileKey& k, Cache::Ptr& cache, const DataSourceInfo& info)
{
    // Send a GET tile request
    auto tileResponse = httpClient_.Get(stx::format(
        "/tile?layer={}&tileId={}&fieldsOffset={}",
        k.layerId_,
        k.tileId_.value_,
        cachedFieldsOffset(info.nodeId_, cache)));

    // Check that the response is OK
    if (!tileResponse || tileResponse->status >= 300) {
        // Forward to base class get(). This will instantiate a
        // default TileFeatureLayer and call fill(). In our implementation
        // of fill, we set an error.
        // TODO: Read HTTPLIB_ERROR header, more log output.
        return DataSource::get(k, cache, info);
    }

    // Check the response body for expected content
    TileFeatureLayer::Ptr result;
    TileLayerStream::Reader reader(
        [&](auto&& mapId, auto&& layerId) { return info.getLayer(std::string(layerId)); },
        [&](auto&& tile) { result = tile; },
        cache);
    reader.read(tileResponse->body);

    return result;
}

RemoteDataSourceProcess::RemoteDataSourceProcess(std::string const& command_line)
    : process_(std::make_unique<TinyProcessLib::Process>(
          command_line,
          "",
          [this](const char* bytes, size_t n)
          {
              auto output = std::string(bytes, n);
              std::cout << "Process output: " << output;
              // Extract port number from the message "Running on port <port>"
              std::regex port_regex(R"(Running on port (\d+))");
              std::smatch matches;
              if (std::regex_search(output, matches, port_regex)) {
                  if (matches.size() > 1) {
                      std::lock_guard<std::mutex> lock(mutex_);
                      uint16_t port = std::stoi(matches.str(1));
                      remoteSource_ = std::make_unique<RemoteDataSource>("127.0.0.1", port);
                      cv_.notify_all();
                  }
              }
          },
          nullptr,
          true))
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::seconds(10), [this] { return remoteSource_ != nullptr; }))
    {
        throw std::runtime_error(
            "Timeout waiting for the child process to initialize the remote data source.");
    }
}

RemoteDataSourceProcess::~RemoteDataSourceProcess()
{
    if (process_) {
        process_->kill();
    }
}

// DataSource method overrides
DataSourceInfo RemoteDataSourceProcess::info()
{
    if (!remoteSource_)
        throw std::runtime_error("Remote data source is not initialized.");
    return remoteSource_->info();
}

void RemoteDataSourceProcess::fill(TileFeatureLayer::Ptr const& featureTile)
{
    if (!remoteSource_)
        throw std::runtime_error("Remote data source is not initialized.");
    remoteSource_->fill(featureTile);
}

TileFeatureLayer::Ptr
RemoteDataSourceProcess::get(MapTileKey const& k, Cache::Ptr& cache, DataSourceInfo const& info)
{
    if (!remoteSource_)
        throw std::runtime_error("Remote data source is not initialized.");
    return remoteSource_->get(k, cache, info);
}

}