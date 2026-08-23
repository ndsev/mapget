#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
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
constexpr int32_t kRelationSourceTileIdValue =
    kSecondHttpTileIdValue;
constexpr int32_t kRelationTargetTileIdValue =
    kThirdHttpTileIdValue;

class CountingRemoteDataSource final
    : public RemoteDataSource
{
public:
    using RemoteDataSource::RemoteDataSource;

    TileLayer::Ptr get(
        MapTileKey const& key,
        Cache::Ptr& cache,
        DataSourceInfo const& info,
        TileLayer::LoadStateCallback callback = {})
        override
    {
        {
            std::lock_guard lock(mutex_);
            ++getCalls_[key];
        }
        return RemoteDataSource::get(
            key,
            cache,
            info,
            std::move(callback));
    }

    [[nodiscard]] size_t getCalls(
        MapTileKey const& key) const
    {
        std::lock_guard lock(mutex_);
        auto found = getCalls_.find(key);
        return found == getCalls_.end()
            ? 0
            : found->second;
    }

private:
    mutable std::mutex mutex_;
    std::map<MapTileKey, size_t> getCalls_;
};

/** Produces empty finite-lifetime tiles for interactive handoff tests. */
class ExpiringInteractiveDataSource final : public DataSource
{
public:
    /** Construct the isolated synthetic map/layer metadata. */
    ExpiringInteractiveDataSource()
        : info_(DataSourceInfo::fromJson(nlohmann::json::parse(R"({
            "stringPoolId": "expiring-interactive-pool",
            "mapId": "ExpiringInteractiveMap",
            "maxParallelJobs": 1,
            "layers": {
                "ExpiringLayer": {
                    "type": "Features",
                    "featureTypes": []
                }
            }
        })")))
    {
    }

    /** Return the synthetic datasource metadata. */
    DataSourceInfo info() override { return info_; }

    /** Stamp one tile with a short positive semantic lifetime. */
    void fill(TileFeatureLayer::Ptr const& tile) override
    {
        ++fillCount_;
        tile->setTimestamp(std::chrono::system_clock::now());
        tile->setTtl(std::chrono::seconds(4));
    }

    /** Reject unsupported source-data requests. */
    void fill(TileSourceDataLayer::Ptr const&) override
    {
        throw std::runtime_error("ExpiringInteractiveDataSource has no source-data layer");
    }

    /** Return the number of actual datasource refreshes. */
    [[nodiscard]] size_t fillCount() const { return fillCount_; }

private:
    DataSourceInfo info_;
    std::atomic_size_t fillCount_ = 0;
};

/** Blocks individual tile fills so active interactive ownership can be reconciled deterministically. */
class BlockingInteractiveDataSource final : public DataSource
{
public:
    /** Construct isolated metadata with enough source concurrency for overlap tests. */
    explicit BlockingInteractiveDataSource(
        std::string mapId = "BlockingInteractiveMap")
        : info_(DataSourceInfo::fromJson(nlohmann::json{
              {"stringPoolId", mapId + "-pool"},
              {"mapId", mapId},
              {"maxParallelJobs", 3},
              {"layers",
               {{"BlockingLayer",
                 {
                     {"type", "Features"},
                     {"featureTypes", nlohmann::json::array()},
                 }}}},
          }))
    {
    }

    /** Return the synthetic datasource metadata. */
    DataSourceInfo info() override { return info_; }

    /** Hold one source call until the test releases all active fills. */
    void fill(TileFeatureLayer::Ptr const& tile) override
    {
        std::unique_lock lock(mutex_);
        ++fillCounts_[tile->tileId()];
        started_.insert(tile->tileId());
        stateChanged_.notify_all();
        stateChanged_.wait(lock, [this] { return released_; });
    }

    /** Reject unsupported source-data requests. */
    void fill(TileSourceDataLayer::Ptr const&) override
    {
        throw std::runtime_error("BlockingInteractiveDataSource has no source-data layer");
    }

    /** Wait until every requested tile has entered its datasource call. */
    [[nodiscard]] bool waitForStarted(
        std::set<TileId> const& tileIds,
        std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return stateChanged_.wait_for(
            lock,
            timeout,
            [&]
            {
                return std::ranges::all_of(
                    tileIds,
                    [&](TileId tileId) { return started_.contains(tileId); });
            });
    }

    /** Return how often one tile reached the datasource. */
    [[nodiscard]] size_t fillCount(TileId tileId) const
    {
        std::lock_guard lock(mutex_);
        auto const found = fillCounts_.find(tileId);
        return found == fillCounts_.end() ? 0 : found->second;
    }

    /** Release every currently blocked datasource call. */
    void releaseAll()
    {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        stateChanged_.notify_all();
    }

private:
    DataSourceInfo info_;
    mutable std::mutex mutex_;
    std::condition_variable stateChanged_;
    std::map<TileId, size_t> fillCounts_;
    std::set<TileId> started_;
    bool released_ = false;
};

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

