#include "http-service.h"

#include "cli.h"
#include "mapget/log.h"
#include "mapget/service/config.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "nlohmann/json-schema.hpp"
#include "nlohmann/json.hpp"
#include "yaml-cpp/yaml.h"

#include <zlib.h>

#ifdef __linux__
#include <malloc.h>
#endif

namespace mapget
{

namespace
{

/**
 * Simple gzip compressor for streaming compression.
 */
class GzipCompressor
{
public:
    GzipCompressor()
    {
        strm_.zalloc = Z_NULL;
        strm_.zfree = Z_NULL;
        strm_.opaque = Z_NULL;
        // 16+MAX_WBITS enables gzip format (not just deflate)
        int ret = deflateInit2(
            &strm_, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        if (ret != Z_OK) {
            throw std::runtime_error("Failed to initialize gzip compressor");
        }
    }

    ~GzipCompressor() { deflateEnd(&strm_); }

    GzipCompressor(GzipCompressor const&) = delete;
    GzipCompressor(GzipCompressor&&) = delete;

    std::string compress(const char* data, size_t size, int flush_mode = Z_NO_FLUSH)
    {
        std::string result;
        if (size == 0 && flush_mode == Z_NO_FLUSH) {
            return result;
        }

        strm_.avail_in = static_cast<uInt>(size);
        strm_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));

        char outbuf[8192];
        do {
            strm_.avail_out = sizeof(outbuf);
            strm_.next_out = reinterpret_cast<Bytef*>(outbuf);

            int ret = deflate(&strm_, flush_mode);
            if (ret == Z_STREAM_ERROR) {
                throw std::runtime_error("Gzip compression failed");
            }

            size_t have = sizeof(outbuf) - strm_.avail_out;
            result.append(outbuf, have);
        } while (strm_.avail_out == 0);

        return result;
    }

    std::string finish() { return compress(nullptr, 0, Z_FINISH); }

private:
    z_stream strm_{};
};

[[nodiscard]] AuthHeaders authHeadersFromRequest(const drogon::HttpRequestPtr& req)
{
    AuthHeaders headers;
    for (auto const& [k, v] : req->headers()) {
        headers.emplace(k, v);
    }
    return headers;
}

[[nodiscard]] bool containsGzip(std::string_view acceptEncoding)
{
    return !acceptEncoding.empty() && acceptEncoding.find("gzip") != std::string_view::npos;
}

}  // namespace

struct HttpService::Impl
{
    HttpService& self_;
    HttpServiceConfig config_;
    mutable std::atomic<uint64_t> binaryRequestCounter_{0};
    mutable std::atomic<uint64_t> jsonRequestCounter_{0};

    explicit Impl(HttpService& self, const HttpServiceConfig& config) : self_(self), config_(config) {}

    enum class ResponseType { Binary, Json };

    void tryMemoryTrim(ResponseType responseType) const
    {
        uint64_t interval =
            (responseType == ResponseType::Binary) ? config_.memoryTrimIntervalBinary : config_.memoryTrimIntervalJson;

        if (interval == 0) {
            return;
        }

        auto& counter = (responseType == ResponseType::Binary) ? binaryRequestCounter_ : jsonRequestCounter_;
        auto count = counter.fetch_add(1, std::memory_order_relaxed);
        if ((count % interval) != 0) {
            return;
        }

#ifdef __linux__
#ifndef NDEBUG
        const char* typeStr = (responseType == ResponseType::Binary) ? "binary" : "JSON";
        log().debug("Trimming memory after {} {} requests (interval: {})", count, typeStr, interval);
#endif
        malloc_trim(0);
#endif
    }

    struct TilesStreamState : std::enable_shared_from_this<TilesStreamState>
    {
        static constexpr auto binaryMimeType = "application/binary";
        static constexpr auto jsonlMimeType = "application/jsonl";
        static constexpr auto anyMimeType = "*/*";

