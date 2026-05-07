#include "http-service-impl.h"

#include "cli.h"
#include "mapget/log.h"
#include "mapget/service/config.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "nlohmann/json.hpp"
#include "yaml-cpp/yaml.h"

namespace mapget
{
namespace
{

constexpr std::string_view kUnavailableReasonGetConfigDisabled = "getConfigDisabled";
constexpr std::string_view kUnavailableReasonConfigPathUnset = "configPathUnset";
constexpr std::string_view kUnavailableReasonConfigFileMissing = "configFileMissing";
constexpr std::string_view kUnavailableReasonConfigFileOpenFailed = "configFileOpenFailed";
constexpr std::string_view kUnavailableReasonConfigParseFailed = "configParseFailed";
constexpr std::string_view kUnavailableReasonConfigValidationFailed = "configValidationFailed";

[[nodiscard]] nlohmann::json buildUnavailableConfigResponse(std::string_view reason)
{
    auto& configService = DataSourceConfigService::get();
    nlohmann::json response = {
        {"schema", nlohmann::json::object()},
        {"model", nlohmann::json::object()},
        {"readOnly", true},
        {"datasourceConfigUnavailable", true},
        {"datasourceConfigUnavailableReason", reason},
    };
    auto publicSections = configService.getPublicConfigSections(YAML::Node{});
    for (auto& [name, value] : publicSections.items()) {
        response[name] = std::move(value);
    }
    return response;
}

[[nodiscard]] drogon::HttpResponsePtr jsonResponse(nlohmann::json payload)
{
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(payload.dump(2));
    return resp;
}

}  // namespace

drogon::HttpResponsePtr HttpService::Impl::openConfigFile(std::ifstream& configFile)
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

void HttpService::Impl::handleGetConfigRequest(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& configService = DataSourceConfigService::get();

    if (!isGetConfigEndpointEnabled()) {
        callback(jsonResponse(buildUnavailableConfigResponse(kUnavailableReasonGetConfigDisabled)));
        return;
    }

    auto configFilePath = configService.getConfigFilePath();
    if (!configFilePath.has_value()) {
        callback(jsonResponse(buildUnavailableConfigResponse(kUnavailableReasonConfigPathUnset)));
        return;
    }

    std::filesystem::path path = *configFilePath;
    if (!std::filesystem::exists(path)) {
        callback(jsonResponse(buildUnavailableConfigResponse(kUnavailableReasonConfigFileMissing)));
        return;
    }

    std::ifstream configFile(*configFilePath);
    if (!configFile) {
        callback(jsonResponse(buildUnavailableConfigResponse(kUnavailableReasonConfigFileOpenFailed)));
        return;
    }

    try {
        YAML::Node configYaml = YAML::Load(configFile);
        configService.validateDataSourceConfig(configYaml);

        nlohmann::json jsonConfig;
        std::unordered_map<std::string, std::string> maskedSecretMap;
        for (const auto& key : configService.topLevelDataSourceConfigKeys()) {
            if (auto configYamlEntry = configYaml[key])
                jsonConfig[key] = yamlToJson(configYaml[key], true, &maskedSecretMap);
        }

        nlohmann::json combinedJson = {
            {"schema", configService.getDataSourceConfigSchema()},
            {"model", std::move(jsonConfig)},
            {"readOnly", !isPostConfigEndpointEnabled()},
            {"datasourceConfigUnavailable", false},
            {"datasourceConfigUnavailableReason", nullptr},
        };
        auto publicSections = configService.getPublicConfigSections(configYaml);
        for (auto& [name, value] : publicSections.items()) {
            combinedJson[name] = std::move(value);
        }

        callback(jsonResponse(std::move(combinedJson)));
    }
    catch (const std::invalid_argument& validationError) {
        log().warn("GET /config validation failed: {}", validationError.what());
        callback(jsonResponse(buildUnavailableConfigResponse(kUnavailableReasonConfigValidationFailed)));
    }
    catch (const YAML::Exception& yamlError) {
        log().warn("GET /config parse failed: {}", yamlError.what());
        callback(jsonResponse(buildUnavailableConfigResponse(kUnavailableReasonConfigParseFailed)));
    }
    catch (const std::exception& e) {
        log().warn("GET /config failed: {}", e.what());
        callback(jsonResponse(buildUnavailableConfigResponse(kUnavailableReasonConfigParseFailed)));
    }
}

void HttpService::Impl::handlePostConfigRequest(
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
    if (auto configFilePath = DataSourceConfigService::get().getConfigFilePath()) {
        std::ofstream newConfigFile(*configFilePath);
        if (!newConfigFile) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k500InternalServerError);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody(std::string("Error applying the configuration: failed to open ") + *configFilePath);
            state->done = true;
            state->callback(resp);
            state->subscription.reset();
            return;
        }

        newConfigFile << yamlConfig;
        newConfigFile.flush();
        newConfigFile.close();

        // Ignore watcher callbacks until the rewritten file has been fully flushed.
        state->wroteConfig = true;
        DataSourceConfigService::get().loadConfig(*configFilePath, false);
    }

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

}  // namespace mapget
