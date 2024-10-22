#include "http-service.h"
#include "mapget/log.h"
#include "mapget/service/config.h"

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>
#include "cli.h"
#include "httplib.h"
#include "nlohmann/json-schema.hpp"
#include "nlohmann/json.hpp"
#include "yaml-cpp/yaml.h"
#include "picosha2.h"

namespace mapget
{

namespace
{

/**
 * Hash a string using the SHA256 implementation.
 */
std::string stringToHash(const std::string& input)
{
    std::string result;
    picosha2::hash256_hex_string(input, result);
    return result;
}

/**
 * Recursively convert a YAML node to a JSON object,
 * with special handling for sensitive fields.
 * The function returns a nlohmann::json object and updates maskedSecretMap.
 */
nlohmann::json yamlToJson(
    const YAML::Node& yamlNode,
    std::map<std::string, std::string>* maskedSecretMap = nullptr,
    const bool mask = false)
{
    if (yamlNode.IsScalar()) {
        if (mask) {
            auto stringToMask = yamlNode.as<std::string>();
            auto stringMask = "MASKED:"+stringToHash(stringToMask);
            if (maskedSecretMap) {
                maskedSecretMap->insert({stringMask, stringToMask});
            }
            return stringMask;
        }

        try {
            return yamlNode.as<int>();
        }
        catch (const YAML::BadConversion&) {
            try {
                return yamlNode.as<double>();
            }
            catch (const YAML::BadConversion&) {
                try {
                    return yamlNode.as<bool>();
                }
                catch (const YAML::BadConversion&) {
                    return yamlNode.as<std::string>();
                }
            }
        }
    }

    if (mask) {
        log().critical("Cannot mask non-scalar value!");
        return {};
    }

    if (yamlNode.IsSequence()) {
        auto arrayJson = nlohmann::json::array();
        for (const auto& elem : yamlNode) {
            arrayJson.push_back(yamlToJson(elem, maskedSecretMap));
        }
        return arrayJson;
    }

    if (yamlNode.IsMap()) {
        auto objectJson = nlohmann::json::object();
        for (const auto& item : yamlNode) {
            auto key = item.first.as<std::string>();
            const YAML::Node& valueNode = item.second;
            objectJson[key] = yamlToJson(
                valueNode,
                maskedSecretMap,
                key == "api-key" || key == "password");
        }
        return objectJson;
    }

    log().warn("Could not convert {} to JSON!", YAML::Dump(yamlNode));
    return {};
}

/**
 * Recursively convert a JSON object to a YAML node,
 * with special handling for sensitive fields.
 */
YAML::Node jsonToYaml(const nlohmann::json& json, std::map<std::string, std::string> const& maskedSecretMap)
{
    YAML::Node node;
    if (json.is_object()) {
        for (auto it = json.begin(); it != json.end(); ++it) {
            if ((it.key() == "api-key" || it.key() == "password") && it.value().is_string())
            {
                // Un-mask sensitive fields.
                auto value = it.value().get<std::string>();
                auto secretIt = maskedSecretMap.find(value);
                if (secretIt != maskedSecretMap.end()) {
                    node[it.key()] = secretIt->second;
                    continue;
                }
            }
            node[it.key()] = jsonToYaml(it.value(), maskedSecretMap);
        }
    }
    else if (json.is_array()) {
        for (const auto& item : json) {
            node.push_back(jsonToYaml(item, maskedSecretMap));
        }
    }
    else if (json.is_string()) {
        node = json.get<std::string>();
    }
    else if (json.is_number_integer()) {
        node = json.get<int>();
    }
    else if (json.is_number_float()) {
        node = json.get<double>();
    }
    else if (json.is_boolean()) {
        node = json.get<bool>();
    }
    else if (json.is_null()) {
        node = YAML::Node(YAML::NodeType::Null);
    }

    return node;
}
}  // namespace

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

