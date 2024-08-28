#include "http-service.h"
#include "mapget/log.h"
#include "mapget/service/config.h"

#include "cli.h"
#include "httplib.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <condition_variable>
#include <mutex>

namespace mapget
{

struct HttpService::Impl
{
    HttpService& self_;

    explicit Impl(HttpService& self) : self_(self) {}

    // Use a shared buffer for the responses and a mutex for thread safety.
    struct HttpTilesRequestState
    {
        static constexpr auto binaryMimeType = "application/binary";
        static constexpr auto jsonlMimeType = "application/jsonl";
        static constexpr auto anyMimeType = "*/*";

        std::mutex mutex_;
        std::condition_variable resultEvent_;

        std::stringstream buffer_;
        std::string responseType_;
        std::unique_ptr<TileLayerStream::Writer> writer_;
        std::vector<LayerTilesRequest::Ptr> requests_;
        TileLayerStream::StringPoolOffsetMap stringOffsets_;

        HttpTilesRequestState() {
            writer_ = std::make_unique<TileLayerStream::Writer>(
                [&, this](auto&& msg, auto&& msgType) { buffer_ << msg; },
                stringOffsets_);
        }

        void parseRequestFromJson(nlohmann::json const& requestJson)
        {
            std::string mapId = requestJson["mapId"];
            std::string layerId = requestJson["layerId"];
            std::vector<TileId> tileIds;
            tileIds.reserve(requestJson["tileIds"].size());
            for (auto const& tid : requestJson["tileIds"].get<std::vector<uint64_t>>())
                tileIds.emplace_back(tid);
            requests_.push_back(
                std::make_shared<LayerTilesRequest>(mapId, layerId, std::move(tileIds)));
        }

        void setResponseType(std::string const& s)
        {
            responseType_ = s;
            if (responseType_ == HttpTilesRequestState::binaryMimeType)
                return;
            if (responseType_ == HttpTilesRequestState::jsonlMimeType)
                return;
            if (responseType_ == HttpTilesRequestState::anyMimeType) {
                responseType_ = binaryMimeType;
                return;
            }
            raise(fmt::format("Unknown Accept-Header value {}", responseType_));
        }

        void addResult(TileLayer::Ptr const& result)
        {
            std::unique_lock lock(mutex_);
            log().debug("Response ready: {}", MapTileKey(*result).toString());
            if (responseType_ == binaryMimeType) {
                // Binary response
                writer_->write(result);
            }
            else {
                // JSON response
                buffer_ << nlohmann::to_string(result->toJson())+"\n";
            }
            resultEvent_.notify_one();
        }
    };

