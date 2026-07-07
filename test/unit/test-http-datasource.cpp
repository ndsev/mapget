#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/WebSocketClient.h>
#include <trantor/net/EventLoopThread.h>

#include "process.hpp"

#include "mapget/http-datasource/datasource-client.h"
#include "mapget/http-service/cli.h"
#include "mapget/http-service/http-client.h"
#include "mapget/http-service/http-service.h"
#include "mapget/log.h"
#include "mapget/model/info.h"
#include "mapget/model/stream.h"
#include "mapget/service/config.h"

#include "nlohmann/json.hpp"

#include "test-http-service-fixture.h"
#include "utility.h"

using namespace mapget;
namespace fs = std::filesystem;

namespace
{


constexpr int32_t kHttpTileIdValue = 131073;
constexpr int32_t kSecondHttpTileIdValue = 131076;
constexpr int32_t kThirdHttpTileIdValue = 131077;

class SyncHttpClient
{
public:
    SyncHttpClient(std::string host, uint16_t port)
    {
        loopThread_ = std::make_unique<trantor::EventLoopThread>("MapgetTestHttpClient");
        loopThread_->run();

        client_ = drogon::HttpClient::newHttpClient(
            fmt::format("http://{}:{}/", host, port),
            loopThread_->getLoop());
    }

    std::pair<drogon::ReqResult, drogon::HttpResponsePtr> get(
        std::string path,
        std::vector<std::pair<std::string, std::string>> headers = {})
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath(std::move(path));
        for (auto const& [key, value] : headers) {
            req->addHeader(key, value);
        }
        return client_->sendRequest(req);
    }

    std::pair<drogon::ReqResult, drogon::HttpResponsePtr> postJson(
        std::string path,
        std::string body,
        std::vector<std::pair<std::string, std::string>> headers = {})
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setPath(std::move(path));
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        for (auto const& [key, value] : headers) {
            req->addHeader(key, value);
        }
        req->setBody(std::move(body));
        return client_->sendRequest(req);
    }

private:
    std::unique_ptr<trantor::EventLoopThread> loopThread_;
    drogon::HttpClientPtr client_;
};

class WsTilesClient
{
public:
    WsTilesClient(uint16_t port, std::shared_ptr<LayerInfo> layerInfo, bool requireFeatureLayer = true)
        : pullClient_("127.0.0.1", port),
          layerInfo_(std::move(layerInfo)),
          requireFeatureLayer_(requireFeatureLayer),
          reader_(
              [this](auto&&, auto&&) { return layerInfo_; },
              [this](auto&& tile) {
                  if (requireFeatureLayer_ && tile->id().layer_ != LayerType::Features) {
                      setError("Unexpected tile layer type");
                      return;
                  }
                  receivedTileCount_.fetch_add(1, std::memory_order_relaxed);
              })
    {
        loopThread_ = std::make_unique<trantor::EventLoopThread>("MapgetTestWsClient");
        loopThread_->run();

        client_ = drogon::WebSocketClient::newWebSocketClient(
            fmt::format("ws://127.0.0.1:{}", port),
            loopThread_->getLoop());

        client_->setMessageHandler(
            [this](std::string&& msg,
                   const drogon::WebSocketClientPtr&,
                   const drogon::WebSocketMessageType& msgType) {
                if (msgType != drogon::WebSocketMessageType::Binary) {
                    return;
                }
                handleBinaryMessage(std::move(msg));
            });
    }

    bool connect(bool sendAuthHeader, std::string_view path = "/interactive")
    {
        auto connectReq = drogon::HttpRequest::newHttpRequest();
        connectReq->setMethod(drogon::Get);
        connectReq->setPath(std::string(path));
        if (sendAuthHeader) {
            connectReq->addHeader("X-USER-ROLE", "Tropico-Viewer");
        }

        std::promise<drogon::ReqResult> connectPromise;
        auto connectFuture = connectPromise.get_future();
        client_->connectToServer(
            connectReq,
            [&connectPromise](
                drogon::ReqResult result,
                const drogon::HttpResponsePtr&,
                const drogon::WebSocketClientPtr&) { connectPromise.set_value(result); });

        if (connectFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            return false;
        }
        return connectFuture.get() == drogon::ReqResult::Ok;
    }

    drogon::WebSocketConnectionPtr connection() const { return client_->getConnection(); }

    void send(std::string_view payload)
    {
        auto conn = connection();
        if (conn && conn->connected()) {
            conn->send(std::string(payload), drogon::WebSocketMessageType::Text);
        }
    }

    [[nodiscard]] bool waitForDone(std::chrono::seconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(mutex_);
                if (!error_.empty() || (lastStatus_.has_value() && lastStatus_->value("allDone", false))) {
                    return true;
                }
            }

            const auto remainingMs = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
            if (remainingMs <= 0) {
                break;
            }