        uint64_t requestId_;
        std::stringstream buffer_;
        std::string responseType_;
        std::unique_ptr<TileLayerStream::Writer> writer_;
        std::vector<LayerTilesRequest::Ptr> requests_;
        TileLayerStream::StringPoolOffsetMap stringOffsets_;

        HttpTilesRequestState()
        {
            static std::atomic_uint64_t nextRequestId;
            writer_ = std::make_unique<TileLayerStream::Writer>(
                [&, this](auto&& msg, auto&& msgType) { buffer_ << msg; },
                stringOffsets_);
            requestId_ = nextRequestId++;
        }

        void parseRequestFromJson(nlohmann::json const& requestJson)
        {
            std::string mapId = requestJson["mapId"];
            std::string layerId = requestJson["layerId"];
            std::vector<TileId> tileIds;
            tileIds.reserve(requestJson["tileIds"].size());
            for (auto const& tid : requestJson["tileIds"].get<std::vector<uint64_t>>())
                tileIds.emplace_back(tid);
            requests_
                .push_back(std::make_shared<LayerTilesRequest>(mapId, layerId, std::move(tileIds)));
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
                buffer_ << nlohmann::to_string(result->toJson()) + "\n";
            }
            resultEvent_.notify_one();
        }
    };

    mutable std::mutex clientRequestMapMutex_;
    mutable std::map<std::string, std::shared_ptr<HttpTilesRequestState>> requestStatePerClientId_;

    void abortRequestsForClientId(std::string clientId, std::shared_ptr<HttpTilesRequestState> newState = nullptr) const
    {
        std::unique_lock clientRequestMapAccess(clientRequestMapMutex_);
        auto clientRequestIt = requestStatePerClientId_.find(clientId);
        if (clientRequestIt != requestStatePerClientId_.end()) {
            // Ensure that any previous requests from the same clientId
            // are finished post-haste!
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
            requestStatePerClientId_.emplace(clientId, newState);
        }
    }

    /**
     * Wraps around the generic mapget service's request() function
     * to include httplib request decoding and response encoding.
     */
    void handleTilesRequest(const httplib::Request& req, httplib::Response& res) const
    {
        // Parse the JSON request.
        nlohmann::json j = nlohmann::json::parse(req.body);
        auto requestsJson = j["requests"];

        // TODO: Limit number of requests to avoid DoS to other users.
        // Within one HTTP request, all requested tiles from the same map+layer
        // combination should be in a single LayerTilesRequest.
        auto state = std::make_shared<HttpTilesRequestState>();
        log().info("Processing tiles request {}", state->requestId_);
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
            request->onFeatureLayer([state](auto&& layer) { state->addResult(layer); });
            request->onSourceDataLayer([state](auto&& layer) { state->addResult(layer); });
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
            std::vector<std::underlying_type_t<RequestStatus>> requestStatuses{};
            for (const auto& r : state->requests_) {
                requestStatuses.push_back(static_cast<std::underlying_type_t<RequestStatus>>(r->getStatus()));
            }
            res.set_content(
                nlohmann::json::object({{"requestStatuses", requestStatuses}}).dump(),
                "application/json");
            return;
        }

        // Parse/Process clientId.
        if (j.contains("clientId")) {
            auto clientId = j["clientId"].get<std::string>();
            abortRequestsForClientId(clientId, state);
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
                if (!success) {
                    log().warn("Aborting tiles request {}", state->requestId_);
                    for (auto& request : state->requests_) {
                        self_.abort(request);
                    }
                }
                else {
                    log().info("Tiles request {} was successful.", state->requestId_);
                }
            });
    }

    void handleAbortRequest(const httplib::Request& req, httplib::Response& res) const
    {
        // Parse the JSON request.
        nlohmann::json j = nlohmann::json::parse(req.body);
        auto requestsJson = j["requests"];

        if (j.contains("clientId")) {
            auto const clientId = j["clientId"].get<std::string>();
            abortRequestsForClientId(clientId);
        }
        else {
            res.status = 400;
            res.set_content("Missing clientId", "text/plain");
        }
    }

    void handleSourcesRequest(const httplib::Request&, httplib::Response& res) const
    {
        auto sourcesInfo = nlohmann::json::array();
        for (auto& source : self_.info()) {
            sourcesInfo.push_back(source.toJson());
        }
        res.set_content(sourcesInfo.dump(), "application/json");
    }

    void handleStatusRequest(const httplib::Request&, httplib::Response& res) const
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

    void handleLocateRequest(const httplib::Request& req, httplib::Response& res) const
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

    static bool openConfigAndSchemaFile(std::ifstream& configFile, std::ifstream& schemaFile, httplib::Response& res)
    {
        auto configFilePath = DataSourceConfigService::get().getConfigFilePath();
        if (!configFilePath.has_value()) {
            res.status = 404;  // Not found.
            res.set_content(
                "The config file path is not set. Check the server configuration.",
                "text/plain");
            return false;
        }

        std::filesystem::path path = *configFilePath;
        if (!configFilePath || !std::filesystem::exists(path)) {
            res.status = 404;  // Not found.
            res.set_content("The server does not have a config file.", "text/plain");
            return false;
        }

        configFile.open(*configFilePath);
        if (!configFile) {
            res.status = 500;  // Internal Server Error.
            res.set_content("Failed to open config file.", "text/plain");
            return false;
        }

        const auto& schemaFilePath = mapget::getPathToSchema();
        if (schemaFilePath.empty()) {
            res.status = 404;  // Not found.
            res.set_content(
                "The schema file path is not set. Check the server configuration.",
                "text/plain");
            return false;
        }

        std::filesystem::path schemaPath = schemaFilePath;
        if (!std::filesystem::exists(schemaPath)) {
            res.status = 404;  // Not found.
            res.set_content("The server does not have a schema file.", "text/plain");
            return false;
        }


        schemaFile.open(schemaPath);
        if (!schemaFile) {
            res.status = 500;  // Internal Server Error.
            res.set_content("Failed to open schema file.", "text/plain");
            return false;
        }

        return true;
    }

    static auto sharedTopLevelConfigKeys()
    {
        return std::array{"sources", "http-settings"};
    }

    static void handleGetConfigRequest(const httplib::Request& req, httplib::Response& res)
    {
        std::ifstream configFile, schemaFile;
        if (!openConfigAndSchemaFile(configFile, schemaFile, res)) {
            return;
        }

        nlohmann::json jsonSchema;
        schemaFile >> jsonSchema;
        schemaFile.close();

        try {
            // Load config YAML
            YAML::Node configYaml = YAML::Load(configFile);
            nlohmann::json jsonConfig;
            for (const auto& key : sharedTopLevelConfigKeys()) {
                if (auto configYamlEntry = configYaml[key])
                    jsonConfig[key] = yamlToJson(configYaml[key]);
            }

            nlohmann::json combinedJson;
            combinedJson["schema"] = jsonSchema;
            combinedJson["model"] = jsonConfig;
            combinedJson["readOnly"] = !isPostConfigEndpointEnabled();

            // Set the response
            res.status = 200;  // OK
            res.set_content(combinedJson.dump(2), "application/json");
        }
        catch (const std::exception& e) {
            res.status = 500;  // Internal Server Error
            res.set_content("Error processing config file: " + std::string(e.what()), "text/plain");
        }
    }

    static void handlePostConfigRequest(const httplib::Request& req, httplib::Response& res)
    {
        if (!isPostConfigEndpointEnabled()) {
            res.status = 403;  // Forbidden.
            res.set_content(
                "The POST /config endpoint is not enabled by the server administrator.",
                "text/plain");
            return;
        }

        std::mutex mtx;
        std::condition_variable cv;
        bool update_done = false;

        std::ifstream configFile, schemaFile;
        if (!openConfigAndSchemaFile(configFile, schemaFile, res)) {
            return;
        }

        // Subscribe to configuration changes.
        auto subscription = DataSourceConfigService::get().subscribe(
            [&](const std::vector<YAML::Node>& serviceConfigNodes)
            {
                std::lock_guard lock(mtx);
                res.status = 200;
                res.set_content("Configuration updated and applied successfully.", "text/plain");
                update_done = true;
                cv.notify_one();
            },
            [&](const std::string& error)
            {
                std::lock_guard lock(mtx);
                res.status = 500;
                res.set_content("Error applying the configuration: " + error, "text/plain");
                update_done = true;
                cv.notify_one();
            });

        // Parse the JSON from the request body.
        nlohmann::json jsonConfig;
        try {
            jsonConfig = nlohmann::json::parse(req.body);
        }
        catch (const nlohmann::json::parse_error& e) {
            res.status = 400;  // Bad Request
            res.set_content("Invalid JSON format: " + std::string(e.what()), "text/plain");
            return;
        }

        // Validate JSON against schema.
        try {
            nlohmann::json jsonSchema;
            schemaFile >> jsonSchema;
            schemaFile.close();

            // Validate with json-schema-validator
            nlohmann::json_schema::json_validator validator;
            validator.set_root_schema(jsonSchema);
            auto _ = validator.validate(jsonConfig);
        }
        catch (const std::exception& e) {
            res.status = 500;  // Internal Server Error.
            res.set_content("Validation failed: " + std::string(e.what()), "text/plain");
            return;
        }

        // Load the YAML, parse the secrets.
        auto yamlConfig = YAML::Load(configFile);
        std::map<std::string, std::string> maskedSecrets;
        yamlToJson(yamlConfig, &maskedSecrets);

        // Create YAML nodes for from JSON nodes.
        for (auto const& key : sharedTopLevelConfigKeys()) {
            if (jsonConfig.contains(key))
                yamlConfig[key] = jsonToYaml(jsonConfig[key], maskedSecrets);
        }

        // Write the YAML to configFilePath.
        update_done = false;
        configFile.close();
        log().trace("Writing new config.");
        std::ofstream newConfigFile(*DataSourceConfigService::get().getConfigFilePath());
        newConfigFile << yamlConfig;
        newConfigFile.close();

        // Wait for the subscription callback.
        std::unique_lock<std::mutex> lk(mtx);
        if (!cv.wait_for(lk, std::chrono::seconds(60), [&] { return update_done; })) {
            res.status = 500;  // Internal Server Error.
            res.set_content("Timeout while waiting for config to update.", "text/plain");
        }
    }
};