        explicit TilesStreamState(Impl const& impl, trantor::EventLoop* loop) : impl_(impl), loop_(loop)
        {
            static std::atomic_uint64_t nextRequestId;
            requestId_ = nextRequestId++;
            writer_ = std::make_unique<TileLayerStream::Writer>(
                [this](auto&& msg, auto&& /*msgType*/) { appendOutgoingUnlocked(msg); }, stringOffsets_);
        }

        void attachStream(drogon::ResponseStreamPtr stream)
        {
            {
                std::lock_guard lock(mutex_);
                if (aborted_ || responseEnded_) {
                    if (stream)
                        stream->close();
                    return;
                }
                stream_ = std::move(stream);
            }
            scheduleDrain();
        }

        void parseRequestFromJson(nlohmann::json const& requestJson)
        {
            std::string mapId = requestJson["mapId"];
            std::string layerId = requestJson["layerId"];
            std::vector<TileId> tileIds;
            tileIds.reserve(requestJson["tileIds"].size());
            for (auto const& tid : requestJson["tileIds"].get<std::vector<uint64_t>>()) {
                tileIds.emplace_back(tid);
            }
            requests_.push_back(std::make_shared<LayerTilesRequest>(mapId, layerId, std::move(tileIds)));
        }

        [[nodiscard]] bool setResponseTypeFromAccept(std::string_view acceptHeader, std::string& error)
        {
            responseType_ = std::string(acceptHeader);
            if (responseType_.empty())
                responseType_ = anyMimeType;
            if (responseType_ == anyMimeType)
                responseType_ = binaryMimeType;

            if (responseType_ == binaryMimeType) {
                trimResponseType_ = ResponseType::Binary;
                return true;
            }
            if (responseType_ == jsonlMimeType) {
                trimResponseType_ = ResponseType::Json;
                return true;
            }

            error = "Unknown Accept header value: " + responseType_;
            return false;
        }

        void enableGzip() { compressor_ = std::make_unique<GzipCompressor>(); }

        void onAborted()
        {
            if (aborted_.exchange(true))
                return;
            for (auto const& req : requests_) {
                if (!req->isDone()) {
                    impl_.self_.abort(req);
                }
            }
            drogon::ResponseStreamPtr stream;
            {
                std::lock_guard lock(mutex_);
                if (responseEnded_.exchange(true))
                    return;
                stream = std::move(stream_);
            }
            if (stream)
                stream->close();
        }

        void addResult(TileLayer::Ptr const& result)
        {
            {
                std::lock_guard lock(mutex_);
                if (aborted_)
                    return;

                log().debug("Response ready: {}", MapTileKey(*result).toString());
                if (responseType_ == binaryMimeType) {
                    writer_->write(result);
                } else {
                    auto dumped = result->toJson().dump(
                        -1, ' ', false, nlohmann::json::error_handler_t::ignore);
                    appendOutgoingUnlocked(dumped);
                    appendOutgoingUnlocked("\n");
                }
            }
            scheduleDrain();
        }

        void onRequestDone()
        {
            {
                std::lock_guard lock(mutex_);
                if (aborted_)
                    return;

                bool allDoneNow = std::all_of(
                    requests_.begin(), requests_.end(), [](auto const& r) { return r->isDone(); });

                if (allDoneNow && !allDone_) {
                    allDone_ = true;
                    if (responseType_ == binaryMimeType && !endOfStreamSent_) {
                        writer_->sendEndOfStream();
                        endOfStreamSent_ = true;
                    }
                }
            }
            scheduleDrain();
        }

        void scheduleDrain()
        {
            if (aborted_ || responseEnded_)
                return;
            if (drainScheduled_.exchange(true))
                return;

            auto weak = weak_from_this();
            loop_->queueInLoop([weak = std::move(weak)]() mutable {
                if (auto self = weak.lock()) {
                    self->drainOnLoop();
                }
            });
        }