            const auto clientId = clientId_.load(std::memory_order_relaxed);
            if (clientId > 0) {
                const auto waitMs = std::clamp<int64_t>(remainingMs, 1, 1000);
                const auto [result, resp] = pullClient_.get(fmt::format(
                    "/interactive/payload?clientId={}&waitMs={}&maxBytes={}",
                    clientId,
                    waitMs,
                    64 * 1024 * 1024));

                if (result != drogon::ReqResult::Ok || !resp) {
                    setError("Failed to pull next tile frame");
                    return true;
                }

                if (resp->statusCode() == drogon::k200OK) {
                    try {
                        std::lock_guard readerLock(readerMutex_);
                        reader_.read(std::string(resp->body()));
                    }
                    catch (const std::exception& e) {
                        setError(std::string("Failed to parse pulled tile stream: ") + e.what());
                        return true;
                    }
                }
                else if (resp->statusCode() == drogon::k204NoContent) {
                    continue;
                }
                else if (resp->statusCode() == drogon::k410Gone) {
                    setError("Tiles pull session closed");
                    return true;
                }
                else {
                    setError(fmt::format("Unexpected /interactive/payload response status: {}", static_cast<int>(resp->statusCode())));
                    return true;
                }

                continue;
            }

            std::unique_lock lock(mutex_);
            cv_.wait_for(
                lock,
                std::chrono::milliseconds(std::min<int64_t>(remainingMs, 50)),
                [this] {
                    return !error_.empty()
                        || clientId_.load(std::memory_order_relaxed) > 0
                        || (lastStatus_.has_value() && lastStatus_->value("allDone", false));
                });
        }

        std::lock_guard lock(mutex_);
        return !error_.empty() || (lastStatus_.has_value() && lastStatus_->value("allDone", false));
    }

    void resetStatus()
    {
        std::lock_guard lock(mutex_);
        lastStatus_.reset();
        error_.clear();
    }

    void resetTileCount() { receivedTileCount_.store(0, std::memory_order_relaxed); }

    std::optional<nlohmann::json> lastStatus() const
    {
        std::lock_guard lock(mutex_);
        return lastStatus_;
    }

    std::string error() const
    {
        std::lock_guard lock(mutex_);
        return error_;
    }

    int receivedTileCount() const { return receivedTileCount_.load(std::memory_order_relaxed); }

    void stop() { client_->stop(); }

private:
    void handleBinaryMessage(std::string&& msg)
    {
        TileLayerStream::MessageType type = TileLayerStream::MessageType::None;
        uint32_t payloadSize = 0;
        size_t headerBytes = 0;
        auto bytes = std::span<const uint8_t>{
            reinterpret_cast<const uint8_t*>(msg.data()),
            msg.size()};
        if (!TileLayerStream::Reader::readMessageHeader(bytes, type, payloadSize, &headerBytes)) {
            setError("Failed to read stream message header");
            return;
        }
        if (bytes.size() < headerBytes + payloadSize) {
            setError("Invalid stream message size");
            return;
        }

        if (type == TileLayerStream::MessageType::Status) {
            auto payload = std::string_view{
                msg.data() + static_cast<std::ptrdiff_t>(headerBytes),
                payloadSize};
            try {
                auto parsed = nlohmann::json::parse(payload);
                {
                    std::lock_guard lock(mutex_);
                    lastStatus_ = std::move(parsed);
                }
                cv_.notify_all();
            }
            catch (const std::exception& e) {
                setError(std::string("Failed to parse status JSON: ") + e.what());
            }
            return;
        }

        if (type == TileLayerStream::MessageType::RequestContext) {
            auto payload = std::string_view{
                msg.data() + static_cast<std::ptrdiff_t>(headerBytes),
                payloadSize};
            try {
                auto parsed = nlohmann::json::parse(payload);
                if (parsed.contains("clientId") && parsed["clientId"].is_number_integer()) {
                    clientId_.store(parsed["clientId"].get<int64_t>(), std::memory_order_relaxed);
                    cv_.notify_all();
                }
            }
            catch (const std::exception& e) {
                setError(std::string("Failed to parse request-context JSON: ") + e.what());
            }
            return;
        }

        try {
            std::lock_guard readerLock(readerMutex_);
            reader_.read(msg);
        }
        catch (const std::exception& e) {
            setError(std::string("Failed to parse tile stream: ") + e.what());
        }
    }

    void setError(std::string message)
    {
        {
            std::lock_guard lock(mutex_);
            error_ = std::move(message);
        }
        cv_.notify_all();
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<nlohmann::json> lastStatus_;
    std::string error_;
    std::mutex readerMutex_;
    std::atomic_int receivedTileCount_{0};
    std::atomic_int64_t clientId_{0};
    std::unique_ptr<trantor::EventLoopThread> loopThread_;
    drogon::WebSocketClientPtr client_;
    SyncHttpClient pullClient_;
    std::shared_ptr<LayerInfo> layerInfo_;
    bool requireFeatureLayer_{true};
    TileLayerStream::Reader reader_;
};