HttpService::HttpService(Cache::Ptr cache, bool watchConfig)
    : Service(std::move(cache), watchConfig), impl_(std::make_unique<Impl>(*this))
{
}

HttpService::~HttpService() = default;

void HttpService::setup(httplib::Server& server)
{
    server.Post(
        "/tiles",
        [&](const httplib::Request& req, httplib::Response& res)
        { impl_->handleTilesRequest(req, res); });

    server.Post(
        "/abort",
        [&](const httplib::Request& req, httplib::Response& res)
        { impl_->handleAbortRequest(req, res); });

    server.Get(
        "/sources",
        [this](const httplib::Request& req, httplib::Response& res)
        { impl_->handleSourcesRequest(req, res); });

    server.Get(
        "/status",
        [this](const httplib::Request& req, httplib::Response& res)
        { impl_->handleStatusRequest(req, res); });

    server.Post(
        "/locate",
        [this](const httplib::Request& req, httplib::Response& res)
        { impl_->handleLocateRequest(req, res); });

    server.Get(
        "/config",
        [this](const httplib::Request& req, httplib::Response& res)
        { impl_->handleGetConfigRequest(req, res); });

    server.Post(
        "/config",
        [this](const httplib::Request& req, httplib::Response& res)
        { impl_->handlePostConfigRequest(req, res); });
}

}  // namespace mapget