	        void drainOnLoop()
	        {
	            drainScheduled_ = false;
	            if (aborted_ || responseEnded_)
	                return;

	            constexpr size_t maxChunk = 64 * 1024;

	            for (;;) {
	                std::string chunk;
	                bool done = false;
	                bool needAbort = false;
	                bool scheduleAgain = false;
	                drogon::ResponseStreamPtr streamToClose;
	                {
	                    std::lock_guard lock(mutex_);
	                    if (!stream_)
	                        return;

                    if (!pending_.empty()) {
                        size_t n = std::min(pending_.size(), maxChunk);
                        chunk.assign(pending_.data(), n);
                        pending_.erase(0, n);
                    } else {
                        if (allDone_ && compressor_ && !compressionFinished_) {
                            pending_.append(compressor_->finish());
                            compressionFinished_ = true;
                            continue;
                        }
                        done = allDone_;
                    }

	                    if (!chunk.empty()) {
	                        if (!stream_->send(chunk)) {
	                            needAbort = true;
	                        } else if (!pending_.empty() || allDone_) {
	                            // Keep draining until we sent everything and closed the stream.
	                            scheduleAgain = true;
	                        }
	                    } else if (done) {
	                        responseEnded_ = true;
	                        streamToClose = std::move(stream_);
	                    }
	                }

                if (needAbort) {
                    onAborted();
                    return;
                }

	                if (done) {
	                    if (streamToClose)
	                        streamToClose->close();
	                    impl_.tryMemoryTrim(trimResponseType_);
	                    return;
	                }
	                if (scheduleAgain)
	                    scheduleDrain();
	                return;
	            }
	        }

        void appendOutgoingUnlocked(std::string_view bytes)
        {
            if (bytes.empty())
                return;

            if (compressor_) {
                pending_.append(compressor_->compress(bytes.data(), bytes.size()));
            } else {
                pending_.append(bytes);
            }
        }

        Impl const& impl_;
        trantor::EventLoop* loop_;

        std::mutex mutex_;
        uint64_t requestId_ = 0;

        std::string responseType_;
        ResponseType trimResponseType_ = ResponseType::Binary;

        std::string pending_;
        drogon::ResponseStreamPtr stream_;
        std::unique_ptr<TileLayerStream::Writer> writer_;
        std::vector<LayerTilesRequest::Ptr> requests_;
        TileLayerStream::StringPoolOffsetMap stringOffsets_;

        std::unique_ptr<GzipCompressor> compressor_;
        bool compressionFinished_ = false;
        bool endOfStreamSent_ = false;
        bool allDone_ = false;

        std::atomic_bool aborted_{false};
        std::atomic_bool drainScheduled_{false};
        std::atomic_bool responseEnded_{false};
    };

    mutable std::mutex clientRequestMapMutex_;
    mutable std::unordered_map<std::string, std::shared_ptr<TilesStreamState>> requestStatePerClientId_;

    void abortRequestsForClientId(
        std::string const& clientId,
        std::shared_ptr<TilesStreamState> newState = nullptr) const
    {
        std::unique_lock clientRequestMapAccess(clientRequestMapMutex_);
        auto clientRequestIt = requestStatePerClientId_.find(clientId);
        if (clientRequestIt != requestStatePerClientId_.end()) {
            bool anySoftAbort = false;
            for (auto const& req : clientRequestIt->second->requests_) {
                if (!req->isDone()) {
                    self_.abort(req);
                    anySoftAbort = true;
                }
            }
            if (anySoftAbort)
                log().warn("Soft-aborting tiles request {}", clientRequestIt->second->requestId_);
            requestStatePerClientId_.erase(clientRequestIt);
        }
        if (newState) {
            requestStatePerClientId_.emplace(clientId, std::move(newState));
        }
    }

