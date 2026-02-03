#include <catch2/catch_test_macros.hpp>

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

    std::pair<drogon::ReqResult, drogon::HttpResponsePtr> get(std::string path)
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath(std::move(path));
        return client_->sendRequest(req);
    }

    std::pair<drogon::ReqResult, drogon::HttpResponsePtr> postJson(std::string path, std::string body)
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setPath(std::move(path));
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        req->setBody(std::move(body));
        return client_->sendRequest(req);
    }

private:
    std::unique_ptr<trantor::EventLoopThread> loopThread_;
    drogon::HttpClientPtr client_;
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
        auto [result, resp] = dsClient.get("/tile?layer=WayLayer&tileId=1");
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
        auto [result, resp] = dsClient.get("/tile?layer=SourceData-WayLayer&tileId=1");
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
        REQUIRE(responseParsed.tileKey_.tileId_.value_ == 1);
    }

    // Query mapget HTTP service (in-process, started once for entire test binary)
    {
        auto& service = test::httpService();
        auto remoteDataSource = std::make_shared<RemoteDataSource>("127.0.0.1", dsProc.port());
        service.add(remoteDataSource);

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
                std::vector<TileId>{{1234, 5678, 9112}});

            REQUIRE(receivedTileCount == 3);
            REQUIRE(request->getStatus() == RequestStatus::Success);
        }

        // Trigger 400 responses
        {
            HttpClient client("127.0.0.1", service.port());

            {
                auto [request, receivedTileCount] =
                    countReceivedTiles(client, "UnknownMap", "WayLayer", std::vector<TileId>{{1234}});
                REQUIRE(request->getStatus() == RequestStatus::NoDataSource);
                REQUIRE(receivedTileCount == 0);
            }

            {
                auto [request, receivedTileCount] =
                    countReceivedTiles(client, "Tropico", "UnknownLayer", std::vector<TileId>{{1234}});
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
            REQUIRE(responseParsed.tileKey_.tileId_.value_ == 1);
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
                    countReceivedTiles(badClient, "Tropico", "WayLayer", std::vector<TileId>{{1234}});
                REQUIRE(request->getStatus() == RequestStatus::Unauthorized);
                REQUIRE(receivedTileCount == 0);
            }

            {
                auto [request, receivedTileCount] =
                    countReceivedTiles(goodClient, "Tropico", "WayLayer", std::vector<TileId>{{1234}});
                REQUIRE(request->getStatus() == RequestStatus::Success);
                REQUIRE(receivedTileCount == 1);
            }

            auto runWsTilesRequest = [&](bool sendAuthHeader, std::string requestJson) {
                auto wsLoopThread = std::make_unique<trantor::EventLoopThread>("MapgetTestWsClient");
                wsLoopThread->run();

                auto wsClient = drogon::WebSocketClient::newWebSocketClient(
                    fmt::format("ws://127.0.0.1:{}", service.port()),
                    wsLoopThread->getLoop());

                std::mutex mutex;
                std::condition_variable cv;
                std::optional<nlohmann::json> lastStatus;
                std::atomic_int receivedTileCount{0};
                std::string error;

                const auto dsInfo = remoteDataSource->info();
                const auto layerInfo = dsInfo.getLayer("WayLayer");
                REQUIRE(layerInfo != nullptr);

                TileLayerStream::Reader reader(
                    [&](auto&&, auto&&) { return layerInfo; },
                    [&](auto&& tile) {
                        if (tile->id().layer_ != LayerType::Features) {
                            std::lock_guard lock(mutex);
                            error = "Unexpected tile layer type";
                        }
                        receivedTileCount.fetch_add(1, std::memory_order_relaxed);
                    });

                wsClient->setMessageHandler(
                    [&](std::string&& msg,
                        const drogon::WebSocketClientPtr&,
                        const drogon::WebSocketMessageType& msgType) {
                        if (msgType != drogon::WebSocketMessageType::Binary) {
                            return;
                        }

                        TileLayerStream::MessageType type = TileLayerStream::MessageType::None;
                        uint32_t payloadSize = 0;
                        std::stringstream ss;
                        ss.write(msg.data(), static_cast<std::streamsize>(msg.size()));
                        if (!TileLayerStream::Reader::readMessageHeader(ss, type, payloadSize)) {
                            std::lock_guard lock(mutex);
                            error = "Failed to read stream message header";
                            cv.notify_all();
                            return;
                        }

                        if (type == TileLayerStream::MessageType::Status) {
                            std::string payload(payloadSize, '\0');
                            ss.read(payload.data(), static_cast<std::streamsize>(payloadSize));
                            nlohmann::json parsed;
                            try {
                                parsed = nlohmann::json::parse(payload);
                            }
                            catch (const std::exception& e) {
                                std::lock_guard lock(mutex);
                                error = std::string("Failed to parse status JSON: ") + e.what();
                                cv.notify_all();
                                return;
                            }
                            {
                                std::lock_guard lock(mutex);
                                lastStatus = std::move(parsed);
                            }
                            cv.notify_all();
                            return;
                        }

                        try {
                            reader.read(msg);
                        }
                        catch (const std::exception& e) {
                            std::lock_guard lock(mutex);
                            error = std::string("Failed to parse tile stream: ") + e.what();
                            cv.notify_all();
                        }
                    });

                auto connectReq = drogon::HttpRequest::newHttpRequest();
                connectReq->setMethod(drogon::Get);
                connectReq->setPath("/tiles");
                if (sendAuthHeader) {
                    connectReq->addHeader("X-USER-ROLE", "Tropico-Viewer");
                }

                std::promise<drogon::ReqResult> connectPromise;
                auto connectFuture = connectPromise.get_future();
                wsClient->connectToServer(
                    connectReq,
                    [&connectPromise](
                        drogon::ReqResult result,
                        const drogon::HttpResponsePtr&,
                        const drogon::WebSocketClientPtr&) { connectPromise.set_value(result); });

                REQUIRE(connectFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
                REQUIRE(connectFuture.get() == drogon::ReqResult::Ok);

                auto conn = wsClient->getConnection();
                if (!conn || !conn->connected()) {
                    wsClient->stop();
                    FAIL("WebSocket connection not established");
                }

                conn->send(requestJson, drogon::WebSocketMessageType::Text);

                {
                    std::unique_lock lock(mutex);
                    REQUIRE(cv.wait_for(lock, std::chrono::seconds(10), [&] {
                        return !error.empty() ||
                               (lastStatus.has_value() && lastStatus->value("allDone", false));
                    }));
                    if (!error.empty()) {
                        wsClient->stop();
                        FAIL(error);
                    }
                }

                wsClient->stop();

                REQUIRE(lastStatus.has_value());
                return std::make_tuple(*lastStatus, receivedTileCount.load(std::memory_order_relaxed));
            };

            // WebSocket tiles: unauthorized without auth header.
            {
                auto req = nlohmann::json::object({
                    {"requests", nlohmann::json::array({nlohmann::json::object({
                        {"mapId", "Tropico"},
                        {"layerId", "WayLayer"},
                        {"tileIds", nlohmann::json::array({1234})},
                    })})},
                }).dump();

                auto [status, wsTileCount] = runWsTilesRequest(false, req);
                REQUIRE(wsTileCount == 0);
                REQUIRE(status["requests"].size() == 1);
                REQUIRE(status["requests"][0]["status"].get<int>() ==
                        static_cast<int>(RequestStatus::Unauthorized));
            }

            // WebSocket tiles: invalid request stays on the same connection, then succeeds.
            {
                auto wsLoopThread = std::make_unique<trantor::EventLoopThread>("MapgetTestWsClientReuse");
                wsLoopThread->run();

                auto wsClient = drogon::WebSocketClient::newWebSocketClient(
                    fmt::format("ws://127.0.0.1:{}", service.port()),
                    wsLoopThread->getLoop());

                std::mutex mutex;
                std::condition_variable cv;
                std::optional<nlohmann::json> lastStatus;
                std::atomic_int receivedTileCount{0};
                std::string error;

                const auto dsInfo = remoteDataSource->info();
                const auto layerInfo = dsInfo.getLayer("WayLayer");
                REQUIRE(layerInfo != nullptr);

                TileLayerStream::Reader reader(
                    [&](auto&&, auto&&) { return layerInfo; },
                    [&](auto&&) { receivedTileCount.fetch_add(1, std::memory_order_relaxed); });

                wsClient->setMessageHandler(
                    [&](std::string&& msg,
                        const drogon::WebSocketClientPtr&,
                        const drogon::WebSocketMessageType& msgType) {
                        if (msgType != drogon::WebSocketMessageType::Binary) {
                            return;
                        }

                        TileLayerStream::MessageType type = TileLayerStream::MessageType::None;
                        uint32_t payloadSize = 0;
                        std::stringstream ss;
                        ss.write(msg.data(), static_cast<std::streamsize>(msg.size()));
                        if (!TileLayerStream::Reader::readMessageHeader(ss, type, payloadSize)) {
                            std::lock_guard lock(mutex);
                            error = "Failed to read stream message header";
                            cv.notify_all();
                            return;
                        }

                        if (type == TileLayerStream::MessageType::Status) {
                            std::string payload(payloadSize, '\0');
                            ss.read(payload.data(), static_cast<std::streamsize>(payloadSize));
                            nlohmann::json parsed;
                            try {
                                parsed = nlohmann::json::parse(payload);
                            }
                            catch (const std::exception& e) {
                                std::lock_guard lock(mutex);
                                error = std::string("Failed to parse status JSON: ") + e.what();
                                cv.notify_all();
                                return;
                            }
                            {
                                std::lock_guard lock(mutex);
                                lastStatus = std::move(parsed);
                            }
                            cv.notify_all();
                            return;
                        }

                        try {
                            reader.read(msg);
                        }
                        catch (const std::exception& e) {
                            std::lock_guard lock(mutex);
                            error = std::string("Failed to parse tile stream: ") + e.what();
                            cv.notify_all();
                        }
                    });

                auto connectReq = drogon::HttpRequest::newHttpRequest();
                connectReq->setMethod(drogon::Get);
                connectReq->setPath("/tiles");
                connectReq->addHeader("X-USER-ROLE", "Tropico-Viewer");

                std::promise<drogon::ReqResult> connectPromise;
                auto connectFuture = connectPromise.get_future();
                wsClient->connectToServer(
                    connectReq,
                    [&connectPromise](
                        drogon::ReqResult result,
                        const drogon::HttpResponsePtr&,
                        const drogon::WebSocketClientPtr&) { connectPromise.set_value(result); });

                REQUIRE(connectFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
                REQUIRE(connectFuture.get() == drogon::ReqResult::Ok);

                auto conn = wsClient->getConnection();
                if (!conn || !conn->connected()) {
                    wsClient->stop();
                    FAIL("WebSocket connection not established");
                }

                // Invalid JSON: should yield a Status message but keep the socket open.
                {
                    conn->send("{not json", drogon::WebSocketMessageType::Text);
                    std::unique_lock lock(mutex);
                    REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
                        return !error.empty() ||
                               (lastStatus.has_value() && lastStatus->value("allDone", false));
                    }));
                    if (!error.empty()) {
                        wsClient->stop();
                        FAIL(error);
                    }
                    REQUIRE(lastStatus->value("message", "").find("Invalid JSON") != std::string::npos);
                    REQUIRE(conn->connected());
                }

                // Valid request should succeed afterwards.
                {
                    {
                        std::lock_guard lock(mutex);
                        lastStatus.reset();
                    }
                    receivedTileCount.store(0, std::memory_order_relaxed);

                    auto req = nlohmann::json::object({
                        {"requests", nlohmann::json::array({nlohmann::json::object({
                            {"mapId", "Tropico"},
                            {"layerId", "WayLayer"},
                            {"tileIds", nlohmann::json::array({1234})},
                        })})},
                    }).dump();

                    conn->send(req, drogon::WebSocketMessageType::Text);

                    std::unique_lock lock(mutex);
                    REQUIRE(cv.wait_for(lock, std::chrono::seconds(10), [&] {
                        return !error.empty() ||
                               (lastStatus.has_value() && lastStatus->value("allDone", false));
                    }));
                    if (!error.empty()) {
                        wsClient->stop();
                        FAIL(error);
                    }

                    REQUIRE(receivedTileCount.load(std::memory_order_relaxed) == 1);
                    REQUIRE(lastStatus->contains("requests"));
                    REQUIRE((*lastStatus)["requests"].size() == 1);
                    REQUIRE((*lastStatus)["requests"][0]["status"].get<int>() ==
                            static_cast<int>(RequestStatus::Success));
                }

                wsClient->stop();
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

    auto tempDir = fs::temp_directory_path() / test::generateTimestampedDirectoryName("mapget_test_http_config");
    fs::create_directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";

    // Set up the config file.
    DataSourceConfigService::get().reset();
    struct SchemaPatchGuard {
        ~SchemaPatchGuard() { DataSourceConfigService::get().setDataSourceConfigSchemaPatch(nlohmann::json::object()); }
    } schemaPatchGuard;

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

    SECTION("Get Configuration - Config File Not Found")
    {
        DataSourceConfigService::get().loadConfig(tempConfigPath.string());
        auto [result, res] = cli.get("/config");
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(res != nullptr);
        REQUIRE(res->statusCode() == drogon::k404NotFound);
        REQUIRE(std::string(res->body()) == "The server does not have a config file.");
    }

    // Create config file for tests that need it
    {
        std::ofstream configFile(tempConfigPath);
        configFile << "sources: []\nhttp-settings: [{'password': 'hunter2'}]";
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
    }

    DataSourceConfigService::get().loadConfig(tempConfigPath.string());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    SECTION("Get Configuration - Not allowed")
    {
        setGetConfigEndpointEnabled(false);
        auto [result, res] = cli.get("/config");
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(res != nullptr);
        REQUIRE(res->statusCode() == drogon::k403Forbidden);
    }

    SECTION("Get Configuration - No Config File Path Set")
    {
        setGetConfigEndpointEnabled(true);
        DataSourceConfigService::get().loadConfig("");
        auto [result, res] = cli.get("/config");
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(res != nullptr);
        REQUIRE(res->statusCode() == drogon::k404NotFound);
        REQUIRE(std::string(res->body()) ==
                "The config file path is not set. Check the server configuration.");
    }

    SECTION("Get Configuration - Success")
    {
        setGetConfigEndpointEnabled(true);
        auto [result, res] = cli.get("/config");
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(res != nullptr);
        REQUIRE(res->statusCode() == drogon::k200OK);

        auto body = std::string(res->body());
        REQUIRE(body.find("sources") != std::string::npos);
        REQUIRE(body.find("http-settings") != std::string::npos);
        REQUIRE(body.find("hunter2") == std::string::npos);
        REQUIRE(
            body.find("MASKED:0:f52fbd32b2b3b86ff88ef6c490628285f482af15ddcb29541f94bcf526a3f6c7") !=
            std::string::npos);
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

    SECTION("Post Configuration - Valid JSON Config")
    {
        setPostConfigEndpointEnabled(true);
        std::string newConfig = R"({
            "sources": [{"type": "TestDataSource"}],
            "http-settings": [{"scope": "https://example.com", "password": "MASKED:0:f52fbd32b2b3b86ff88ef6c490628285f482af15ddcb29541f94bcf526a3f6c7"}]
        })";

        auto [result, res] = cli.postJson("/config", newConfig);
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(res != nullptr);
        REQUIRE(res->statusCode() == drogon::k200OK);
        REQUIRE(std::string(res->body()) == "Configuration updated and applied successfully.");

        std::ifstream config(*mapget::DataSourceConfigService::get().getConfigFilePath());
        std::stringstream configContentStream;
        configContentStream << config.rdbuf();
        auto configContent = configContentStream.str();
        REQUIRE(configContent.find("hunter2") != std::string::npos);
    }

    fs::remove(tempConfigPath);
}