    /** Sends one registered-field JSON PATCH with optional optimistic-concurrency headers. */
    std::pair<drogon::ReqResult, drogon::HttpResponsePtr> patchJson(
        std::string path,
        std::string body,
        std::vector<std::pair<std::string, std::string>> headers = {})
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Patch);
        req->setPath(std::move(path));
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        for (auto const& [key, value] : headers) {
            req->addHeader(key, value);
        }
        req->setBody(std::move(body));
        return client_->sendRequest(req);
    }

    /** Sends one plain-text PUT request to exercise writable static mounts. */
    std::pair<drogon::ReqResult, drogon::HttpResponsePtr> putText(
        std::string path,
        std::string body)
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Put);
        req->setPath(std::move(path));
        req->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        req->setBody(std::move(body));
        return client_->sendRequest(req);
    }

    /** Sends one JSON PUT with optional optimistic-concurrency headers. */
    std::pair<drogon::ReqResult, drogon::HttpResponsePtr> putJson(
        std::string path,
        std::string body,
        std::vector<std::pair<std::string, std::string>> headers = {})
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Put);
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
    WsTilesClient(
        uint16_t port,
        std::shared_ptr<LayerInfo> layerInfo,
        bool requireFeatureLayer = true,
        std::string pullPath = "/interactive/payload")
        : pullClient_("127.0.0.1", port),
          layerInfo_(std::move(layerInfo)),
          requireFeatureLayer_(requireFeatureLayer),
          pullPath_(std::move(pullPath)),
          reader_(
              [this](auto&&, auto&&) { return layerInfo_; },
              [this](auto&& tile) {
                  if (requireFeatureLayer_ && tile->id().layer_ != LayerType::Features) {
                      setError("Unexpected tile layer type");
                      return;
                  }
                  receivedTileCount_.fetch_add(1, std::memory_order_relaxed);
                  cv_.notify_all();
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
            auto allDone = false;
            {
                std::lock_guard lock(mutex_);
                if (!error_.empty()) {
                    return true;
                }
                allDone = lastStatus_.has_value() && lastStatus_->value("allDone", false);
            }

            const auto remainingMs = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
            if (remainingMs <= 0) {
                break;
            }

            const auto clientId = clientId_.load(std::memory_order_relaxed);
            if (clientId > 0) {
                // The WS status can arrive before the long-poll payload has
                // been drained. After allDone, wait for one empty payload poll
                // so tile-count assertions do not race the binary stream.
                const auto waitCapMs = allDone ? 100 : 1000;
                const auto waitMs = std::clamp<int64_t>(remainingMs, 1, waitCapMs);
                const auto [result, resp] = pullClient_.get(fmt::format(
                    "{}?clientId={}&waitMs={}&maxBytes={}",
                    pullPath_,
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
                    if (allDone) {
                        return true;
                    }
                    continue;
                }
                else if (resp->statusCode() == drogon::k410Gone) {
                    setError("Tiles pull session closed");
                    return true;
                }
                else {
                    setError(fmt::format("Unexpected {} response status: {}", pullPath_, static_cast<int>(resp->statusCode())));
                    return true;
                }

                continue;
            }

            if (allDone) {
                return true;
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

    /** Drain payloads until at least the requested number of tile frames arrives. */
    [[nodiscard]] bool waitForTileCount(
        int expected,
        std::chrono::milliseconds timeout)
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (receivedTileCount() >= expected || !error().empty()) {
                return receivedTileCount() >= expected;
            }
            auto const clientId = clientId_.load(std::memory_order_relaxed);
            if (clientId <= 0) {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(20));
                continue;
            }
            auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            auto const waitMs = std::clamp<int64_t>(remaining.count(), 1, 100);
            auto const [result, resp] = pullClient_.get(fmt::format(
                "{}?clientId={}&waitMs={}&maxBytes={}",
                pullPath_,
                clientId,
                waitMs,
                64 * 1024 * 1024));
            if (result != drogon::ReqResult::Ok || !resp) {
                setError("Failed to pull tile frame");
                return false;
            }
            if (resp->statusCode() == drogon::k200OK) {
                try {
                    std::lock_guard readerLock(readerMutex_);
                    reader_.read(std::string(resp->body()));
                }
                catch (std::exception const& e) {
                    setError(std::string("Failed to parse tile stream: ") + e.what());
                    return false;
                }
            }
            else if (resp->statusCode() != drogon::k204NoContent) {
                setError(fmt::format(
                    "Unexpected payload response status: {}",
                    static_cast<int>(resp->statusCode())));
                return false;
            }
        }
        return receivedTileCount() >= expected;
    }

    [[nodiscard]] bool waitForStatus(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(
            lock,
            timeout,
            [this] { return !error_.empty() || lastStatus_.has_value(); });
    }

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
    std::string pullPath_;
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
        "stringPoolId": "test-datasource",
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

    // Fetch a separately transferred tile attachment.
    {
        auto [result, resp] = dsClient.get(
            fmt::format(
                "/attachment?layer=WayLayer&tileId={}&name=ways.glb",
                kHttpTileIdValue));
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(resp != nullptr);
        REQUIRE(resp->statusCode() == drogon::k200OK);
        REQUIRE(resp->contentTypeString() == "model/gltf-binary");
        REQUIRE(resp->getHeader("ETag") == "\"ways-v1\"");
        REQUIRE(std::string(resp->body()) == "glTF");

        auto [notModifiedResult, notModified] =
            dsClient.get(
                fmt::format(
                    "/attachment?layer=WayLayer&tileId={}&name=ways.glb",
                    kHttpTileIdValue),
                {{"If-None-Match", "\"ways-v1\""}});
        REQUIRE(
            notModifiedResult ==
            drogon::ReqResult::Ok);
        REQUIRE(notModified != nullptr);
        REQUIRE(
            notModified->statusCode() ==
            drogon::k304NotModified);
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

        LocateCandidate responseParsed(nlohmann::json::parse(std::string(resp->body()))[0]);
        REQUIRE(responseParsed.tileKey_.mapId_ == "Tropico");
        REQUIRE(responseParsed.tileKey_.layer_ == LayerType::Features);
        REQUIRE(responseParsed.tileKey_.layerId_ == "WayLayer");
        REQUIRE(responseParsed.tileKey_.tileId_.value() == kHttpTileIdValue);
        REQUIRE(
            responseParsed.selector_.canonicalFeatureId_ ==
            std::optional<std::string>{
                "Way.Area42.0"});
    }

    // Query mapget HTTP service (in-process, started once for entire test binary)
    {
        auto& service = test::httpService();
        auto remoteDataSource =
            std::make_shared<
                CountingRemoteDataSource>(
                "127.0.0.1",
                dsProc.port());
        service.add(remoteDataSource);

        // The MapTileStream attachment endpoint validates the name against
        // the normally cached source tile and forwards remote datasource
        // bytes without embedding them in that tile.
        {
            SyncHttpClient serviceClient(
                "127.0.0.1",
                service.port());
            auto path = fmt::format(
                "/attachment?mapId=Tropico&layerId=WayLayer&tileId={}&name=ways.glb",
                kHttpTileIdValue);
            auto [result, resp] =
                serviceClient.get(path);
            REQUIRE(
                result ==
                drogon::ReqResult::Ok);
            REQUIRE(resp != nullptr);
            REQUIRE(
                resp->statusCode() ==
                drogon::k200OK);
            REQUIRE(
                resp->contentTypeString() ==
                "model/gltf-binary");
            REQUIRE(
                std::string(resp->body()) ==
                "glTF");
            auto etag =
                resp->getHeader("ETag");
            REQUIRE(etag == "\"ways-v1\"");

            auto [notModifiedResult,
                  notModified] =
                serviceClient.get(
                    path,
                    {{"If-None-Match",
                      etag}});
            REQUIRE(
                notModifiedResult ==
                drogon::ReqResult::Ok);
            REQUIRE(notModified != nullptr);
            REQUIRE(
                notModified->statusCode() ==
                drogon::k304NotModified);

            HttpClient mapgetClient(
                "127.0.0.1",
                service.port(),
                {},
                false);
            auto attachment =
                mapgetClient.attachment({
                    .tileKey_ = MapTileKey(
                        LayerType::Features,
                        "Tropico",
                        "WayLayer",
                        TileId::fromValue(
                            kHttpTileIdValue)),
                    .name_ = "ways.glb",
                });
            REQUIRE(attachment);
            REQUIRE(attachment->bytes_);
            REQUIRE(
                std::string(
                    attachment->bytes_->begin(),
                    attachment->bytes_->end()) ==
                "glTF");
        }

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

            auto [statusResult, statusResp] = serviceClient.get("/status-data");
            REQUIRE(statusResult == drogon::ReqResult::Ok);
            REQUIRE(statusResp != nullptr);
            REQUIRE(statusResp->statusCode() == drogon::k200OK);
            auto const status = nlohmann::json::parse(std::string(statusResp->body()));
            REQUIRE_FALSE(status["service"].contains("cached-feature-tree-bytes"));
            REQUIRE_FALSE(status["service"].contains("cached-feature-tile-size-distribution"));
            REQUIRE_FALSE(status["memory"].contains("unattributed-resident-bytes"));
            auto const& reconciliation = status["memory"]["reconciliation"];
            REQUIRE(reconciliation["measurement"] == "diagnostic-residuals");
            REQUIRE(
                reconciliation["known-ownership-bytes"] ==
                status["memory"]["known-current-bytes"]);
            auto const& trim = status["memory"]["allocator-trim"];
            REQUIRE(trim.contains("attempts"));
            REQUIRE(trim.contains("successful-trims"));
            REQUIRE(trim.contains("last-duration-microseconds"));
            REQUIRE(trim.contains("last-free-arena-before-bytes"));
            REQUIRE(trim.contains("last-free-arena-after-bytes"));
#if defined(__linux__) && defined(__GLIBC__)
            REQUIRE(trim["supported"] == true);
            REQUIRE(trim["enabled"] == true);
            REQUIRE(trim["period-seconds"] == 10);
#else
            REQUIRE(trim["supported"] == false);
            REQUIRE(trim["enabled"] == false);
            REQUIRE(trim["period-seconds"] == 0);
#endif

            auto [pageResult, pageResp] = serviceClient.get("/status");
            REQUIRE(pageResult == drogon::ReqResult::Ok);
            REQUIRE(pageResp != nullptr);
            REQUIRE(pageResp->statusCode() == drogon::k200OK);
            auto const page = std::string(pageResp->body());
            REQUIRE(page.find("Cache Report") != std::string::npos);
            REQUIRE(page.find("/status-data/cache-report") != std::string::npos);
            REQUIRE(page.find("includeTileSizeDistribution") == std::string::npos);
            REQUIRE(page.find("color-scheme: light") != std::string::npos);
            REQUIRE(page.find("color-scheme: dark") != std::string::npos);
            REQUIRE(page.find("id=\"themeSelect\"") != std::string::npos);
            REQUIRE(page.find("brand-mark") == std::string::npos);
            REQUIRE(page.find("info-bubble") != std::string::npos);
            REQUIRE(page.find("Unattributed process RSS") == std::string::npos);

            auto [reportResult, reportResp] = serviceClient.postJson(
                "/status-data/cache-report",
                "{}");
            REQUIRE(reportResult == drogon::ReqResult::Ok);
            REQUIRE(reportResp != nullptr);
            REQUIRE(reportResp->statusCode() == drogon::k200OK);
            auto const report = nlohmann::json::parse(std::string(reportResp->body()));
            REQUIRE(report.contains("generatedAtMs"));
            REQUIRE(report.contains("durationMs"));
            REQUIRE(report["featureTree"].is_object());
            REQUIRE(report["tileSizeDistribution"].is_object());
            REQUIRE(report["featureTree"]["tile-count"].get<uint64_t>() > 0);
        }

        // A remote relation target is planned by /locate and materialized only
        // through the ordinary service tile path. Neither the datasource
        // server nor RemoteDataSource reconstructs the target for location.
        {
            auto const sourceTile =
                TileId::fromValue(
                    kRelationSourceTileIdValue);
            auto const targetTile =
                TileId::fromValue(
                    kRelationTargetTileIdValue);
            auto const sourceKey =
                MapTileKey{
                    LayerType::Features,
                    "Tropico",
                    "WayLayer",
                    sourceTile};
            auto const targetKey =
                MapTileKey{
                    LayerType::Features,
                    "Tropico",
                    "WayLayer",
                    targetTile};
            auto const sourceCallsBefore =
                remoteDataSource->getCalls(
                    sourceKey);
            auto const targetCallsBefore =
                remoteDataSource->getCalls(
                    targetKey);

            auto request =
                std::make_shared<
                    FeatureLayerFilterTilesRequest>(
                    "Tropico",
                    "WayLayer",
                    std::vector<TileId>{
                        sourceTile},
                    FeatureLayerFilterRequest{
                        .filterId_ =
                            "remote-relation",
                        .generation_ = 1,
                        .channels_ = {
                            FeatureLayerFilterChannel{
                                .channelId_ =
                                    "connected",
                                .featureFilter_ =
                                    "typeId == 'Way'",
                                .scope_ =
                                    FeatureLayerFilterScope::
                                        Relation,
                                .featureTypes_ = {
                                    "Way"},
                                .geometryName_ =
                                    "centerline",
                                .relation_ =
                                    FeatureLayerStoredRelationOptions{
                                        .relationNamePattern_ =
                                            "connected",
                                        .recursive_ =
                                            true,
                                    },
                            },
                        },
                    });

            std::vector<
                TileSubsetLayer::Ptr>
                results;
            request->onFilterResult(
                [&](TileSubsetLayer::Ptr layer) {
                    results.push_back(
                        std::move(layer));
                });
            REQUIRE(service.request(request));
            request->wait();

            REQUIRE(
                request->getStatus() ==
                RequestStatus::Success);
            REQUIRE(results.size() == 1);
            REQUIRE(
                results.front()
                    ->at(0)
                    ->relationEntryCount() ==
                1);
            REQUIRE(
                remoteDataSource->getCalls(
                    sourceKey) -
                    sourceCallsBefore ==
                1);
            REQUIRE(
                remoteDataSource->getCalls(
                    targetKey) -
                    targetCallsBefore ==
                1);
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

        // Filter through the dedicated REST endpoint and C++ client helper.
        {
            SyncHttpClient serviceClient("127.0.0.1", service.port());
            auto filterBody = nlohmann::json::object({
                {"channels", nlohmann::json::array({
                    nlohmann::json::object({
                        {"channelId", "ways"},
                        {"scope", "feature"},
                        {"entryFilter", "typeId == 'Way'"},
                        {"featureFields",
                         nlohmann::json::array({"typeId"})},
                        {"featureTypes",
                         nlohmann::json::array({"Way"})},
                    }),
                })},
                {"responseType", "jsonl"},
                {"requests", nlohmann::json::array({nlohmann::json::object({
                    {"mapId", "Tropico"},
                    {"layerId", "WayLayer"},
                    {"tileIds", nlohmann::json::array({kHttpTileIdValue})},
                })})},
            }).dump();

            auto [result, resp] =
                serviceClient.postJson("/filter", filterBody);
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
                if (parsed.value("type", "") != "TileSubsetLayer") {
                    continue;
                }
                sawResultLayer = true;
                REQUIRE(parsed["filterId"] == "");
                REQUIRE(parsed["generation"] == 0);
                REQUIRE(parsed["channels"].size() == 1);
                REQUIRE(
                    parsed["channels"][0]["channelId"] ==
                    "ways");
                REQUIRE(
                    parsed["channels"][0]["featureFields"] ==
                    nlohmann::json::array({"typeId"}));
                REQUIRE(
                    parsed["channels"][0]["featureEntries"]
                        .size() == 1);
                REQUIRE(
                    parsed["channels"][0]["featureEntries"][0]
                          ["values"] ==
                    nlohmann::json::array({"Way"}));
            }
            REQUIRE(sawResultLayer);

            HttpClient client("127.0.0.1", service.port());
            FeatureLayerFilterRequest filter{
                .filterId_ = "client-filter",
                .generation_ = 3,
                .channels_ = {
                    FeatureLayerFilterChannel{
                        .channelId_ = "ways",
                        .entryFilter_ =
                            "typeId == 'Way'",
                        .scope_ =
                            FeatureLayerFilterScope::Feature,
                        .featureTypes_ = {"Way"},
                        .featureFields_ = {"typeId"},
                    },
                },
            };

            auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
                "Tropico",
                "WayLayer",
                std::vector<TileId>{TileId::fromValue(kHttpTileIdValue)},
                std::move(filter));
            size_t resultCount = 0;
            size_t statusCount = 0;
            request->onFilterResult([&](TileSubsetLayer::Ptr layer) {
                REQUIRE(layer->size() == 1);
                resultCount +=
                    layer->at(0)->featureEntryCount();
                REQUIRE(layer->filterId().empty());
                REQUIRE(layer->generation() == 0);
                REQUIRE(
                    layer->at(0)->featureFields() ==
                    std::vector<std::string>{"typeId"});
            });
            request->onStatus([&](nlohmann::json const&) {
                ++statusCount;
            });

            client.filter(request)->wait();
            REQUIRE(request->getStatus() == RequestStatus::Success);
            REQUIRE(resultCount == 1);
            REQUIRE(statusCount > 0);
        }

        // POST /tiles does not accept filter fields.
        {
            SyncHttpClient serviceClient("127.0.0.1", service.port());
            auto [result, resp] = serviceClient.postJson(
                "/tiles",
                nlohmann::json::object({
                    {"channels", nlohmann::json::array({
                        nlohmann::json::object({
                            {"channelId", "ways"},
                        }),
                    })},
                    {"requests", nlohmann::json::array({nlohmann::json::object({
                        {"mapId", "Tropico"},
                        {"layerId", "WayLayer"},
                        {"tileIds", nlohmann::json::array({kHttpTileIdValue})},
                    })})},
                }).dump());

            REQUIRE(result == drogon::ReqResult::Ok);
            REQUIRE(resp != nullptr);
            REQUIRE(resp->statusCode() == drogon::k400BadRequest);
            REQUIRE(
                std::string(resp->body()).find("POST /filter") !=
                std::string::npos);
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

            auto runWsTilesRequestAtPath = [&](
                std::string_view path,
                bool sendAuthHeader,
                const std::string& requestJson,
                std::string pullPath = "/interactive/payload") {
                WsTilesClient wsClient(service.port(), layerInfo, true, std::move(pullPath));

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

                auto [status, wsTileCount] = runWsTilesRequestAtPath("/tiles", true, req, "/tiles/next");
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

                // Ping/Pong connection-health traffic is transport control,
                // not an invalid tile request.
                {
                    wsClient.resetStatus();
                    conn->send("health", drogon::WebSocketMessageType::Pong);
                    REQUIRE_FALSE(
                        wsClient.waitForStatus(std::chrono::milliseconds(200)));
                    REQUIRE(conn->connected());
                }

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

            // Repeating active work is idempotent, and replacing {A, B} with
            // {B, C} preserves B while suppressing A's eventual callback.
            {
                auto source = std::make_shared<BlockingInteractiveDataSource>();
                service.add(source);
                auto blockingLayerInfo = source->info().getLayer("BlockingLayer");
                REQUIRE(blockingLayerInfo);

                auto const tileA = TileId::fromValue(kHttpTileIdValue);
                auto const tileB = TileId::fromValue(kSecondHttpTileIdValue);
                auto const tileC = TileId::fromValue(kThirdHttpTileIdValue);
                WsTilesClient wsClient(service.port(), blockingLayerInfo);
                REQUIRE(wsClient.connect(true));
                auto conn = requireConnected(wsClient);
                auto makeRequest = [](std::initializer_list<int32_t> tileIds) {
                    return nlohmann::json::object({
                        {"requests", nlohmann::json::array({
                            nlohmann::json::object({
                                {"mapId", "BlockingInteractiveMap"},
                                {"layerId", "BlockingLayer"},
                                {"tileIds", tileIds},
                                {"priorityTileIds", tileIds},
                            }),
                        })},
                    }).dump();
                };

                conn->send(
                    makeRequest({kHttpTileIdValue, kSecondHttpTileIdValue}),
                    drogon::WebSocketMessageType::Text);
                REQUIRE(source->waitForStarted({tileA, tileB}, std::chrono::seconds(5)));

                wsClient.resetStatus();
                conn->send(
                    makeRequest({kHttpTileIdValue, kSecondHttpTileIdValue}),
                    drogon::WebSocketMessageType::Text);
                REQUIRE(wsClient.waitForStatus(std::chrono::seconds(5)));
                REQUIRE(source->fillCount(tileA) == 1);
                REQUIRE(source->fillCount(tileB) == 1);

                wsClient.resetStatus();
                conn->send(
                    makeRequest({kSecondHttpTileIdValue, kThirdHttpTileIdValue}),
                    drogon::WebSocketMessageType::Text);
                REQUIRE(source->waitForStarted({tileC}, std::chrono::seconds(5)));
                source->releaseAll();

                REQUIRE(wsClient.waitForDone(std::chrono::seconds(10)));
                if (!wsClient.error().empty()) {
                    wsClient.stop();
                    FAIL(wsClient.error());
                }
                REQUIRE(wsClient.receivedTileCount() == 2);
                REQUIRE(source->fillCount(tileA) == 1);
                REQUIRE(source->fillCount(tileB) == 1);
                REQUIRE(source->fillCount(tileC) == 1);
                wsClient.stop();
            }

            // Complete snapshots use a latest-wins mailbox. A deliberately
            // expensive candidate keeps reconciliation busy while two small
            // replacements arrive; the final replacement must become active.
            // An intermediate may already have started before its successor
            // reaches the mailbox and is canceled normally in that case.
            {
                constexpr size_t RepeatedTileCount = 250'000;
                auto source = std::make_shared<BlockingInteractiveDataSource>(
                    "CoalescingInteractiveMap");
                service.add(source);
                auto blockingLayerInfo = source->info().getLayer("BlockingLayer");
                REQUIRE(blockingLayerInfo);

                SyncHttpClient statusClient("127.0.0.1", service.port());
                auto supersededSnapshots = [&]() {
                    auto [result, response] = statusClient.get("/status-data");
                    REQUIRE(result == drogon::ReqResult::Ok);
                    REQUIRE(response != nullptr);
                    return nlohmann::json::parse(std::string(response->body()))
                        ["tilesWebsocket"]["superseded-snapshots"]
                            .get<int64_t>();
                };
                auto const supersededBefore = supersededSnapshots();

                WsTilesClient wsClient(service.port(), blockingLayerInfo);
                REQUIRE(wsClient.connect(true));
                auto conn = requireConnected(wsClient);
                auto requestFor = [](int32_t tileId, uint64_t requestId) {
                    return nlohmann::json::object({
                        {"requestId", requestId},
                        {"requests", nlohmann::json::array({
                            nlohmann::json::object({
                                {"mapId", "CoalescingInteractiveMap"},
                                {"layerId", "BlockingLayer"},
                                {"tileIds", nlohmann::json::array({tileId})},
                            }),
                        })},
                    }).dump();
                };

                auto repeatedIds = std::vector<int32_t>(
                    RepeatedTileCount,
                    kHttpTileIdValue);
                conn->send(
                    nlohmann::json::object({
                        {"requestId", 800},
                        {"requests", nlohmann::json::array({
                            nlohmann::json::object({
                                {"mapId", "UnknownCoalescingMap"},
                                {"layerId", "BlockingLayer"},
                                {"tileIds", std::move(repeatedIds)},
                            }),
                        })},
                    }).dump(),
                    drogon::WebSocketMessageType::Text);
                conn->send(
                    requestFor(kSecondHttpTileIdValue, 801),
                    drogon::WebSocketMessageType::Text);
                conn->send(
                    requestFor(kThirdHttpTileIdValue, 802),
                    drogon::WebSocketMessageType::Text);

                auto const finalTile = TileId::fromValue(kThirdHttpTileIdValue);
                REQUIRE(source->waitForStarted({finalTile}, std::chrono::seconds(10)));
                source->releaseAll();

                wsClient.resetStatus();
                REQUIRE(wsClient.waitForDone(std::chrono::seconds(10)));
                REQUIRE(wsClient.error().empty());
                auto const finalStatus = wsClient.lastStatus();
                REQUIRE(finalStatus.has_value());
                REQUIRE(finalStatus->value("requestId", 0) == 802);
                REQUIRE(source->fillCount(finalTile) == 1);
                REQUIRE(supersededSnapshots() > supersededBefore);
                wsClient.stop();
            }

            // Pending filter snapshots suppress overlap while it is active,
            // queued, or represented by a lightweight handoff record.
            {
                WsTilesClient wsClient(
                    service.port(),
                    layerInfo,
                    false);
                REQUIRE(wsClient.connect(true));
                auto conn = requireConnected(wsClient);
                auto makeRequest =
                    [](std::initializer_list<int32_t> tileIds) {
                        return nlohmann::json::object({
                            {"filterId", "coverage-overlap"},
                            {"generation", 1},
                            {"channels", nlohmann::json::array({
                                nlohmann::json::object({
                                    {"channelId", "ways"},
                                    {"scope", "feature"},
                                    {"entryFilter", "typeId == 'Way'"},
                                    {"featureTypes", nlohmann::json::array({"Way"})},
                                }),
                            })},
                            {"requests", nlohmann::json::array({
                                nlohmann::json::object({
                                    {"mapId", "Tropico"},
                                    {"layerId", "WayLayer"},
                                    {"tileIds", tileIds},
                                    {"priorityTileIds", tileIds},
                                }),
                            })},
                        }).dump();
                    };
                auto sendAndDrain =
                    [&](std::initializer_list<int32_t> tileIds) {
                        wsClient.resetStatus();
                        wsClient.resetTileCount();
                        conn->send(
                            makeRequest(tileIds),
                            drogon::WebSocketMessageType::Text);
                        REQUIRE(
                            wsClient.waitForDone(
                                std::chrono::seconds(10)));
                        if (!wsClient.error().empty()) {
                            wsClient.stop();
                            FAIL(wsClient.error());
                        }
                        auto status = wsClient.lastStatus();
                        REQUIRE(status.has_value());
                        REQUIRE((*status)["requests"].size() == 1);
                        REQUIRE(
                            (*status)["requests"][0]["status"].get<int>() ==
                            static_cast<int>(RequestStatus::Success));
                        return wsClient.receivedTileCount();
                    };

                REQUIRE(
                    sendAndDrain({
                        kHttpTileIdValue,
                        kSecondHttpTileIdValue,
                    }) == 2);
                REQUIRE(
                    sendAndDrain({
                        kHttpTileIdValue,
                        kSecondHttpTileIdValue,
                    }) == 0);
                REQUIRE(
                    sendAndDrain({
                        kSecondHttpTileIdValue,
                        kThirdHttpTileIdValue,
                    }) == 1);
                // Omission acknowledges or withdraws the first handoff, so a
                // later ordinary pending snapshot evaluates it again.
                REQUIRE(
                    sendAndDrain({
                        kHttpTileIdValue,
                    }) == 1);
                wsClient.stop();
            }

            // A filter generation is immutable while any of its output keys
            // overlap. Rejecting a semantic mutation must leave the preceding
            // handoff and pending snapshot intact.
            {
                WsTilesClient wsClient(service.port(), layerInfo, false);
                REQUIRE(wsClient.connect(true));
                auto conn = requireConnected(wsClient);
                auto makeRequest = [](std::string entryFilter) {
                    return nlohmann::json::object({
                        {"filterId", "immutable-overlap"},
                        {"generation", 5},
                        {"channels", nlohmann::json::array({
                            nlohmann::json::object({
                                {"channelId", "ways"},
                                {"scope", "feature"},
                                {"entryFilter", std::move(entryFilter)},
                                {"featureTypes", nlohmann::json::array({"Way"})},
                            }),
                        })},
                        {"requests", nlohmann::json::array({
                            nlohmann::json::object({
                                {"mapId", "Tropico"},
                                {"layerId", "WayLayer"},
                                {"tileIds", nlohmann::json::array({kHttpTileIdValue})},
                            }),
                        })},
                    }).dump();
                };

                auto const originalRequest = makeRequest("typeId == 'Way'");
                conn->send(originalRequest, drogon::WebSocketMessageType::Text);
                REQUIRE(wsClient.waitForDone(std::chrono::seconds(10)));
                REQUIRE(wsClient.receivedTileCount() == 1);

                wsClient.resetStatus();
                wsClient.resetTileCount();
                conn->send(
                    makeRequest("typeId != 'Way'"),
                    drogon::WebSocketMessageType::Text);
                REQUIRE(wsClient.waitForStatus(std::chrono::seconds(5)));
                auto status = wsClient.lastStatus();
                REQUIRE(status.has_value());
                REQUIRE(
                    status->value("message", "").find("advance generation") !=
                    std::string::npos);
                REQUIRE(wsClient.receivedTileCount() == 0);

                wsClient.resetStatus();
                conn->send(originalRequest, drogon::WebSocketMessageType::Text);
                REQUIRE(wsClient.waitForDone(std::chrono::seconds(5)));
                REQUIRE(wsClient.receivedTileCount() == 0);
                wsClient.stop();
            }

            // Indexed chunks form one atomic pending snapshot. Applying the
            // first chunk early would clear A's handoff and recompute it when
            // the final chunk adds A back.
            {
                WsTilesClient wsClient(service.port(), layerInfo);
                REQUIRE(wsClient.connect(true));
                auto conn = requireConnected(wsClient);
                auto requestFor = [](int32_t tileId) {
                    return nlohmann::json::object({
                        {"mapId", "Tropico"},
                        {"layerId", "WayLayer"},
                        {"tileIds", nlohmann::json::array({tileId})},
                    });
                };

                conn->send(
                    nlohmann::json::object({
                        {"requests", nlohmann::json::array({
                            requestFor(kHttpTileIdValue),
                        })},
                    }).dump(),
                    drogon::WebSocketMessageType::Text);
                REQUIRE(wsClient.waitForDone(std::chrono::seconds(10)));
                REQUIRE(wsClient.receivedTileCount() == 1);

                wsClient.resetStatus();
                wsClient.resetTileCount();
                conn->send(
                    nlohmann::json::object({
                        {"requestId", 700},
                        {"chunk", {{"index", 0}, {"isLast", false}}},
                        {"requests", nlohmann::json::array({
                            requestFor(kSecondHttpTileIdValue),
                        })},
                    }).dump(),
                    drogon::WebSocketMessageType::Text);
                REQUIRE_FALSE(
                    wsClient.waitForStatus(std::chrono::milliseconds(200)));

                conn->send(
                    nlohmann::json::object({
                        {"requestId", 700},
                        {"chunk", {{"index", 1}, {"isLast", true}}},
                        {"requests", nlohmann::json::array({
                            requestFor(kHttpTileIdValue),
                        })},
                    }).dump(),
                    drogon::WebSocketMessageType::Text);
                REQUIRE(wsClient.waitForDone(std::chrono::seconds(10)));
                REQUIRE(wsClient.receivedTileCount() == 1);
                wsClient.stop();
            }

            // A handoff uses the produced value's absolute semantic expiry.
            // Repeating the same pending key after expiry follows the normal
            // cache path without an omission or delivery attempt number.
            {
                auto source = std::make_shared<ExpiringInteractiveDataSource>();
                service.add(source);
                auto expiringLayerInfo = source->info().getLayer("ExpiringLayer");
                REQUIRE(expiringLayerInfo);

                WsTilesClient wsClient(service.port(), expiringLayerInfo);
                REQUIRE(wsClient.connect(true));
                auto conn = requireConnected(wsClient);
                auto const request = nlohmann::json::object({
                    {"requests", nlohmann::json::array({
                        nlohmann::json::object({
                            {"mapId", "ExpiringInteractiveMap"},
                            {"layerId", "ExpiringLayer"},
                            {"tileIds", nlohmann::json::array({kHttpTileIdValue})},
                        }),
                    })},
                }).dump();
                auto sendAndDrain = [&]() {
                    wsClient.resetStatus();
                    wsClient.resetTileCount();
                    conn->send(request, drogon::WebSocketMessageType::Text);
                    REQUIRE(wsClient.waitForDone(std::chrono::seconds(10)));
                    return wsClient.receivedTileCount();
                };

                wsClient.resetStatus();
                wsClient.resetTileCount();
                conn->send(request, drogon::WebSocketMessageType::Text);
                REQUIRE(wsClient.waitForTileCount(1, std::chrono::seconds(10)));
                REQUIRE(source->fillCount() == 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                REQUIRE(sendAndDrain() == 0);
                REQUIRE(source->fillCount() == 1);
                // The repeated snapshot above must not move the original
                // four-second absolute handoff deadline forward.
                std::this_thread::sleep_for(std::chrono::milliseconds(2700));
                REQUIRE(sendAndDrain() == 1);
                REQUIRE(source->fillCount() == 2);
                wsClient.stop();
            }

            // Protocol 4 rejects the removed delivery-attempt operation and
            // field instead of silently accepting stale renewal semantics.
            {
                WsTilesClient wsClient(
                    service.port(),
                    layerInfo,
                    false);
                REQUIRE(wsClient.connect(true));
                auto conn = requireConnected(wsClient);
                auto const channels = nlohmann::json::array({
                    nlohmann::json::object({
                        {"channelId", "ways"},
                        {"scope", "feature"},
                        {"entryFilter", "typeId == 'Way'"},
                        {"featureTypes", nlohmann::json::array({"Way"})},
                    }),
                });
                auto const legacyRequest = nlohmann::json::object({
                    {"mapId", "Tropico"},
                    {"layerId", "WayLayer"},
                    {"tileIds", nlohmann::json::array({kHttpTileIdValue})},
                    {"filterId", "removed-renewal"},
                    {"generation", 7},
                    {"deliveryEpoch", 2},
                    {"channels", channels},
                });
                conn->send(
                    nlohmann::json::object({
                        {"renewals", nlohmann::json::array({legacyRequest})},
                    }).dump(),
                    drogon::WebSocketMessageType::Text);
                REQUIRE(wsClient.waitForStatus(std::chrono::seconds(5)));
                auto status = wsClient.lastStatus();
                REQUIRE(status.has_value());
                REQUIRE(status->value("message", "").find("renewals") != std::string::npos);

                wsClient.resetStatus();
                conn->send(
                    nlohmann::json::object({
                        {"requests", nlohmann::json::array({legacyRequest})},
                    }).dump(),
                    drogon::WebSocketMessageType::Text);
                REQUIRE(wsClient.waitForStatus(std::chrono::seconds(5)));
                status = wsClient.lastStatus();
                REQUIRE(status.has_value());
                REQUIRE(
                    status->value("message", "").find("deliveryEpoch") !=
                    std::string::npos);
                REQUIRE(wsClient.receivedTileCount() == 0);
                wsClient.stop();
            }

            // WebSocket tiles: staged bucket requests are rejected after the protocol-3 cutover.
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
                REQUIRE(wsTileCount == 0);
                REQUIRE(status["requests"].empty());
                REQUIRE(status["message"].get<std::string>().find("tileIdsByNextStage") !=
                        std::string::npos);
            }

            // Reset authorization is layered, and the next ordinary request
            // must miss the cleared service cache and reach the datasource.
            {
                SyncHttpClient resetClient(
                    "127.0.0.1",
                    service.port());
                auto const resetTile =
                    TileId::fromValue(131079);
                auto const resetKey = MapTileKey(
                    LayerType::Features,
                    "Tropico",
                    "WayLayer",
                    resetTile);
                auto loadResetTile = [&] {
                    auto [request, receivedTileCount] =
                        countReceivedTiles(
                            goodClient,
                            "Tropico",
                            "WayLayer",
                            std::vector<TileId>{resetTile});
                    REQUIRE(
                        request->getStatus() ==
                        RequestStatus::Success);
                    REQUIRE(receivedTileCount == 1);
                };

                auto const callsBefore =
                    remoteDataSource->getCalls(resetKey);
                loadResetTile();
                auto const callsAfterWarm =
                    remoteDataSource->getCalls(resetKey);
                REQUIRE(callsAfterWarm == callsBefore + 1);
                loadResetTile();
                REQUIRE(
                    remoteDataSource->getCalls(resetKey) ==
                    callsAfterWarm);

                auto [missingGateResult, missingGate] =
                    resetClient.postJson(
                        "/cache/reset",
                        R"({"mapId":"Tropico"})",
                        {{"X-USER-ROLE", "Tropico-Viewer"}});
                REQUIRE(missingGateResult == drogon::ReqResult::Ok);
                REQUIRE(missingGate->statusCode() == drogon::k403Forbidden);

                auto [wrongGateResult, wrongGate] =
                    resetClient.postJson(
                        "/cache/reset",
                        R"({"mapId":"Tropico"})",
                        {
                            {"X-CACHE-ROLE", "resetter-extra"},
                            {"X-USER-ROLE", "Tropico-Viewer"},
                        });
                REQUIRE(wrongGateResult == drogon::ReqResult::Ok);
                REQUIRE(wrongGate->statusCode() == drogon::k403Forbidden);

                auto [missingMapAuthResult, missingMapAuth] =
                    resetClient.postJson(
                        "/cache/reset",
                        R"({"mapId":"Tropico"})",
                        {{"X-CACHE-ROLE", "resetter"}});
                REQUIRE(missingMapAuthResult == drogon::ReqResult::Ok);
                REQUIRE(missingMapAuth->statusCode() == drogon::k404NotFound);

                auto const resetHeaders =
                    std::vector<std::pair<std::string, std::string>>{
                        {"X-CACHE-ROLE", "resetter"},
                        {"X-USER-ROLE", "Tropico-Viewer"},
                    };
                auto [malformedResult, malformed] =
                    resetClient.postJson(
                        "/cache/reset",
                        R"({"mapId":42})",
                        resetHeaders);
                REQUIRE(malformedResult == drogon::ReqResult::Ok);
                REQUIRE(malformed->statusCode() == drogon::k400BadRequest);

                auto [unknownResult, unknown] =
                    resetClient.postJson(
                        "/cache/reset",
                        R"({"mapId":"UnknownMap"})",
                        resetHeaders);
                REQUIRE(unknownResult == drogon::ReqResult::Ok);
                REQUIRE(unknown->statusCode() == drogon::k404NotFound);

                auto [resetResult, resetResponse] =
                    resetClient.postJson(
                        "/cache/reset",
                        R"({"mapId":"Tropico"})",
                        resetHeaders);
                REQUIRE(resetResult == drogon::ReqResult::Ok);
                REQUIRE(resetResponse->statusCode() == drogon::k204NoContent);

                loadResetTile();
                REQUIRE(
                    remoteDataSource->getCalls(resetKey) ==
                    callsAfterWarm + 1);

                auto [alternativeResult, alternativeResponse] =
                    resetClient.postJson(
                        "/cache/reset",
                        R"({"mapId":"Tropico"})",
                        {
                            {"x-cache-group", "operators"},
                            {"x-user-role", "Tropico-Viewer"},
                        });
                REQUIRE(alternativeResult == drogon::ReqResult::Ok);
                REQUIRE(alternativeResponse->statusCode() == drogon::k204NoContent);
            }
        }

        service.remove(remoteDataSource);
    }
}

