#include "http-service-impl.h"

#include "cli.h"
#include "mapget/log.h"
#include "mapget/service/config.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>

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

    auto response = drogon::HttpResponse::newHttpResponse();
    response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
    try {
        auto& service = DataSourceConfigService::get();
        auto path = service.getConfigFilePath();
        if (!path || !std::filesystem::exists(*path)) {
            response->setStatusCode(drogon::k404NotFound);
            response->setBody("The server does not have a config file.");
            callback(response);
            return;
        }
        auto model = nlohmann::json::parse(req->body());
        // REST POST keeps its complete-model contract; MCP supports explicit partial sections.
        service.validateDataSourceConfig(model);
        auto result = updateDatasourceConfiguration(model);
        response->setStatusCode(
            result.at("reloadAccepted").get<bool>() ?
                drogon::k200OK :
                drogon::k500InternalServerError);
        response->setBody(
            result.at("reloadAccepted").get<bool>() ?
                "Configuration updated and applied successfully." :
                "Configuration persisted, but reload failed. Consult protected server logs.");
    }
    catch (nlohmann::json::parse_error const&) {
        response->setStatusCode(drogon::k400BadRequest);
        response->setBody("Invalid JSON format.");
    }
    catch (std::invalid_argument const&) {
        response->setStatusCode(drogon::k500InternalServerError);
        response->setBody("Validation failed: invalid datasource configuration.");
    }
    catch (std::exception const&) {
        response->setStatusCode(drogon::k500InternalServerError);
        response->setBody("Configuration validation or persistence failed.");
    }
    callback(response);
}

nlohmann::json HttpService::Impl::datasourceConfiguration() const
{
    auto& service = DataSourceConfigService::get();
    auto lock = service.lockConfigMutation();
    auto path = service.getConfigFilePath();
    if (!isGetConfigEndpointEnabled() || !path ||
        std::filesystem::file_size(*path) > 8 * 1024 * 1024)
        throw std::runtime_error("Datasource configuration unavailable");
    auto yaml = YAML::LoadFile(*path);
    auto model = nlohmann::json::object();
    std::unordered_map<std::string, std::string> masked;
    for (auto const& key : service.topLevelDataSourceConfigKeys())
        if (yaml[key])
            model[key] = yamlToJson(yaml[key], true, &masked);
    return {
        {"model", std::move(model)},
        {"revision", service.getConfigFileRevision().value_or("")},
        {"persistence", "server-config-file"}};
}

nlohmann::json HttpService::Impl::updateDatasourceConfiguration(
    nlohmann::json const& model,
    std::string const& expectedRevision) const
{
    if (!model.is_object())
        throw std::invalid_argument("Expected datasource configuration object");
    auto& service = DataSourceConfigService::get();
    auto lock = service.lockConfigMutation();
    auto path = service.getConfigFilePath();
    if (!isPostConfigEndpointEnabled() || !path ||
        std::filesystem::file_size(*path) > 8 * 1024 * 1024)
        throw std::runtime_error("Datasource configuration unavailable");
    auto revision = service.getConfigFileRevision();
    if (!expectedRevision.empty() && revision != expectedRevision)
        throw std::system_error(std::make_error_code(std::errc::state_not_recoverable));
    auto yaml = YAML::LoadFile(*path);
    std::unordered_map<std::string, std::string> masked;
    auto const keys = service.topLevelDataSourceConfigKeys();
    for (auto const& key : keys)
        if (yaml[key])
            (void)yamlToJson(yaml[key], true, &masked);
    // A stale/forged mask must never silently replace a real credential with the mask text.
    std::vector<nlohmann::json const*> pending{&model};
    while (!pending.empty()) {
        auto value = pending.back();
        pending.pop_back();
        if (value->is_string()) {
            auto const& text = value->get_ref<std::string const&>();
            if (text.starts_with("MASKED:") && !masked.contains(text))
                throw std::invalid_argument("Unknown masked credential");
        }
        else if (value->is_structured()) {
            for (auto const& child : *value)
                pending.push_back(&child);
        }
    }
    // Never let an MCP datasource edit alter OAuth, listener or host runtime configuration.
    for (auto const& [key, value] : model.items()) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            throw std::invalid_argument("Unknown datasource configuration section");
        yaml[key] = jsonToYaml(value, masked);
    }
    service.validateDataSourceConfig(yaml);
    if (service.getConfigFileRevision() != revision)
        throw std::system_error(std::make_error_code(std::errc::state_not_recoverable));
    if (replaceConfigFile(*path, yaml))
        throw std::runtime_error("Config persistence failed");

    // loadConfig reports synchronously; datasource construction continues asynchronously.
    bool reloadAccepted = true;
    auto subscription =
        service.subscribe([](auto const&) {}, [&](auto const&) { reloadAccepted = false; });
    service.loadConfig(*path, false);
    return {
        {"persisted", true},
        {"reloadAccepted", reloadAccepted},
        {"initializationPending", reloadAccepted},
        {"revision", service.getConfigFileRevision().value_or("")},
        {"persistence", "server-config-file"}};
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
    originalFile.close();
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
