#include "http-service-impl.h"

#include "cli.h"
#include "mapget/log.h"
#include "mapget/service/config.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_map>

#include "nlohmann/json.hpp"
#include "yaml-cpp/yaml.h"

namespace mapget
{

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
    state->wroteConfig = true;
    if (auto configFilePath = DataSourceConfigService::get().getConfigFilePath()) {
        std::ofstream newConfigFile(*configFilePath);
        newConfigFile << yamlConfig;
        newConfigFile.close();
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