    /**
     * Wraps around the generic mapget service's request() function
     * to include httplib request decoding and response encoding.
     */
    void handleTilesRequest(const httplib::Request& req, httplib::Response& res)
    {
        // Parse the JSON request.
        nlohmann::json j = nlohmann::json::parse(req.body);
        auto requestsJson = j["requests"];

        // TODO: Limit number of requests to avoid DoS to other users.
        // Within one HTTP request, all requested tiles from the same map+layer
        // combination should be in a single LayerTilesRequest.
        auto state = std::make_shared<HttpTilesRequestState>();
        for (auto& requestJson : requestsJson) {
            state->parseRequestFromJson(requestJson);
        }

        // Parse stringPoolOffsets.
        if (j.contains("stringPoolOffsets")) {
            for (auto& item : j["stringPoolOffsets"].items()) {
                state->stringOffsets_[item.key()] = item.value().get<simfil::StringId>();
            }
        }

        // Determine response type.
        state->setResponseType(req.get_header_value("Accept"));

        // Process requests.
        for (auto& request : state->requests_) {
            request->onFeatureLayer([state](auto&& layer)
            {
                state->addResult(layer);
            });
            request->onSourceDataLayer([state](auto&& layer)
            {
                state->addResult(layer);
            });
            request->onDone_ = [state](RequestStatus r)
            {
                state->resultEvent_.notify_one();
            };
        }
        auto canProcess = self_.request(state->requests_);

        if (!canProcess) {
            // Send a status report detailing for each request
            // whether its data source is unavailable or it was aborted.
            res.status = 400;
            std::vector<int> requestStatuses{};
            for (const auto& r : state->requests_) {
                requestStatuses.push_back(r->getStatus());
            }
            res.set_content(
                nlohmann::json::object({{"requestStatuses", requestStatuses}}).dump(),
                "application/json");
            return;
        }

        // For efficiency, set up httplib to stream tile layer responses to client:
        // (1) Lambda continuously supplies response data to httplib's DataSink,
        //     picking up data from state->buffer_ until all tile requests are done.
        //     Then, signal sink->done() to close the stream with a 200 status.
        //     See httplib::write_content_without_length(...) too.
        // (2) Lambda acts as a cleanup routine, triggered by httplib upon request wrap-up.
        //     The success flag indicates if wrap-up was due to sink->done() or external factors
        //     like network errors or request aborts in lengthy tile requests (e.g., map-viewer).
        res.set_content_provider(
            state->responseType_,
            [state](size_t offset, httplib::DataSink& sink)
            {
                std::unique_lock lock(state->mutex_);

                // Wait until there is data to be read.
                std::string strBuf;
                bool allDone = false;
                state->resultEvent_.wait(
                    lock,
                    [&]
                    {
                        allDone = std::all_of(
                            state->requests_.begin(),
                            state->requests_.end(),
                            [](const auto& r) { return r->isDone(); });
                        if (allDone)
                            state->writer_->sendEndOfStream();
                        strBuf = state->buffer_.str();
                        return !strBuf.empty() || allDone;
                    });

                if (!strBuf.empty()) {
                    log().debug("Streaming {} bytes...", strBuf.size());
                    sink.write(strBuf.data(), strBuf.size());
                    sink.os.flush();
                    state->buffer_.str("");  // Clear buffer after reading.
                }

                // Call sink.done() when all requests are done.
                if (allDone) {
                    sink.done();
                }

                return true;
            },
            // Network error/timeout of request to datasource:
            // cleanup callback to abort the requests.
            [state, this](bool success)
            {
                log().debug("Request finished, success: {}", success);
                if (!success) {
                    for (auto& request : state->requests_) {
                        self_.abort(request);
                    }
                }
            });
    }

    void handleSourcesRequest(const httplib::Request&, httplib::Response& res)
    {
        auto sourcesInfo = nlohmann::json::array();
        for (auto& source : self_.info()) {
            sourcesInfo.push_back(source.toJson());
        }
        res.set_content(sourcesInfo.dump(), "application/json");
    }

    void handleStatusRequest(const httplib::Request&, httplib::Response& res)
    {
        auto serviceStats = self_.getStatistics();
        auto cacheStats = self_.cache()->getStatistics();

        std::ostringstream oss;
        oss << "<html><body>";
        oss << "<h1>Status Information</h1>";

        // Output serviceStats
        oss << "<h2>Service Statistics</h2>";
        oss << "<pre>" << serviceStats.dump(4) << "</pre>";  // Indentation of 4 for pretty printing

        // Output cacheStats
        oss << "<h2>Cache Statistics</h2>";
        oss << "<pre>" << cacheStats.dump(4) << "</pre>";  // Indentation of 4 for pretty printing

        oss << "</body></html>";
        res.set_content(oss.str(), "text/html");
    }

    void handleLocateRequest(const httplib::Request& req, httplib::Response& res)
    {
        // Parse the JSON request.
        nlohmann::json j = nlohmann::json::parse(req.body);
        auto requestsJson = j["requests"];
        auto allResponsesJson = nlohmann::json::array();

        for (auto const& locateReqJson : requestsJson) {
            LocateRequest locateReq{locateReqJson};
            auto responsesJson = nlohmann::json::array();
            for (auto const& resp : self_.locate(locateReq))
                responsesJson.emplace_back(resp.serialize());
            allResponsesJson.emplace_back(responsesJson);
        }

        res.set_content(
            nlohmann::json::object({{"responses", allResponsesJson}}).dump(),
            "application/json");
    }