TEST_CASE("Cache reset configuration fails closed", "[Configuration][Cache]")
{
    HttpServiceConfig enabledWithoutGate;
    enabledWithoutGate.cacheResetEnabled = true;

    REQUIRE_THROWS_AS(
        HttpService(
            std::make_shared<MemCache>(),
            enabledWithoutGate),
        std::invalid_argument);

    HttpServiceConfig disabledByDefault;
    REQUIRE_NOTHROW(
        HttpService(
            std::make_shared<MemCache>(),
            disabledByDefault));
}

TEST_CASE("Runtime static mounts can opt into writes", "[StaticMount]")
{
    auto& service = test::httpService();
    REQUIRE(service.isRunning() == true);

    auto tempDir = fs::current_path() / test::generateTimestampedDirectoryName("mapget_test_static_mount");
    auto stylesDir = tempDir / "styles";
    fs::create_directories(stylesDir);
    auto writableFile = stylesDir / "writable.yaml";
    auto readOnlyFile = tempDir / "readonly.yaml";
    {
        std::ofstream(writableFile) << "before";
        std::ofstream(readOnlyFile) << "unchanged";
    }

    auto const readOnlyPrefix = "/test-static";
    auto const writablePrefix = readOnlyPrefix + std::string("/styles");
    struct MountGuard {
        std::string writablePrefix;
        std::string readOnlyPrefix;
        fs::path tempDir;

        /** Removes process-global test mounts and their temporary files. */
        ~MountGuard()
        {
            removeStaticMount(writablePrefix);
            removeStaticMount(readOnlyPrefix);
            fs::remove_all(tempDir);
        }
    } guard{writablePrefix, readOnlyPrefix, tempDir};
    REQUIRE(ensureStaticMount(readOnlyPrefix, tempDir));
    REQUIRE(ensureStaticMount(writablePrefix, stylesDir, StaticMountAccess::ReadWrite));

    SyncHttpClient client("127.0.0.1", service.port());
    auto [writeResult, writeResponse] = client.putText(
        writablePrefix + std::string("/writable.yaml"),
        "after");
    REQUIRE(writeResult == drogon::ReqResult::Ok);
    REQUIRE(writeResponse != nullptr);
    REQUIRE(writeResponse->statusCode() == drogon::k200OK);

    std::ifstream updatedFile(writableFile);
    REQUIRE(std::string(std::istreambuf_iterator<char>(updatedFile), {}) == "after");

    auto [getResult, getResponse] = client.get(writablePrefix + std::string("/writable.yaml"));
    REQUIRE(getResult == drogon::ReqResult::Ok);
    REQUIRE(getResponse != nullptr);
    REQUIRE(getResponse->body() == "after");
    REQUIRE(getResponse->getHeader("Cache-Control") == "no-store");

    auto [readOnlyResult, readOnlyResponse] = client.putText(
        readOnlyPrefix + std::string("/readonly.yaml"),
        "overwritten");
    REQUIRE(readOnlyResult == drogon::ReqResult::Ok);
    REQUIRE(readOnlyResponse != nullptr);
    REQUIRE(readOnlyResponse->statusCode() == drogon::k403Forbidden);

    std::ifstream unchangedFile(readOnlyFile);
    REQUIRE(std::string(std::istreambuf_iterator<char>(unchangedFile), {}) == "unchanged");
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

    constexpr std::string_view publicFieldPath = "/extension/catalog";
    DataSourceConfigService::get().registerPublicConfigSection(
        "publicConfig",
        [](YAML::Node const& fullConfig) -> nlohmann::json {
            auto section = fullConfig["publicConfig"];
            if (!section || !section.IsMap() || section.size() == 0) {
                return nlohmann::json::object();
            }
            return yamlToJson(section, false);
        });
    DataSourceConfigService::get().registerPublicConfigSection(
        "capabilities",
        [path = std::string{publicFieldPath}](YAML::Node const& fullConfig) -> nlohmann::json {
            YAML::Node section{YAML::NodeType::Undefined};
            if (fullConfig.IsMap()) {
                for (auto const& entry : fullConfig) {
                    if (entry.first.IsScalar() && entry.first.Scalar() == "extensionConfig") {
                        section = entry.second;
                        break;
                    }
                }
            }
            YAML::Node catalog{YAML::NodeType::Undefined};
            if (section.IsMap()) {
                for (auto const& entry : section) {
                    if (entry.first.IsScalar() && entry.first.Scalar() == "catalog") {
                        catalog = entry.second;
                        break;
                    }
                }
            }
            auto& configService = DataSourceConfigService::get();
            return {{"configField", {
                {"configured", catalog.IsDefined()},
                {"valid", !catalog.IsDefined() || catalog.IsSequence()},
                {"write", isPostConfigEndpointEnabled()
                    && configService.hasPublicConfigFieldWriter(path)},
                {"endpoint", "/config"},
                {"method", "PATCH"},
                {"path", path},
                {"revision", DataSourceConfigService::get().getConfigFileRevision().value_or("")}
            }}};
        });
    DataSourceConfigService::get().registerPublicConfigFieldWriter(
        std::string{publicFieldPath},
        [](YAML::Node& fullConfig, nlohmann::json const& requested) {
            if (!requested.is_array()) {
                return DataSourceConfigService::PublicConfigWriteResult{
                    .error = "catalog must be an array"};
            }
            fullConfig["extensionConfig"]["catalog"] = jsonToYaml(requested);
            return DataSourceConfigService::PublicConfigWriteResult{
                .canonicalValue = requested};
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

    auto getConfigPayload = [&](std::vector<std::pair<std::string, std::string>> headers = {}) {
        auto [result, res] = cli.get("/config", std::move(headers));
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(res != nullptr);
        REQUIRE(res->statusCode() == drogon::k200OK);
        REQUIRE(res->getHeader("Cache-Control") == "private, no-store");
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
        "  catalog: []\n");
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

    SECTION("Get Configuration - Cache reset capability is caller specific")
    {
        auto unavailable = getConfigPayload();
        REQUIRE(unavailable["capabilities"]["cacheReset"] == false);

        auto available = getConfigPayload({
            {"X-CACHE-ROLE", "resetter"},
        });
        REQUIRE(available["capabilities"]["cacheReset"] == true);
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
        REQUIRE(getConfigPayload()["capabilities"]["configField"]["write"] == false);
        auto [result, res] = cli.postJson("/config", "");
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(res != nullptr);
        REQUIRE(res->statusCode() == drogon::k403Forbidden);

        auto [patchResult, patchResponse] = cli.patchJson(
            "/config",
            nlohmann::json{{"path", publicFieldPath}, {"value", nlohmann::json::array()}}.dump(),
            {{"If-Match", "unused"}});
        REQUIRE(patchResult == drogon::ReqResult::Ok);
        REQUIRE(patchResponse->statusCode() == drogon::k403Forbidden);
    }

    SECTION("Registered-field PATCH requires a current revision and preserves config state")
    {
        setPostConfigEndpointEnabled(true);
        auto payload = getConfigPayload();
        auto const capability = payload["capabilities"]["configField"];
        REQUIRE(capability["configured"] == false);
        REQUIRE(capability["valid"] == true);
        REQUIRE(capability["write"] == true);
        auto const revision = capability["revision"].get<std::string>();
        REQUIRE_FALSE(revision.empty());

        auto const emptyPatch = nlohmann::json{
            {"path", publicFieldPath},
            {"value", nlohmann::json::array()}};
        auto [missingResult, missing] = cli.patchJson("/config", emptyPatch.dump());
        REQUIRE(missingResult == drogon::ReqResult::Ok);
        REQUIRE(missing->statusCode() == drogon::k428PreconditionRequired);

        auto [staleResult, stale] = cli.patchJson(
            "/config",
            emptyPatch.dump(),
            {{"If-Match", "stale"}});
        REQUIRE(staleResult == drogon::ReqResult::Ok);
        REQUIRE(stale->statusCode() == drogon::k412PreconditionFailed);

        std::atomic_size_t datasourceNotifications{0};
        auto subscription = DataSourceConfigService::get().subscribe(
            [&](auto const&) { ++datasourceNotifications; });
        auto const notificationsBeforeWrite = datasourceNotifications.load();

#ifndef _WIN32
        fs::permissions(tempConfigPath, fs::perms::owner_read | fs::perms::owner_write);
#endif
        nlohmann::json catalog = nlohmann::json::array({{{
            "id", "network"},
            {"name", "Network"},
            {"enabled", true},
            {"layerPresets", nlohmann::json::array({{{
                "layerId", "Lane"},
                {"styleId", "Lanes"},
                {"presetId", "topology"}}})}}});
        auto [writeResult, written] = cli.patchJson(
            "/config",
            nlohmann::json{{"path", publicFieldPath}, {"value", catalog}}.dump(),
            {{"If-Match", "\"" + revision + "\""}});
        REQUIRE(writeResult == drogon::ReqResult::Ok);
        REQUIRE(written->statusCode() == drogon::k200OK);
        auto response = nlohmann::json::parse(std::string(written->body()));
        REQUIRE(response["path"].get<std::string>() == publicFieldPath);
        REQUIRE(response["value"] == catalog);
        REQUIRE(response["revision"].get<std::string>() != revision);
        REQUIRE(written->getHeader("ETag") == "\"" + response["revision"].get<std::string>() + "\"");

        std::this_thread::sleep_for(std::chrono::milliseconds(750));
        REQUIRE(datasourceNotifications == notificationsBeforeWrite);
        auto stored = YAML::LoadFile(tempConfigPath.string());
        REQUIRE(yamlToJson(stored["extensionConfig"]["catalog"], false) == catalog);
        REQUIRE(stored["publicConfig"]["featureFlag"].as<bool>());
        REQUIRE(stored["http-settings"][0]["password"].as<std::string>() == "hunter2");
#ifndef _WIN32
        REQUIRE(
            (fs::status(tempConfigPath).permissions() & fs::perms::owner_all)
            == (fs::perms::owner_read | fs::perms::owner_write));
#endif
    }

    SECTION("Registered-field PATCH rejects malformed and unregistered requests")
    {
        setPostConfigEndpointEnabled(true);
        auto const revision = DataSourceConfigService::get().getConfigFileRevision().value();
        std::ifstream original(tempConfigPath, std::ios::binary);
        std::string const originalContents(
            std::istreambuf_iterator<char>(original), {});

        auto [invalidResult, invalid] = cli.patchJson(
            "/config",
            nlohmann::json{{"path", publicFieldPath}}.dump(),
            {{"If-Match", revision}});
        REQUIRE(invalidResult == drogon::ReqResult::Ok);
        REQUIRE(invalid->statusCode() == drogon::k400BadRequest);

        auto [unknownResult, unknown] = cli.patchJson(
            "/config",
            nlohmann::json{{"path", "/unregistered/value"}, {"value", 1}}.dump(),
            {{"If-Match", revision}});
        REQUIRE(unknownResult == drogon::ReqResult::Ok);
        REQUIRE(unknown->statusCode() == drogon::k400BadRequest);

        std::ifstream unchanged(tempConfigPath, std::ios::binary);
        REQUIRE(std::string(std::istreambuf_iterator<char>(unchanged), {}) == originalContents);
    }

    SECTION("Registered-field PATCH restores original bytes after canonical read-back failure")
    {
        setPostConfigEndpointEnabled(true);
        auto const revision = DataSourceConfigService::get().getConfigFileRevision().value();
        std::ifstream original(tempConfigPath, std::ios::binary);
        std::string const originalContents(
            std::istreambuf_iterator<char>(original), {});
#ifndef _WIN32
        auto const originalPermissions = fs::status(tempConfigPath).permissions();
#endif
        auto calls = std::make_shared<size_t>(0);
        DataSourceConfigService::get().registerPublicConfigFieldWriter(
            std::string{publicFieldPath},
            [calls](YAML::Node& fullConfig, nlohmann::json const& requested) {
                if ((*calls)++ > 0) {
                    return DataSourceConfigService::PublicConfigWriteResult{
                        .error = "intentional read-back failure"};
                }
                fullConfig["extensionConfig"]["catalog"] = jsonToYaml(requested);
                return DataSourceConfigService::PublicConfigWriteResult{
                    .canonicalValue = requested};
            });

        auto [result, response] = cli.patchJson(
            "/config",
            nlohmann::json{
                {"path", publicFieldPath},
                {"value", nlohmann::json::array({"will-roll-back"})}}.dump(),
            {{"If-Match", revision}});
        REQUIRE(result == drogon::ReqResult::Ok);
        REQUIRE(response->statusCode() == drogon::k500InternalServerError);

        std::ifstream restored(tempConfigPath, std::ios::binary);
        REQUIRE(std::string(std::istreambuf_iterator<char>(restored), {}) == originalContents);
#ifndef _WIN32
        REQUIRE(fs::status(tempConfigPath).permissions() == originalPermissions);
#endif
        REQUIRE(DataSourceConfigService::get().getConfigFileRevision() == revision);
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

    SECTION("Post Configuration - Valid JSON Config preserves registered public sections")
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
        REQUIRE(configContent.find("publicConfig") != std::string::npos);
        REQUIRE(configContent.find("featureFlag") != std::string::npos);
        auto stored = YAML::Load(configContent);
        REQUIRE(yamlToJson(stored["publicConfig"]["catalog"], false)
            == nlohmann::json::array());
    }

    DataSourceConfigService::get().end();
    fs::remove_all(tempDir);
}