class ChildProcessWithPort
{
public:
    explicit ChildProcessWithPort(std::string exePath)
    {
        auto stderrCallback = [](const char* bytes, size_t n) {
            auto output = std::string(bytes, n);
            output.erase(output.find_last_not_of(" \n\r\t") + 1);
            if (!output.empty())
                std::cerr << output << std::endl;
        };

        auto stdoutCallback = [this](const char* bytes, size_t n) {
            std::lock_guard<std::mutex> lock(mutex_);
            stdoutBuffer_.append(bytes, n);

            for (;;) {
                auto nl = stdoutBuffer_.find_first_of("\r\n");
                if (nl == std::string::npos)
                    break;

                auto line = stdoutBuffer_.substr(0, nl);
                stdoutBuffer_.erase(0, nl + 1);
                line.erase(line.find_last_not_of(" \n\r\t") + 1);

                if (!portReady_) {
                    std::regex portRegex(R"(Running on port (\d+))");
                    std::smatch matches;
                    if (std::regex_search(line, matches, portRegex) && matches.size() > 1) {
                        port_ = static_cast<uint16_t>(std::stoi(matches.str(1)));
                        portReady_ = true;
                        cv_.notify_all();
                    }
                }
            }
        };

        process_ = std::make_unique<TinyProcessLib::Process>(
            fmt::format("\"{}\"", exePath),
            "",
            stdoutCallback,
            stderrCallback,
            true);

        std::unique_lock<std::mutex> lock(mutex_);
#if defined(NDEBUG)
        if (!cv_.wait_for(lock, std::chrono::seconds(10), [this] { return portReady_; })) {
            raise("Timeout waiting for the child process to start listening.");
        }
#else
        log().warn("Using Debug build: will wait forever!");
        cv_.wait(lock, [this] { return portReady_; });
#endif
    }

    ~ChildProcessWithPort()
    {
        if (process_) {
            process_->kill(true);
            process_->get_exit_status();
        }
    }

    [[nodiscard]] uint16_t port() const
    {
        return port_;
    }

private:
    std::unique_ptr<TinyProcessLib::Process> process_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string stdoutBuffer_;
    uint16_t port_ = 0;
    bool portReady_ = false;
};

nlohmann::json testDataSourceInfoJson()
{
    using nlohmann::json;
    return json::parse(R"(
    {
        "nodeId": "test-datasource",
        "mapId": "Tropico",
        "layers": {
            "WayLayer": {
                "featureTypes":
                [
                    {
                        "name": "Way",
                        "uniqueIdCompositions":
                        [
                            [
                                {
                                    "partId": "areaId",
                                    "description": "String which identifies the map area.",
                                    "datatype": "STR"
                                },
                                {
                                    "partId": "wayId",
                                    "description": "Globally Unique 32b integer.",
                                    "datatype": "U32"
                                }
                            ]
                        ]
                    }
                ]
            },
            "SourceData-WayLayer": {
                "type": "SourceData"
            }
        }
    }
    )");
}

}  // namespace

