#include "datasource-client.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/stream.h"
#include "process.hpp"
#include "mapget/log.h"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <trantor/net/EventLoopThread.h>

#include <chrono>
#include <regex>

namespace mapget
{

RemoteDataSource::RemoteDataSource(const std::string& host, uint16_t port)
{
    httpClientLoop_ = std::make_unique<trantor::EventLoopThread>("MapgetRemoteDataSource");
    httpClientLoop_->run();

    const auto hostString = fmt::format("http://{}:{}/", host, port);

    // Fetch data source info.
    auto infoClient = drogon::HttpClient::newHttpClient(hostString, httpClientLoop_->getLoop());
    auto infoReq = drogon::HttpRequest::newHttpRequest();
    infoReq->setMethod(drogon::Get);
    infoReq->setPath("/info");

    auto [result, fetchedInfoResp] = infoClient->sendRequest(infoReq);
    if (result != drogon::ReqResult::Ok || !fetchedInfoResp) {
        raise(fmt::format("Failed to fetch datasource info: [{}]", drogon::to_string_view(result)));
    }
    if ((int)fetchedInfoResp->statusCode() >= 300) {
        raise(fmt::format("Failed to fetch datasource info: [{}]", (int)fetchedInfoResp->statusCode()));
    }
    auto infoJson = nlohmann::json::parse(std::string(fetchedInfoResp->body()));
    auto const protocolVersionIt = infoJson.find("protocolVersion");
    if (protocolVersionIt == infoJson.end()) {
        raise(fmt::format(
            "Remote data source is missing protocolVersion; expected mapget protocol {}.",
            TileLayerStream::CurrentProtocolVersion.toString()));
    }
    auto const remoteProtocolVersion = Version::fromJson(*protocolVersionIt);
    if (!remoteProtocolVersion.isCompatible(TileLayerStream::CurrentProtocolVersion)) {
        raise(fmt::format(
            "Remote data source protocol {} is incompatible with mapget protocol {}.",
            remoteProtocolVersion.toString(),
            TileLayerStream::CurrentProtocolVersion.toString()));
    }
    info_ = DataSourceInfo::fromJson(infoJson);

    if (info_.nodeId_.empty()) {
        // Unique node IDs are required for the string pool offsets.
        raise(
            fmt::format("Remote data source is missing node ID! Source info: {}",
                std::string(fetchedInfoResp->body())));
    }

    // Create as many clients as parallel requests are allowed.
    const auto clientCount = (std::max)(info_.maxParallelJobs_, 1);
    httpClients_.reserve(clientCount);
    for (auto i = 0; i < clientCount; ++i) {
        httpClients_.emplace_back(drogon::HttpClient::newHttpClient(hostString, httpClientLoop_->getLoop()));
    }
}

RemoteDataSource::~RemoteDataSource() = default;

DataSourceInfo RemoteDataSource::info()
{
    return info_;
}

void RemoteDataSource::fill(const TileFeatureLayer::Ptr& featureTile)
{
    // If we get here, an error occurred.
    featureTile->setError(fmt::format("Error while contacting remote data source: {}", error_));
}

void RemoteDataSource::fill(const TileSourceDataLayer::Ptr& blobTile)
{
    // If we get here, an error occurred.
    blobTile->setError(fmt::format("Error while contacting remote data source: {}", error_));
}

TileLayer::Ptr
RemoteDataSource::get(
    const MapTileKey& k,
    Cache::Ptr& cache,
    const DataSourceInfo& info,
    TileLayer::LoadStateCallback loadStateCallback)
{
    // Round-robin usage of http clients to facilitate parallel requests.
    auto& client = httpClients_[(nextClient_++) % httpClients_.size()];

    // Send a GET tile request.
    auto tileReq = drogon::HttpRequest::newHttpRequest();
    tileReq->setMethod(drogon::Get);
    tileReq->setPath(fmt::format(
        "/tile?layer={}&tileId={}&stage={}&stringPoolOffset={}",
        k.layerId_,
        k.tileId_.value(),
        k.stage_,
        cachedStringPoolOffset(info.nodeId_, cache)));
    auto [resultCode, tileResponse] = client->sendRequest(tileReq);

    // Check that the response is OK.
    if (resultCode != drogon::ReqResult::Ok || !tileResponse || (int)tileResponse->statusCode() >= 300) {
        // Forward to base class get(). This will instantiate a
        // default TileLayer and call fill(). In our implementation
        // of fill, we set an error.

        if (resultCode != drogon::ReqResult::Ok) {
            error_ = drogon::to_string(resultCode);
        } else if (tileResponse) {
            error_ = fmt::format("Code {}", (int)tileResponse->statusCode());
        } else {
            error_ = "No remote response.";
        }

        // Use tile instantiation logic of the base class,
        // the error is then set in fill().
        return DataSource::get(k, cache, info, std::move(loadStateCallback));
    }

    // Check the response body for expected content.
    TileLayer::Ptr result;
    TileLayerStream::Reader reader(
        [&](auto&& mapId, auto&& layerId) { return info.getLayer(std::string(layerId)); },
        [&](auto&& tile) { result = tile; },
        cache);
    reader.read(std::string(tileResponse->body()));

    if (result && loadStateCallback) {
        result->setLoadStateCallback(std::move(loadStateCallback));
    }
    return result;
}

std::vector<LocateResponse> RemoteDataSource::locate(const LocateRequest& req)
{
    // Round-robin usage of http clients to facilitate parallel requests.
    auto& client = httpClients_[(nextClient_++) % httpClients_.size()];

    auto locateReq = drogon::HttpRequest::newHttpRequest();
    locateReq->setMethod(drogon::Post);
    locateReq->setPath("/locate");
    locateReq->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    locateReq->setBody(req.serialize().dump());
    auto [resultCode, locateResponse] = client->sendRequest(locateReq);

    // Check that the response is OK.
    if (resultCode != drogon::ReqResult::Ok || !locateResponse || (int)locateResponse->statusCode() >= 300) {
        // Forward to base class get(). This will instantiate a
        // default TileFeatureLayer and call fill(). In our implementation
        // of fill, we set an error.
        // TODO: Read HTTPLIB_ERROR header, more log output.
        return {};
    }

    // Check the response body for expected content.
    auto responseJson = nlohmann::json::parse(std::string(locateResponse->body()));
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

void RemoteDataSource::onCacheExpired(
    MapTileKey const& tileKey,
    std::chrono::system_clock::time_point expiredAt)
{
    auto& client = httpClients_[(nextClient_++) % httpClients_.size()];

    auto cacheExpiredReq = drogon::HttpRequest::newHttpRequest();
    cacheExpiredReq->setMethod(drogon::Post);
    cacheExpiredReq->setPath("/cache-expired");
    cacheExpiredReq->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    cacheExpiredReq->setBody(nlohmann::json{
        {"tileKey", tileKey.toString()},
        {"expiredAt", std::chrono::duration_cast<std::chrono::microseconds>(
            expiredAt.time_since_epoch()).count()},
    }.dump());

    auto [resultCode, response] = client->sendRequest(cacheExpiredReq);
    if (resultCode != drogon::ReqResult::Ok || !response || (int)response->statusCode() >= 300) {
        log().warn(
            "Failed to notify remote data source about cache expiry for {}: {}",
            tileKey.toString(),
            resultCode != drogon::ReqResult::Ok
                ? drogon::to_string(resultCode)
                : response ? fmt::format("HTTP {}", (int)response->statusCode()) : "No remote response");
    }
}

std::shared_ptr<RemoteDataSource> RemoteDataSource::fromHostPort(const std::string& hostPort)
{
    auto delimiterPos = hostPort.find(':');
    std::string dsHost = hostPort.substr(0, delimiterPos);
    int dsPort = std::stoi(hostPort.substr(delimiterPos + 1, hostPort.size()));
    log().info("Connecting to datasource at {}:{}.", dsHost, dsPort);
    return std::make_shared<RemoteDataSource>(dsHost, dsPort);
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
        raise(
            "Timeout waiting for the child process to initialize the remote data source.");
    }
#else
    log().warn("Using Debug build: will wait forever!");
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
        raise("Remote data source is not initialized.");
    return remoteSource_->info();
}

void RemoteDataSourceProcess::fill(TileFeatureLayer::Ptr const& featureTile)
{
    if (!remoteSource_)
        raise("Remote data source is not initialized.");
    remoteSource_->fill(featureTile);
}

void RemoteDataSourceProcess::fill(TileSourceDataLayer::Ptr const& sourceDataLayer)
{
    if (!remoteSource_)
        raise("Remote data source is not initialized.");
    remoteSource_->fill(sourceDataLayer);
}

TileLayer::Ptr
RemoteDataSourceProcess::get(
    MapTileKey const& k,
    Cache::Ptr& cache,
    DataSourceInfo const& info,
    TileLayer::LoadStateCallback loadStateCallback)
{
    if (!remoteSource_)
        raise("Remote data source is not initialized.");
    return remoteSource_->get(k, cache, info, std::move(loadStateCallback));
}

std::vector<LocateResponse> RemoteDataSourceProcess::locate(const LocateRequest& req)
{
    if (!remoteSource_)
        raise("Remote data source is not initialized.");
    return remoteSource_->locate(req);
}

void RemoteDataSourceProcess::onCacheExpired(
    MapTileKey const& tileKey,
    std::chrono::system_clock::time_point expiredAt)
{
    if (!remoteSource_)
        raise("Remote data source is not initialized.");
    remoteSource_->onCacheExpired(tileKey, expiredAt);
}

}