    void handleGetConfigRequest(const httplib::Request& req, httplib::Response& res) {
        if (!isConfigEndpointEnabled()) {
            res.status = 403;  // Forbidden.
            res.set_content("The /config endpoint is not enabled by the server administrator.", "text/plain");
            return;
        }

        auto configFilePath = DataSourceConfigService::get().getConfigFilePath();
        std::filesystem::path path = *configFilePath;
        if (!configFilePath || !std::filesystem::exists(path)) {
            res.status = 404;  // Not found.
            res.set_content("The server does not have a config file.", "text/plain");
            return;
        }

        std::ifstream file(path);
        if (!file) {
            res.status = 500;  // Internal Server Error.
            res.set_content("Failed to open config file.", "text/plain");
            return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();  // Read the whole file into a string stream.
        res.status = 200;  // OK.
        res.set_content(buffer.str(), "text/plain");  // Set the content of the response.
    }

    void handlePostConfigRequest(const httplib::Request& req, httplib::Response& res) {
        std::mutex mtx;
        std::condition_variable cv;
        bool update_done = false;

        // Obtain the configuration file path
        auto configFilePath = DataSourceConfigService::get().getConfigFilePath();
        if (!configFilePath) {
            res.status = 404;  // Not found.
            res.set_content("Configuration file path is undefined.", "text/plain");
            return;
        }

        // Subscribe to configuration changes
        auto subscription = DataSourceConfigService::get().subscribe(
            [&](const std::vector<YAML::Node>& serviceConfigNodes) {
                std::lock_guard<std::mutex> lock(mtx);
                res.status = 200;
                res.set_content("Configuration updated and applied successfully.", "text/plain");
                update_done = true;
                cv.notify_one();
            },
            [&](const std::string& error) {
                std::lock_guard<std::mutex> lock(mtx);
                res.status = 500;
                res.set_content("Error applying the configuration: " + error, "text/plain");
                update_done = true;
                cv.notify_one();
            }
        );

        // Write the new configuration to the file
        std::filesystem::path path = *configFilePath;
        std::ofstream file(path);
        if (!file) {
            res.status = 500;  // Internal Server Error.
            res.set_content("Failed to open the configuration file for writing.", "text/plain");
            return;
        }
        file << req.body;
        file.close();

        // Wait for the subscription callback
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [&] { return update_done; });
    }
};

HttpService::HttpService(Cache::Ptr cache, bool watchConfig)
    : Service(std::move(cache), watchConfig), impl_(std::make_unique<Impl>(*this))
{}

HttpService::~HttpService() = default;

void HttpService::setup(httplib::Server& server)
{
    server.Post(
        "/tiles",
        [&](const httplib::Request& req, httplib::Response& res)
        {
            impl_->handleTilesRequest(req, res);
        });

    server.Get(
        "/sources",
        [this](const httplib::Request& req, httplib::Response& res)
        {
            impl_->handleSourcesRequest(req, res);
        });

    server.Get(
        "/status",
        [this](const httplib::Request& req, httplib::Response& res)
        {
            impl_->handleStatusRequest(req, res);
        });

    server.Post(
        "/locate",
        [this](const httplib::Request& req, httplib::Response& res)
        {
            impl_->handleLocateRequest(req, res);
        });

    server.Get(
        "/config",
        [this](const httplib::Request& req, httplib::Response& res)
        {
            impl_->handleGetConfigRequest(req, res);
        });

    server.Post(
        "/config",
        [this](const httplib::Request& req, httplib::Response& res)
        {
            impl_->handlePostConfigRequest(req, res);
        });
}

}  // namespace mapget