TEST_CASE("HttpDataSource", "[HttpDataSource]")
{
    setLogLevel("trace", log());

    // Start datasource server in a separate process (Drogon is singleton).
    ChildProcessWithPort dsProc(MAPGET_TEST_DATASOURCE_SERVER_EXE);

    // Expected datasource info.
    auto info = DataSourceInfo::fromJson(testDataSourceInfoJson());

    SyncHttpClient dsClient("127.0.0.1", dsProc.port());

    // Fetch /info
    {
        auto [result, resp] = dsClient.get("/info");
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(resp != nullptr);
        REQUIRE(resp->statusCode() == drogon::k200OK);

        auto fetchedInfo = DataSourceInfo::fromJson(nlohmann::json::parse(std::string(resp->body())));
        REQUIRE(fetchedInfo.toJson() == info.toJson());
    }

    // Fetch /tile
    {
        auto [result, resp] = dsClient.get(fmt::format("/tile?layer=WayLayer&tileId={}", kHttpTileIdValue));
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(resp != nullptr);
        REQUIRE(resp->statusCode() == drogon::k200OK);

        auto receivedTileCount = 0;
        TileLayerStream::Reader reader(
            [&](auto&& mapId, auto&& layerId)
            {
                REQUIRE(mapId == info.mapId_);
                return info.getLayer(std::string(layerId));
            },
            [&](auto&& tile) {
                REQUIRE(tile->id().layer_ == LayerType::Features);
                receivedTileCount++;
            });
        reader.read(std::string(resp->body()));

        REQUIRE(receivedTileCount == 1);
    }

    // Fetch /tile SourceData
    {
        auto [result, resp] = dsClient.get(fmt::format("/tile?layer=SourceData-WayLayer&tileId={}", kHttpTileIdValue));
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(resp != nullptr);
        REQUIRE(resp->statusCode() == drogon::k200OK);

        auto receivedTileCount = 0;
        TileLayerStream::Reader reader(
            [&](auto&& mapId, auto&& layerId)
            {
                REQUIRE(mapId == info.mapId_);
                return info.getLayer(std::string(layerId));
            },
            [&](auto&& tile) {
                REQUIRE(tile->id().layer_ == LayerType::SourceData);
                receivedTileCount++;
            });
        reader.read(std::string(resp->body()));

        REQUIRE(receivedTileCount == 1);
    }

    // Fetch /locate
    {
        auto [result, resp] = dsClient.postJson(
            "/locate",
            R"({
                "mapId": "Tropico",
                "typeId": "Way",
                "featureId": ["wayId", 0]
            })");

        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(resp != nullptr);
        REQUIRE(resp->statusCode() == drogon::k200OK);

        LocateResponse responseParsed(nlohmann::json::parse(std::string(resp->body()))[0]);
        REQUIRE(responseParsed.tileKey_.mapId_ == "Tropico");
        REQUIRE(responseParsed.tileKey_.layer_ == LayerType::Features);
        REQUIRE(responseParsed.tileKey_.layerId_ == "WayLayer");
        REQUIRE(responseParsed.tileKey_.tileId_.value() == kHttpTileIdValue);
    }

    // Query mapget HTTP service (in-process, started once for entire test binary)
    {
        auto& service = test::httpService();
        auto remoteDataSource = std::make_shared<RemoteDataSource>("127.0.0.1", dsProc.port());
        service.add(remoteDataSource);

        // `/sources` keeps the legacy array body while exposing catalog metadata.
        {
            SyncHttpClient serviceClient("127.0.0.1", service.port());
            auto [result, resp] = serviceClient.get("/sources");
            REQUIRE(result == drogon::ReqResult::Ok);
            REQUIRE(resp != nullptr);
            REQUIRE(resp->statusCode() == drogon::k200OK);
            REQUIRE_FALSE(resp->getHeader("X-Mapget-Sources-Revision").empty());
            REQUIRE(resp->getHeader("X-Mapget-Sources-Config-Status") == "ok");

            auto sources = nlohmann::json::parse(std::string(resp->body()));
            REQUIRE(sources.is_array());
            REQUIRE_FALSE(sources.empty());
            auto const& source = sources.front();
            REQUIRE(source.value("status", "") == "ready");
            REQUIRE(source.contains("sourceId"));
            REQUIRE(source.contains("configIndex"));

            auto etag = resp->getHeader("ETag");
            REQUIRE_FALSE(etag.empty());
            auto [notModifiedResult, notModifiedResp] = serviceClient.get(
                "/sources",
                {{"If-None-Match", etag}});
            REQUIRE(notModifiedResult == drogon::ReqResult::Ok);
            REQUIRE(notModifiedResp != nullptr);
            REQUIRE(notModifiedResp->statusCode() == drogon::k304NotModified);

            auto [nonBlockingResult, nonBlockingResp] = serviceClient.get("/sources?blocking=false");
            REQUIRE(nonBlockingResult == drogon::ReqResult::Ok);
            REQUIRE(nonBlockingResp != nullptr);
            REQUIRE(nonBlockingResp->statusCode() == drogon::k200OK);
        }

        auto countReceivedTiles = [](auto& client, auto mapId, auto layerId, auto tiles) {
            auto tileCount = 0;
            auto request = std::make_shared<LayerTilesRequest>(mapId, layerId, tiles);
            request->onFeatureLayer([&](auto&&) { tileCount++; });
            client.request(request)->wait();
            return std::make_tuple(request, tileCount);
        };

        // Query through mapget HTTP service
        {
            HttpClient client("127.0.0.1", service.port());

            auto [request, receivedTileCount] = countReceivedTiles(
                client,
                "Tropico",
                "WayLayer",
                std::vector<TileId>{TileId::fromValue(kHttpTileIdValue), TileId::fromValue(kSecondHttpTileIdValue), TileId::fromValue(kThirdHttpTileIdValue)});

            REQUIRE(receivedTileCount == 3);
            REQUIRE(request->getStatus() == RequestStatus::Success);
        }

        // Search through the dedicated REST endpoint and C++ client helper.
        {
            SyncHttpClient serviceClient("127.0.0.1", service.port());
            auto searchBody = nlohmann::json::object({
                {"query", "typeId == 'Way'"},
                {"scope", "feature"},
                {"withFields", nlohmann::json::array({"typeId"})},
                {"responseType", "jsonl"},
                {"requests", nlohmann::json::array({nlohmann::json::object({
                    {"mapId", "Tropico"},
                    {"layerId", "WayLayer"},
                    {"tileIds", nlohmann::json::array({kHttpTileIdValue})},
                })})},
            }).dump();

            auto [result, resp] = serviceClient.postJson("/search", searchBody);
            REQUIRE(result == drogon::ReqResult::Ok);
            REQUIRE(resp != nullptr);
            REQUIRE(resp->statusCode() == drogon::k200OK);

            bool sawResultLayer = false;
            std::istringstream lines(std::string(resp->body()));
            std::string line;
            while (std::getline(lines, line)) {
                if (line.empty()) {
                    continue;
                }
                auto parsed = nlohmann::json::parse(line);
                if (parsed.value("type", "") != "SearchResultCollection") {
                    continue;
                }
                sawResultLayer = true;
                REQUIRE(parsed["results"].size() == 1);
                REQUIRE(parsed["resultFields"] == nlohmann::json::array({"typeId"}));
                REQUIRE(parsed["info"].contains("searchId") == false);
            }
            REQUIRE(sawResultLayer);

            HttpClient client("127.0.0.1", service.port());
            FeatureLayerSearchRequest search;
            search.query_ = "typeId == 'Way'";
            search.withFields_ = {"typeId"};

            auto request = std::make_shared<FeatureLayerSearchTilesRequest>(
                "Tropico",
                "WayLayer",
                std::vector<TileId>{TileId::fromValue(kHttpTileIdValue)},
                std::move(search));
            size_t resultCount = 0;
            size_t statusCount = 0;
            request->onSearchResult([&](TileSearchResultLayer::Ptr layer) {
                resultCount += layer->size();
                REQUIRE(layer->info().contains("searchId") == false);
            });
            request->onStatus([&](nlohmann::json const&) {
                ++statusCount;
            });

            client.search(request)->wait();
            REQUIRE(request->getStatus() == RequestStatus::Success);
            REQUIRE(resultCount == 1);
            REQUIRE(statusCount > 0);
        }

        // POST /tiles no longer accepts search fields; REST clients must use /search.
        {
            SyncHttpClient serviceClient("127.0.0.1", service.port());
            auto [result, resp] = serviceClient.postJson(
                "/tiles",
                nlohmann::json::object({
                    {"searchId", "legacy-rest-search"},
                    {"searchQuery", "typeId == 'Way'"},
                    {"requests", nlohmann::json::array({nlohmann::json::object({
                        {"mapId", "Tropico"},
                        {"layerId", "WayLayer"},
                        {"tileIds", nlohmann::json::array({kHttpTileIdValue})},
                    })})},
                }).dump());

            REQUIRE(result == drogon::ReqResult::Ok);
            REQUIRE(resp != nullptr);
            REQUIRE(resp->statusCode() == drogon::k400BadRequest);
            REQUIRE(std::string(resp->body()).find("POST /search") != std::string::npos);
        }

        // Trigger 400 responses
        {
            HttpClient client("127.0.0.1", service.port());

            {
                auto [request, receivedTileCount] =
                    countReceivedTiles(client, "UnknownMap", "WayLayer", std::vector<TileId>{TileId::fromValue(kHttpTileIdValue)});
                REQUIRE(request->getStatus() == RequestStatus::NoDataSource);
                REQUIRE(receivedTileCount == 0);
            }

            {
                auto [request, receivedTileCount] =
                    countReceivedTiles(client, "Tropico", "UnknownLayer", std::vector<TileId>{TileId::fromValue(kHttpTileIdValue)});
                REQUIRE(request->getStatus() == RequestStatus::NoDataSource);
                REQUIRE(receivedTileCount == 0);
            }
        }

        // Run /locate through service
        {
            SyncHttpClient serviceClient("127.0.0.1", service.port());

            auto [result, resp] = serviceClient.postJson(
                "/locate",
                R"({
                    "requests": [{
                        "mapId": "Tropico",
                        "typeId": "Way",
                        "featureId": ["wayId", 0]
                    }]
                })");

            REQUIRE(result == drogon::ReqResult::Ok);
            REQUIRE(resp != nullptr);
            REQUIRE(resp->statusCode() == drogon::k200OK);

            auto responseJsonLists = nlohmann::json::parse(std::string(resp->body()))["responses"];
            REQUIRE(responseJsonLists.size() == 1);
            auto responseJsonList = responseJsonLists[0];
            REQUIRE(responseJsonList.size() == 1);
            LocateResponse responseParsed(responseJsonList[0]);
            REQUIRE(responseParsed.tileKey_.mapId_ == "Tropico");
            REQUIRE(responseParsed.tileKey_.layer_ == LayerType::Features);
            REQUIRE(responseParsed.tileKey_.layerId_ == "WayLayer");
            REQUIRE(responseParsed.tileKey_.tileId_.value() == kHttpTileIdValue);
        }

        // Test auth header requirement
        {
            remoteDataSource->requireAuthHeaderRegexMatchOption("X-USER-ROLE", std::regex("\\bTropico-Viewer\\b"));

            HttpClient badClient("127.0.0.1", service.port());
            HttpClient goodClient("127.0.0.1", service.port(), {{"X-USER-ROLE", "Tropico-Viewer"}});

            REQUIRE(badClient.sources().empty());
            REQUIRE(goodClient.sources().size() == 1);

            {
                auto [request, receivedTileCount] =
                    countReceivedTiles(badClient, "Tropico", "WayLayer", std::vector<TileId>{TileId::fromValue(kHttpTileIdValue)});
                REQUIRE(request->getStatus() == RequestStatus::Unauthorized);
                REQUIRE(receivedTileCount == 0);
            }

            {
                auto [request, receivedTileCount] =
                    countReceivedTiles(goodClient, "Tropico", "WayLayer", std::vector<TileId>{TileId::fromValue(kHttpTileIdValue)});
                REQUIRE(request->getStatus() == RequestStatus::Success);
                REQUIRE(receivedTileCount == 1);
            }

            const auto dsInfo = remoteDataSource->info();
            const auto layerInfo = dsInfo.getLayer("WayLayer");
            REQUIRE(layerInfo != nullptr);

            auto requireConnected = [](WsTilesClient& wsClient) {
                auto conn = wsClient.connection();
                if (!conn || !conn->connected()) {
                    wsClient.stop();
                    FAIL("WebSocket connection not established");
                }
                return conn;
            };

            auto runWsTilesRequestAtPath = [&](std::string_view path, bool sendAuthHeader, const std::string& requestJson) {
                WsTilesClient wsClient(service.port(), layerInfo);

                REQUIRE(wsClient.connect(sendAuthHeader, path));
                requireConnected(wsClient)->send(requestJson, drogon::WebSocketMessageType::Text);

                REQUIRE(wsClient.waitForDone(std::chrono::seconds(10)));
                if (!wsClient.error().empty()) {
                    wsClient.stop();
                    FAIL(wsClient.error());
                }

                auto status = wsClient.lastStatus();
                wsClient.stop();

                REQUIRE(status.has_value());
                return std::make_tuple(*status, wsClient.receivedTileCount());
            };

            auto runWsTilesRequest = [&](bool sendAuthHeader, const std::string& requestJson) {
                return runWsTilesRequestAtPath("/interactive", sendAuthHeader, requestJson);
            };

            // WebSocket tiles: `/tiles` remains a legacy alias for `/interactive`.
            {
                auto req = nlohmann::json::object({
                    {"requests", nlohmann::json::array({nlohmann::json::object({
                        {"mapId", "Tropico"},
                        {"layerId", "WayLayer"},
                        {"tileIds", nlohmann::json::array({kHttpTileIdValue})},
                    })})},
                }).dump();

                auto [status, wsTileCount] = runWsTilesRequestAtPath("/tiles", true, req);
                REQUIRE(wsTileCount == 1);
                REQUIRE(status["requests"].size() == 1);
                REQUIRE(status["requests"][0]["status"].get<int>() ==
                        static_cast<int>(RequestStatus::Success));
            }

            // WebSocket tiles: unauthorized without auth header.
            {
                auto req = nlohmann::json::object({
                    {"requests", nlohmann::json::array({nlohmann::json::object({
                        {"mapId", "Tropico"},
                        {"layerId", "WayLayer"},
                        {"tileIds", nlohmann::json::array({kHttpTileIdValue})},
                    })})},
                }).dump();

                auto [status, wsTileCount] = runWsTilesRequest(false, req);
                REQUIRE(wsTileCount == 0);
                REQUIRE(status["requests"].size() == 1);
                REQUIRE(status["requests"][0]["status"].get<int>() ==
                        static_cast<int>(RequestStatus::Unauthorized));
            }

            // WebSocket tiles: include noDataSourceReason in status payload when available.
            {
                auto req = nlohmann::json::object({
                    {"requests", nlohmann::json::array({nlohmann::json::object({
                        {"mapId", "UnknownMap"},
                        {"layerId", "WayLayer"},
                        {"tileIds", nlohmann::json::array({kHttpTileIdValue})},
                    })})},
                }).dump();

                auto [status, wsTileCount] = runWsTilesRequest(true, req);
                REQUIRE(wsTileCount == 0);
                REQUIRE(status["requests"].size() == 1);
                REQUIRE(status["requests"][0]["status"].get<int>() ==
                        static_cast<int>(RequestStatus::NoDataSource));
                REQUIRE(status["requests"][0]["noDataSourceReason"].get<std::string>() == "missingMapOrLayer");
            }

            // WebSocket tiles: invalid request stays on the same connection, then succeeds.
            {
                WsTilesClient wsClient(service.port(), layerInfo);
                REQUIRE(wsClient.connect(true));

                auto conn = requireConnected(wsClient);

                // Invalid JSON: should yield a Status message but keep the socket open.
                {
                    conn->send("{not json", drogon::WebSocketMessageType::Text);
                    REQUIRE(wsClient.waitForDone(std::chrono::seconds(5)));
                    if (!wsClient.error().empty()) {
                        wsClient.stop();
                        FAIL(wsClient.error());
                    }

                    auto status = wsClient.lastStatus();
                    REQUIRE(status.has_value());
                    REQUIRE(status->value("message", "").find("Invalid JSON") != std::string::npos);
                    REQUIRE(conn->connected());
                }

                // Valid request should succeed afterwards.
                {
                    wsClient.resetStatus();
                    wsClient.resetTileCount();

                    auto req = nlohmann::json::object({
                        {"requests", nlohmann::json::array({nlohmann::json::object({
                            {"mapId", "Tropico"},
                            {"layerId", "WayLayer"},
                            {"tileIds", nlohmann::json::array({kHttpTileIdValue})},
                        })})},
                    }).dump();

                    conn->send(req, drogon::WebSocketMessageType::Text);

                    REQUIRE(wsClient.waitForDone(std::chrono::seconds(10)));
                    if (!wsClient.error().empty()) {
                        wsClient.stop();
                        FAIL(wsClient.error());
                    }

                    auto status = wsClient.lastStatus();
                    REQUIRE(wsClient.receivedTileCount() == 1);
                    REQUIRE(status.has_value());
                    REQUIRE(status->contains("requests"));
                    REQUIRE((*status)["requests"].size() == 1);
                    REQUIRE((*status)["requests"][0]["status"].get<int>() ==
                            static_cast<int>(RequestStatus::Success));
                }

                wsClient.stop();
            }

            // WebSocket tiles: single-stage layers must also work through staged bucket requests.
            {
                auto req = nlohmann::json::object({
                    {"requests", nlohmann::json::array({nlohmann::json::object({
                        {"mapId", "Tropico"},
                        {"layerId", "WayLayer"},
                        {"tileIdsByNextStage", nlohmann::json::array({
                            nlohmann::json::array({kHttpTileIdValue}),
                        })},
                    })})},
                }).dump();

                auto [status, wsTileCount] = runWsTilesRequest(true, req);
                REQUIRE(wsTileCount == 1);
                REQUIRE(status["requests"].size() == 1);
                REQUIRE(status["requests"][0]["status"].get<int>() ==
                        static_cast<int>(RequestStatus::Success));
            }
        }

        service.remove(remoteDataSource);
    }
}