    void handleTilesRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
    {
        auto state = std::make_shared<TilesStreamState>(*this, drogon::app().getLoop());

        const std::string accept = req->getHeader("accept");
        const std::string acceptEncoding = req->getHeader("accept-encoding");
        auto clientHeaders = authHeadersFromRequest(req);

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(std::string(req->body()));
        }
        catch (const std::exception& e) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k400BadRequest);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody(std::string("Invalid JSON: ") + e.what());
            callback(resp);
            return;
        }

        auto requestsIt = j.find("requests");
        if (requestsIt == j.end() || !requestsIt->is_array()) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k400BadRequest);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody("Missing or invalid 'requests' array");
            callback(resp);
            return;
        }

        log().info("Processing tiles request {}", state->requestId_);
        for (auto& requestJson : *requestsIt) {
            state->parseRequestFromJson(requestJson);
        }

        if (j.contains("stringPoolOffsets")) {
            for (auto& item : j["stringPoolOffsets"].items()) {
                state->stringOffsets_[item.key()] = item.value().get<simfil::StringId>();
            }
        }

        std::string acceptError;
        if (!state->setResponseTypeFromAccept(accept, acceptError)) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k400BadRequest);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody(std::move(acceptError));
            callback(resp);
            return;
        }

        const bool gzip = containsGzip(acceptEncoding);
        if (gzip) {
            state->enableGzip();
        }

        for (auto& request : state->requests_) {
            request->onFeatureLayer([state](auto&& layer) { state->addResult(layer); });
            request->onSourceDataLayer([state](auto&& layer) { state->addResult(layer); });
            request->onDone_ = [state](RequestStatus) { state->onRequestDone(); };
        }

        const auto canProcess = self_.request(state->requests_, clientHeaders);
        if (!canProcess) {
            std::vector<std::underlying_type_t<RequestStatus>> requestStatuses{};
            bool anyUnauthorized = false;
            for (auto const& r : state->requests_) {
                auto status = r->getStatus();
                requestStatuses.emplace_back(static_cast<std::underlying_type_t<RequestStatus>>(status));
                anyUnauthorized |= (status == RequestStatus::Unauthorized);
            }

            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(anyUnauthorized ? drogon::k403Forbidden : drogon::k400BadRequest);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(nlohmann::json::object({{"status", requestStatuses}}).dump());
            callback(resp);
            return;
        }

        if (j.contains("clientId")) {
            abortRequestsForClientId(j["clientId"].get<std::string>(), state);
        }

        auto resp = drogon::HttpResponse::newAsyncStreamResponse(
            [state](drogon::ResponseStreamPtr stream) { state->attachStream(std::move(stream)); },
            true);
        resp->setStatusCode(drogon::k200OK);
        resp->setContentTypeString(state->responseType_);
        if (gzip) {
            resp->addHeader("Content-Encoding", "gzip");
        }
        callback(resp);
    }

    void handleAbortRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
    {
        try {
            auto j = nlohmann::json::parse(std::string(req->body()));
            if (j.contains("clientId")) {
                abortRequestsForClientId(j["clientId"].get<std::string>());
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                resp->setBody("OK");
                callback(resp);
                return;
            }

            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k400BadRequest);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody("Missing clientId");
            callback(resp);
        }
        catch (const std::exception& e) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k400BadRequest);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody(std::string("Invalid JSON: ") + e.what());
            callback(resp);
        }
    }

    void handleSourcesRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
    {
        auto sourcesInfo = nlohmann::json::array();
        for (auto& source : self_.info(authHeadersFromRequest(req))) {
            sourcesInfo.push_back(source.toJson());
        }

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(sourcesInfo.dump());
        callback(resp);
    }

    void handleStatusRequest(
        const drogon::HttpRequestPtr&,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
    {
        auto serviceStats = self_.getStatistics();
        auto cacheStats = self_.cache()->getStatistics();

        std::ostringstream oss;
        oss << "<html><body>";
        oss << "<h1>Status Information</h1>";
        oss << "<h2>Service Statistics</h2>";
        oss << "<pre>" << serviceStats.dump(4) << "</pre>";
        oss << "<h2>Cache Statistics</h2>";
        oss << "<pre>" << cacheStats.dump(4) << "</pre>";
        oss << "</body></html>";

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setContentTypeCode(drogon::CT_TEXT_HTML);
        resp->setBody(oss.str());
        callback(resp);
    }

    void handleLocateRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
    {
        try {
            nlohmann::json j = nlohmann::json::parse(std::string(req->body()));
            auto requestsJson = j["requests"];
            auto allResponsesJson = nlohmann::json::array();

            for (auto const& locateReqJson : requestsJson) {
                LocateRequest locateReq{locateReqJson};
                auto responsesJson = nlohmann::json::array();
                for (auto const& resp : self_.locate(locateReq))
                    responsesJson.emplace_back(resp.serialize());
                allResponsesJson.emplace_back(responsesJson);
            }

            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(nlohmann::json::object({{"responses", allResponsesJson}}).dump());
            callback(resp);
        }
        catch (const std::exception& e) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k400BadRequest);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody(std::string("Invalid JSON: ") + e.what());
            callback(resp);
        }
    }

    static drogon::HttpResponsePtr openConfigFile(std::ifstream& configFile)
    {
        auto configFilePath = DataSourceConfigService::get().getConfigFilePath();
        if (!configFilePath.has_value()) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k404NotFound);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody("The config file path is not set. Check the server configuration.");
            return resp;
        }

        std::filesystem::path path = *configFilePath;
        if (!std::filesystem::exists(path)) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k404NotFound);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody("The server does not have a config file.");
            return resp;
        }

        configFile.open(*configFilePath);
        if (!configFile) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k500InternalServerError);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody("Failed to open config file.");
            return resp;
        }

        return nullptr;
    }

    static void handleGetConfigRequest(
        const drogon::HttpRequestPtr&,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback)
    {
        if (!isGetConfigEndpointEnabled()) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k403Forbidden);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody("The GET /config endpoint is disabled by the server administrator.");
            callback(resp);
            return;
        }

        std::ifstream configFile;
        if (auto errorResp = openConfigFile(configFile)) {
            callback(errorResp);
            return;
        }

        nlohmann::json jsonSchema = DataSourceConfigService::get().getDataSourceConfigSchema();

        try {
            YAML::Node configYaml = YAML::Load(configFile);
            nlohmann::json jsonConfig;
            std::unordered_map<std::string, std::string> maskedSecretMap;
            for (const auto& key : DataSourceConfigService::get().topLevelDataSourceConfigKeys()) {
                if (auto configYamlEntry = configYaml[key])
                    jsonConfig[key] = yamlToJson(configYaml[key], true, &maskedSecretMap);
            }

            nlohmann::json combinedJson;
            combinedJson["schema"] = jsonSchema;
            combinedJson["model"] = jsonConfig;
            combinedJson["readOnly"] = !isPostConfigEndpointEnabled();

            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(combinedJson.dump(2));
            callback(resp);
        }
        catch (const std::exception& e) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k500InternalServerError);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody(std::string("Error processing config file: ") + e.what());
            callback(resp);
        }
    }

    void handlePostConfigRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
    {
        if (!isPostConfigEndpointEnabled()) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k403Forbidden);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody("The POST /config endpoint is not enabled by the server administrator.");
            callback(resp);
            return;
        }

        struct ConfigUpdateState : std::enable_shared_from_this<ConfigUpdateState>
        {
            trantor::EventLoop* loop = nullptr;
            std::atomic_bool done{false};
            std::atomic_bool wroteConfig{false};
            std::unique_ptr<DataSourceConfigService::Subscription> subscription;
            std::function<void(const drogon::HttpResponsePtr&)> callback;
        };

        std::ifstream configFile;
        if (auto errorResp = openConfigFile(configFile)) {
            callback(errorResp);
            return;
        }

        nlohmann::json jsonConfig;
        try {
            jsonConfig = nlohmann::json::parse(std::string(req->body()));
        }
        catch (const nlohmann::json::parse_error& e) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k400BadRequest);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody(std::string("Invalid JSON format: ") + e.what());
            callback(resp);
            return;
        }

        try {
            DataSourceConfigService::get().validateDataSourceConfig(jsonConfig);
        }
        catch (const std::exception& e) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k500InternalServerError);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody(std::string("Validation failed: ") + e.what());
            callback(resp);
            return;
        }

        auto yamlConfig = YAML::Load(configFile);
        std::unordered_map<std::string, std::string> maskedSecrets;
        yamlToJson(yamlConfig, true, &maskedSecrets);

        for (auto const& key : DataSourceConfigService::get().topLevelDataSourceConfigKeys()) {
            if (jsonConfig.contains(key))
                yamlConfig[key] = jsonToYaml(jsonConfig[key], maskedSecrets);
        }

        auto state = std::make_shared<ConfigUpdateState>();
        state->loop = drogon::app().getLoop();
        state->callback = std::move(callback);

        // Subscribe before writing; ignore any callbacks that happen before we write.
        state->subscription = DataSourceConfigService::get().subscribe(
            [state](std::vector<YAML::Node> const&) mutable {
                if (!state->wroteConfig) {
                    return;
                }
                if (state->done.exchange(true))
                    return;
                state->loop->queueInLoop([state]() mutable {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k200OK);
                    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                    resp->setBody("Configuration updated and applied successfully.");
                    state->callback(resp);
                    state->subscription.reset();
                });
            },
            [state](std::string const& error) mutable {
                if (!state->wroteConfig) {
                    return;
                }
                if (state->done.exchange(true))
                    return;
                state->loop->queueInLoop([state, error]() mutable {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k500InternalServerError);
                    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                    resp->setBody(std::string("Error applying the configuration: ") + error);
                    state->callback(resp);
                    state->subscription.reset();
                });
            });

        configFile.close();
        log().trace("Writing new config.");
        state->wroteConfig = true;
        if (auto configFilePath = DataSourceConfigService::get().getConfigFilePath()) {
            std::ofstream newConfigFile(*configFilePath);
            newConfigFile << yamlConfig;
            newConfigFile.close();
        }

        // Timeout fail-safe (rare endpoint; ok to spawn a thread).
        std::thread([weak = state->weak_from_this()]() {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            if (auto state = weak.lock()) {
                if (state->done.exchange(true))
                    return;
                state->loop->queueInLoop([state]() mutable {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k500InternalServerError);
                    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                    resp->setBody("Timeout while waiting for config to update.");
                    state->callback(resp);
                    state->subscription.reset();
                });
            }
        }).detach();
    }
};

