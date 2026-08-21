#include "http-service-impl.h"

#include "cli.h"
#include "mapget/log.h"
#include "mapget/service/config.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

[[nodiscard]] YAML::Node loadConfigYamlForPublicSections()
{
    auto configFilePath = DataSourceConfigService::get().getConfigFilePath();
    if (!configFilePath.has_value()) {
        return {};
    }

    std::filesystem::path path = *configFilePath;
    if (!std::filesystem::exists(path)) {
        return {};
    }

    std::ifstream configFile(*configFilePath);
    if (!configFile) {
        return {};
    }

    try {
        return YAML::Load(configFile);
    }
    catch (const YAML::Exception& yamlError) {
        log().warn("Failed to parse YAML config for public /config sections: {}", yamlError.what());
    }
    return {};
}

[[nodiscard]] nlohmann::json buildUnavailableConfigResponse(
    std::string_view reason,
    YAML::Node const& fullConfig = {},
    bool readOnly = true,
    bool includeSchema = false)
{
    auto& configService = DataSourceConfigService::get();
    nlohmann::json response = {
        {"schema", includeSchema ? configService.getDataSourceConfigSchema() : nlohmann::json::object()},
        {"model", nlohmann::json::object()},
        {"readOnly", readOnly},
        {"datasourceConfigUnavailable", true},
        {"datasourceConfigUnavailableReason", reason},
    };
    auto publicSections = configService.getPublicConfigSections(fullConfig);
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

/** Atomically replaces a config with exact bytes and optional preserved mode. */
[[nodiscard]] std::optional<std::string> replaceConfigFileContents(
    std::filesystem::path const& configFilePath,
    std::string const& contents,
    std::optional<std::filesystem::perms> permissions)
{
    static std::atomic_uint64_t tempFileCounter{0};
    auto tempConfigPath = configFilePath;
    auto tempFileSuffix = std::string(".tmp.")
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "."
        + std::to_string(tempFileCounter.fetch_add(1, std::memory_order_relaxed));
    tempConfigPath += tempFileSuffix;

    {
        std::ofstream tempConfigFile(tempConfigPath, std::ios::out | std::ios::trunc);
        if (!tempConfigFile) {
            return std::string("failed to open temporary config file ") + tempConfigPath.string();
        }

        tempConfigFile << contents;
        tempConfigFile.flush();
        if (!tempConfigFile) {
            std::error_code cleanupError;
            std::filesystem::remove(tempConfigPath, cleanupError);
            return std::string("failed to write temporary config file ") + tempConfigPath.string();
        }
    }

    if (permissions) {
        std::error_code permissionError;
        std::filesystem::permissions(
            tempConfigPath,
            *permissions,
            std::filesystem::perm_options::replace,
            permissionError);
        if (permissionError) {
            std::error_code cleanupError;
            std::filesystem::remove(tempConfigPath, cleanupError);
            return std::string("failed to preserve config file permissions: ")
                + permissionError.message();
        }
    }

#ifdef _WIN32
    // Windows std::filesystem::rename does not portably replace existing files.
    if (!MoveFileExW(
            tempConfigPath.wstring().c_str(),
            configFilePath.wstring().c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        auto const errorCode = GetLastError();
        std::error_code cleanupError;
        std::filesystem::remove(tempConfigPath, cleanupError);
        return std::string("failed to replace config file ") + configFilePath.string()
            + " with temporary config file " + tempConfigPath.string() + ": Windows error "
            + std::to_string(errorCode);
    }
#else
    std::error_code replaceError;
    std::filesystem::rename(tempConfigPath, configFilePath, replaceError);
    if (replaceError) {
        std::error_code cleanupError;
        std::filesystem::remove(tempConfigPath, cleanupError);
        return std::string("failed to replace config file ") + configFilePath.string()
            + " with temporary config file " + tempConfigPath.string() + ": "
            + replaceError.message();
    }
#endif

    return std::nullopt;
}

/**
 * Rewrite the config via a temporary file so filesystem watchers never observe
 * an empty or partially written target file.
 */
[[nodiscard]] std::optional<std::string> replaceConfigFile(
    std::filesystem::path const& configFilePath,
    YAML::Node const& yamlConfig,
    std::string* serializedOutput = nullptr)
{
    std::ostringstream serializedConfig;
    serializedConfig << yamlConfig;
    auto serialized = serializedConfig.str();
    if (serializedOutput) {
        *serializedOutput = serialized;
    }

    std::error_code statusError;
    auto const originalPermissions = std::filesystem::status(configFilePath, statusError).permissions();
    return replaceConfigFileContents(
        configFilePath,
        serialized,
        statusError ? std::nullopt : std::optional{originalPermissions});
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
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
{
    auto& configService = DataSourceConfigService::get();
    auto const cacheResetAvailable =
        config_.cacheResetEnabled &&
        authHeadersMatch(
            config_.cacheResetAuthHeaderAlternatives,
            detail::authHeadersFromRequest(req));
    auto respond = [&](nlohmann::json payload) {
        if (!payload.contains("capabilities") ||
            !payload["capabilities"].is_object())
        {
            payload["capabilities"] =
                nlohmann::json::object();
        }
        payload["capabilities"]["cacheReset"] =
            cacheResetAvailable;
        auto response = jsonResponse(
            std::move(payload));
        response->addHeader(
            "Cache-Control",
            "private, no-store");
        callback(std::move(response));
    };

    if (!isGetConfigEndpointEnabled()) {
        const bool readOnly = !isPostConfigEndpointEnabled();
        respond(buildUnavailableConfigResponse(
            kUnavailableReasonGetConfigDisabled,
            loadConfigYamlForPublicSections(),
            readOnly,
            !readOnly));
        return;
    }

    auto configFilePath = configService.getConfigFilePath();
    if (!configFilePath.has_value()) {
        respond(buildUnavailableConfigResponse(kUnavailableReasonConfigPathUnset));
        return;
    }

    std::filesystem::path path = *configFilePath;
    if (!std::filesystem::exists(path)) {
        respond(buildUnavailableConfigResponse(kUnavailableReasonConfigFileMissing));
        return;
    }

    std::ifstream configFile(*configFilePath);
    if (!configFile) {
        respond(buildUnavailableConfigResponse(kUnavailableReasonConfigFileOpenFailed));
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

        respond(std::move(combinedJson));
    }
    catch (const std::invalid_argument& validationError) {
        log().warn("GET /config validation failed: {}", validationError.what());
        respond(buildUnavailableConfigResponse(kUnavailableReasonConfigValidationFailed));
    }
    catch (const YAML::Exception& yamlError) {
        log().warn("GET /config parse failed: {}", yamlError.what());
        respond(buildUnavailableConfigResponse(kUnavailableReasonConfigParseFailed));
    }
    catch (const std::exception& e) {
        log().warn("GET /config failed: {}", e.what());
        respond(buildUnavailableConfigResponse(kUnavailableReasonConfigParseFailed));
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

    auto mutationLock = DataSourceConfigService::get().lockConfigMutation();

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
    auto configFilePath = DataSourceConfigService::get().getConfigFilePath();
    if (!configFilePath) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k500InternalServerError);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody("Error applying the configuration: config file path is no longer set.");
        state->done = true;
        state->callback(resp);
        state->subscription.reset();
        return;
    }

    if (auto writeError = replaceConfigFile(*configFilePath, yamlConfig)) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k500InternalServerError);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody(std::string("Error applying the configuration: ") + *writeError);
        state->done = true;
        state->callback(resp);
        state->subscription.reset();
        return;
    }

    // Ignore watcher callbacks until the rewritten file has been fully replaced.
    state->wroteConfig = true;
    DataSourceConfigService::get().loadConfig(*configFilePath, false);

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

void HttpService::Impl::handlePatchConfigRequest(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
{
    auto respondText = [&](drogon::HttpStatusCode status, std::string body) {
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(status);
        response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        response->setBody(std::move(body));
        callback(std::move(response));
    };

    auto& configService = DataSourceConfigService::get();
    if (!isPostConfigEndpointEnabled()) {
        respondText(
            drogon::k403Forbidden,
            "Configuration writes through /config are not enabled by the server administrator.");
        return;
    }

    auto ifMatch = req->getHeader("If-Match");
    if (ifMatch.empty()) {
        respondText(drogon::k428PreconditionRequired, "If-Match is required.");
        return;
    }
    if (ifMatch.starts_with("W/")) {
        ifMatch.erase(0, 2);
    }
    if (ifMatch.size() >= 2 && ifMatch.front() == '"' && ifMatch.back() == '"') {
        ifMatch = ifMatch.substr(1, ifMatch.size() - 2);
    }

    nlohmann::json patch;
    try {
        patch = nlohmann::json::parse(std::string(req->body()));
    }
    catch (nlohmann::json::parse_error const& error) {
        respondText(drogon::k400BadRequest, std::string("Invalid JSON: ") + error.what());
        return;
    }
    if (!patch.is_object()
        || patch.size() != 2
        || !patch.contains("path")
        || !patch["path"].is_string()
        || patch["path"].get_ref<std::string const&>().empty()
        || !patch.contains("value")) {
        respondText(
            drogon::k400BadRequest,
            "The request must contain exactly a non-empty string 'path' and a 'value'.");
        return;
    }
    auto const path = patch["path"].get<std::string>();
    if (!configService.hasPublicConfigFieldWriter(path)) {
        respondText(drogon::k400BadRequest, "No public config writer is registered for the requested path.");
        return;
    }

    auto mutationLock = configService.lockConfigMutation();
    auto const currentRevision = configService.getConfigFileRevision();
    if (!currentRevision) {
        respondText(drogon::k404NotFound, "The durable config file is unavailable.");
        return;
    }
    if (ifMatch != *currentRevision) {
        respondText(drogon::k412PreconditionFailed, "The config revision changed; refetch and retry.");
        return;
    }

    auto const configFilePath = configService.getConfigFilePath();
    if (!configFilePath) {
        respondText(drogon::k404NotFound, "The durable config file path is unavailable.");
        return;
    }

    std::ifstream originalFile(*configFilePath, std::ios::binary);
    if (!originalFile) {
        respondText(drogon::k500InternalServerError, "Failed to preserve the current config for rollback.");
        return;
    }
    std::ostringstream originalStream;
    originalStream << originalFile.rdbuf();
    auto const originalContents = originalStream.str();
    std::error_code originalStatusError;
    auto const originalPermissions = std::filesystem::status(
        *configFilePath,
        originalStatusError).permissions();

    YAML::Node yamlConfig;
    try {
        yamlConfig = YAML::LoadFile(*configFilePath);
    }
    catch (std::exception const& error) {
        respondText(drogon::k500InternalServerError, std::string("Failed to read config: ") + error.what());
        return;
    }

    auto writeResult = configService.applyPublicConfigFieldWrite(
        path,
        yamlConfig,
        patch["value"]);
    if (writeResult.error) {
        respondText(drogon::k400BadRequest, *writeResult.error);
        return;
    }

    // Close the parse/validation race with generic POST or an external file edit.
    if (configService.getConfigFileRevision() != currentRevision) {
        respondText(drogon::k412PreconditionFailed, "The config revision changed; refetch and retry.");
        return;
    }

    std::string serialized;
    if (auto error = replaceConfigFile(*configFilePath, yamlConfig, &serialized)) {
        respondText(drogon::k500InternalServerError, "Failed to write config: " + *error);
        return;
    }

    try {
        auto readBack = YAML::LoadFile(*configFilePath);
        if (!readBack || !readBack.IsMap()) {
            throw std::runtime_error("written document is not a YAML object");
        }
        auto readBackResult = configService.applyPublicConfigFieldWrite(
            path,
            readBack,
            writeResult.canonicalValue);
        if (readBackResult.error || readBackResult.canonicalValue != writeResult.canonicalValue) {
            throw std::runtime_error("written public config field did not validate canonically");
        }
    }
    catch (std::exception const& error) {
        auto restoreError = replaceConfigFileContents(
            *configFilePath,
            originalContents,
            originalStatusError ? std::nullopt : std::optional{originalPermissions});
        if (!restoreError) {
            configService.acknowledgePublicConfigWrite(originalContents);
        }
        respondText(
            drogon::k500InternalServerError,
            std::string("Canonical read-back failed; ")
                + (restoreError ? "rollback also failed: " + *restoreError : "the original config was restored: ")
                + error.what());
        return;
    }

    configService.acknowledgePublicConfigWrite(serialized);
    auto const newRevision = configService.getConfigFileRevision();
    nlohmann::json body = {
        {"path", path},
        {"value", std::move(writeResult.canonicalValue)},
        {"revision", newRevision.value_or("")}};
    auto response = jsonResponse(std::move(body));
    response->addHeader("Cache-Control", "private, no-store");
    if (newRevision) {
        response->addHeader("ETag", "\"" + *newRevision + "\"");
    }
    callback(std::move(response));
}

}  // namespace mapget