TEST_CASE("Configuration Endpoint Tests", "[Configuration]")
{
    auto& service = test::httpService();
    REQUIRE(service.isRunning() == true);

    SyncHttpClient cli("127.0.0.1", service.port());

    auto tempDir = fs::current_path() / test::generateTimestampedDirectoryName("mapget_test_http_config");
    fs::create_directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";
    auto tempMissingConfigPath = tempDir / "missing_config.yaml";

    DataSourceConfigService::get().reset();
    struct ConfigWatchGuard {
        ~ConfigWatchGuard() { DataSourceConfigService::get().end(); }
    } configWatchGuard;
    struct SchemaPatchGuard {
        ~SchemaPatchGuard() { DataSourceConfigService::get().setDataSourceConfigSchemaPatch(nlohmann::json::object()); }
    } schemaPatchGuard;
    struct EndpointToggleGuard {
        ~EndpointToggleGuard()
        {
            setGetConfigEndpointEnabled(true);
            setPostConfigEndpointEnabled(false);
        }
    } endpointToggleGuard;

    auto schemaPatch = nlohmann::json::parse(R"(
    {
        "properties": {
            "http-settings": {
                "type": "array"
            }
        },
        "required": ["sources", "http-settings"]
    }
    )");
    DataSourceConfigService::get().setDataSourceConfigSchemaPatch(schemaPatch);

    DataSourceConfigService::get().registerPublicConfigSection(
        "publicConfig",
        [](YAML::Node const& fullConfig) -> nlohmann::json {
            auto section = fullConfig["publicConfig"];
            if (!section || !section.IsMap() || section.size() == 0) {
                return nlohmann::json::object();
            }
            return yamlToJson(section, false);
        });

    auto writeConfigFile = [&](std::string const& contents) {
        std::ofstream configFile(tempConfigPath);
        configFile << contents;
        configFile.flush();
        configFile.close();
#ifndef _WIN32
        int fd = open(tempConfigPath.c_str(), O_RDONLY);
        if (fd != -1) {
            fsync(fd);
            close(fd);
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };

    auto getConfigPayload = [&]() {
        auto [result, res] = cli.get("/config");
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(res != nullptr);
        REQUIRE(res->statusCode() == drogon::k200OK);
        return nlohmann::json::parse(std::string(res->body()));
    };

    auto requireUnavailablePayload = [&](
        std::string_view reason,
        bool expectPublicConfig = false,
        bool expectReadOnly = true,
        bool expectSchema = false) {
        auto payload = getConfigPayload();
        REQUIRE(payload["datasourceConfigUnavailable"].get<bool>() == true);
        REQUIRE(payload["datasourceConfigUnavailableReason"].get<std::string>() == reason);
        REQUIRE(payload["model"] == nlohmann::json::object());
        REQUIRE(payload["readOnly"].get<bool>() == expectReadOnly);
        if (expectSchema) {
            REQUIRE(payload["schema"].is_object());
            REQUIRE(payload["schema"].empty() == false);
        }
        else {
            REQUIRE(payload["schema"] == nlohmann::json::object());
        }
        if (expectPublicConfig) {
            REQUIRE(payload["publicConfig"]["featureFlag"].get<bool>() == true);
        }
        else {
            REQUIRE(payload["publicConfig"] == nlohmann::json::object());
        }
    };

    writeConfigFile(
        "sources: []\n"
        "http-settings:\n"
        "  - password: hunter2\n"
        "    apiKey: camel-secret\n"
        "    oauth2:\n"
        "      clientSecret: oauth-secret\n"
        "publicConfig:\n"
        "  featureFlag: true\n"
        "erdblick:\n"
        "  keepMe: true\n");
    DataSourceConfigService::get().loadConfig(tempConfigPath.string());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    SECTION("Get Configuration - Config File Missing")
    {
        DataSourceConfigService::get().loadConfig(tempMissingConfigPath.string());
        requireUnavailablePayload("configFileMissing");
    }

    SECTION("Get Configuration - Endpoint disabled returns flagged 200")
    {
        setGetConfigEndpointEnabled(false);
        requireUnavailablePayload("getConfigDisabled", true);
    }

    SECTION("Get Configuration - Endpoint hidden but POST enabled returns writable empty model")
    {
        setGetConfigEndpointEnabled(false);
        setPostConfigEndpointEnabled(true);
        requireUnavailablePayload("getConfigDisabled", true, false, true);
    }

    SECTION("Get Configuration - No Config File Path Set")
    {
        DataSourceConfigService::get().loadConfig("");
        requireUnavailablePayload("configPathUnset");
    }

    SECTION("Get Configuration - Validation failure is flagged")
    {
        writeConfigFile("sources: []\n");
        DataSourceConfigService::get().loadConfig(tempConfigPath.string());
        requireUnavailablePayload("configValidationFailed");
    }

    SECTION("Get Configuration - Success")
    {
        auto payload = getConfigPayload();
        REQUIRE(payload["datasourceConfigUnavailable"].get<bool>() == false);
        REQUIRE(payload["datasourceConfigUnavailableReason"].is_null());
        REQUIRE(payload["readOnly"].get<bool>() == true);
        REQUIRE(payload["model"].contains("sources"));
        REQUIRE(payload["model"].contains("http-settings"));
        REQUIRE(payload["model"].contains("publicConfig") == false);
        REQUIRE(payload["publicConfig"]["featureFlag"].get<bool>() == true);

        auto body = payload.dump();
        REQUIRE(body.find("hunter2") == std::string::npos);
        REQUIRE(body.find("camel-secret") == std::string::npos);
        REQUIRE(body.find("oauth-secret") == std::string::npos);

        auto settings = payload["model"]["http-settings"][0];
        auto passwordToken = settings["password"].get<std::string>();
        auto apiKeyToken = settings["apiKey"].get<std::string>();
        auto clientSecretToken = settings["oauth2"]["clientSecret"].get<std::string>();
        REQUIRE(passwordToken.starts_with("MASKED:"));
        REQUIRE(apiKeyToken.starts_with("MASKED:"));
        REQUIRE(clientSecretToken.starts_with("MASKED:"));
        REQUIRE(passwordToken != apiKeyToken);
        REQUIRE(apiKeyToken != clientSecretToken);
    }

    SECTION("Get Configuration - Public section serializer exceptions are tolerated")
    {
        struct ThrowingSectionError : std::runtime_error {
            using std::runtime_error::runtime_error;
        };

        DataSourceConfigService::get().registerPublicConfigSection(
            "throwingSection",
            [](YAML::Node const&) -> nlohmann::json { throw ThrowingSectionError("boom"); });

        auto payload = getConfigPayload();
        REQUIRE(payload["datasourceConfigUnavailable"].get<bool>() == false);
        REQUIRE(payload["throwingSection"] == nlohmann::json::object());
    }

    SECTION("Post Configuration - Not Enabled")
    {
        setPostConfigEndpointEnabled(false);
        auto [result, res] = cli.postJson("/config", "");
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(res != nullptr);
        REQUIRE(res->statusCode() == drogon::k403Forbidden);
    }

    SECTION("Post Configuration - Invalid JSON Format")
    {
        setPostConfigEndpointEnabled(true);
        auto [result, res] = cli.postJson("/config", "this is not valid json");
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(res != nullptr);
        REQUIRE(res->statusCode() == drogon::k400BadRequest);
        REQUIRE(std::string(res->body()).find("Invalid JSON format") != std::string::npos);
    }

    SECTION("Post Configuration - Missing Sources")
    {
        setPostConfigEndpointEnabled(true);
        auto [result, res] = cli.postJson("/config", R"({"http-settings": []})");
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(res != nullptr);
        REQUIRE(res->statusCode() == drogon::k500InternalServerError);
        REQUIRE(std::string(res->body()).starts_with("Validation failed"));
    }

    SECTION("Post Configuration - Missing Http Settings")
    {
        setPostConfigEndpointEnabled(true);
        auto [result, res] = cli.postJson("/config", R"({"sources": []})");
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(res != nullptr);
        REQUIRE(res->statusCode() == drogon::k500InternalServerError);
        REQUIRE(std::string(res->body()).starts_with("Validation failed"));
    }

    SECTION("Post Configuration - Valid JSON Config preserves top-level erdblick")
    {
        setPostConfigEndpointEnabled(true);
        auto payload = getConfigPayload();
        auto settings = payload["model"]["http-settings"][0];
        auto newConfig = nlohmann::json::object({
            {"sources", nlohmann::json::array({
                nlohmann::json::object({{"type", "TestDataSource"}})
            })},
            {"http-settings", nlohmann::json::array({
                nlohmann::json::object({
                    {"scope", "https://example.com"},
                    {"password", settings["password"].get<std::string>()},
                    {"apiKey", settings["apiKey"].get<std::string>()},
                    {"oauth2", nlohmann::json::object({
                        {"clientSecret", settings["oauth2"]["clientSecret"].get<std::string>()}
                    })}
                })
            })}
        });

        auto [result, res] = cli.postJson("/config", newConfig.dump());
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(res != nullptr);
        REQUIRE(res->statusCode() == drogon::k200OK);
        REQUIRE(std::string(res->body()) == "Configuration updated and applied successfully.");

        std::ifstream config(*mapget::DataSourceConfigService::get().getConfigFilePath());
        std::stringstream configContentStream;
        configContentStream << config.rdbuf();
        auto configContent = configContentStream.str();
        REQUIRE(configContent.find("hunter2") != std::string::npos);
        REQUIRE(configContent.find("camel-secret") != std::string::npos);
        REQUIRE(configContent.find("oauth-secret") != std::string::npos);
        REQUIRE(configContent.find("MASKED:") == std::string::npos);
        REQUIRE(configContent.find("erdblick") != std::string::npos);
        REQUIRE(configContent.find("keepMe") != std::string::npos);
    }

    DataSourceConfigService::get().end();
    fs::remove_all(tempDir);
}