HttpService::HttpService(Cache::Ptr cache, const HttpServiceConfig& config)
    : Service(std::move(cache), config.watchConfig, config.defaultTtl), impl_(std::make_unique<Impl>(*this, config))
{
}

HttpService::~HttpService() = default;

void HttpService::setup(drogon::HttpAppFramework& app)
{
    app.registerHandler(
        "/tiles",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            impl_->handleTilesRequest(req, std::move(callback));
        },
        {drogon::Post});

    app.registerHandler(
        "/abort",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            impl_->handleAbortRequest(req, std::move(callback));
        },
        {drogon::Post});

    app.registerHandler(
        "/sources",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            impl_->handleSourcesRequest(req, std::move(callback));
        },
        {drogon::Get});

    app.registerHandler(
        "/status",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            impl_->handleStatusRequest(req, std::move(callback));
        },
        {drogon::Get});

    app.registerHandler(
        "/locate",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            impl_->handleLocateRequest(req, std::move(callback));
        },
        {drogon::Post});

    app.registerHandler(
        "/config",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            if (req->method() == drogon::Get) {
                Impl::handleGetConfigRequest(req, std::move(callback));
                return;
            }
            if (req->method() == drogon::Post) {
                impl_->handlePostConfigRequest(req, std::move(callback));
                return;
            }

            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k405MethodNotAllowed);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody("Method not allowed");
            callback(resp);
        },
        {drogon::Get, drogon::Post});
}

}  // namespace mapget
