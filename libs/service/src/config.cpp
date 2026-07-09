
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <chrono>
#include <cctype>
#include <nlohmann/json-schema.hpp>
#include <fmt/format.h>

#include "picosha2.h"
#include "config.h"
#include "mapget/log.h"

namespace mapget
{
namespace {

nlohmann::json ttlSchema()
{
    return {
        {"type", "integer"},
        {"title", "TTL (seconds)"},
        {"description", "Time-to-live for cached tiles produced by this datasource. 0 = infinite."}
    };
}

nlohmann::json authHeaderSchema()
{
    return {
        {"type", "object"},
        {"title", "Authorization headers"},
        {"description", "Map of header names to regular expressions. At least one must match for access."},
        {"additionalProperties", {{"type", "string"}}}
    };
}

nlohmann::json enabledSchema()
{
    return {
        {"type", "boolean"},
        {"title", "Enabled"},
        {"description", "If false, this datasource entry is skipped."}
    };
}

void reportInitStatus(DataSourceInitContext& initContext, std::string message)
{
    if (initContext.setStatusMessage) {
        initContext.setStatusMessage(std::move(message));
    }
}

[[nodiscard]] bool initCancelled(DataSourceInitContext const& initContext)
{
    return initContext.isCancelled && initContext.isCancelled();
}

[[nodiscard]] std::string lowercaseConfigKey(std::string key)
{
    std::ranges::transform(
        key,
        key.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

[[nodiscard]] bool isSecretConfigKey(std::string const& key)
{
    auto lowerKey = lowercaseConfigKey(key);
    auto compactKey = lowerKey;
    compactKey.erase(
        std::remove_if(
            compactKey.begin(),
            compactKey.end(),
            [](char c) { return c == '-' || c == '_'; }),
        compactKey.end());

    return compactKey == "apikey" ||
           lowerKey.find("password") != std::string::npos ||
           lowerKey.find("secret") != std::string::npos;
}

[[nodiscard]] std::optional<std::string> scalarString(YAML::Node const& node, std::string const& key)
{
    if (auto value = node[key]; value && value.IsScalar()) {
        try {
            return value.as<std::string>();
        }
        catch (std::exception const&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::unordered_map<std::string, std::regex> parseAuthHeaderAlternatives(YAML::Node const& descriptor)
{
    std::unordered_map<std::string, std::regex> result;
    for (auto authOption : descriptor["auth-header"]) {
        auto key = authOption.first.as<std::string>();
        auto value = authOption.second.as<std::string>();
        std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        result.emplace(std::move(key), std::regex(value));
    }
    return result;
}

void applyGenericDataSourceOptions(DataSource& dataSource, YAML::Node const& descriptor)
{
    if (auto ttlNode = descriptor["ttl"]) {
        auto ttlSeconds = ttlNode.as<int64_t>();
        if (ttlSeconds < 0) {
            throw std::runtime_error("`ttl` must be non-negative.");
        }
        dataSource.setTtl(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds(ttlSeconds)));
    }

    for (auto const& [key, value] : parseAuthHeaderAlternatives(descriptor)) {
        dataSource.requireAuthHeaderRegexMatchOption(key, value);
    }
}

} // namespace

DataSourceConfigService& DataSourceConfigService::get()
{
    static DataSourceConfigService instance;
    return instance;
}

DataSourceConfigService::Subscription::~Subscription()
{
    DataSourceConfigService::get().unsubscribe(id_);
}

DataSourceConfigService::Subscription::Subscription(uint32_t id) : id_(id) {}

std::unique_ptr<DataSourceConfigService::Subscription> DataSourceConfigService::subscribe(
    std::function<void(std::vector<YAML::Node> const&)> const& successCallback,
    std::function<void(std::string const&)> const& errorCallback)
{
    if (!successCallback) {
        log().warn("Refusing to register config subscription with NULL callback.");
        return nullptr;
    }

    std::lock_guard memberAccessLock(memberAccessMutex_);
    auto sub = std::make_unique<Subscription>(nextSubscriptionId_++);
    log().debug("Registering config subscription with ID: {}", sub->id_);
    subscriptions_[sub->id_] = {successCallback, errorCallback};
    // Optionally, trigger the callback with the current configuration immediately
    if (!currentConfig_.empty()) {
        log().debug("Triggering immediate callback for subscription {} with {} config nodes", 
                    sub->id_, currentConfig_.size());
        successCallback(currentConfig_);
    }
    return sub;
}

void DataSourceConfigService::unsubscribe(uint32_t id)
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    subscriptions_.erase(id);
}

void DataSourceConfigService::loadConfig(std::string const& path, bool startWatchThread)
{
    log().debug("loadConfig called with path: {}, startWatchThread: {}", path, startWatchThread);
    configFilePath_ = path;

    // Force reload by clearing checksum.
    lastConfigSHA256_.clear();

    // Notify subscribers immediately to allow dependent apps to proceed
    // This is needed as there otherwise apps would have to wait manually
    // for the callback to be called before they can continue.
    loadConfig();

    if (startWatchThread)
        startConfigFileWatchThread();
}

void DataSourceConfigService::loadConfig()
{
    std::optional<std::string> error;

    log().trace("loadConfig() called, configFilePath: {}", configFilePath_);

    try {
        std::ifstream file(configFilePath_);
        if (!file) {
            log().trace("Config file does not exist or cannot be opened: {}", configFilePath_);
            throw YAML::Exception(YAML::Mark::null_mark(), "The file does not exist.");
        }

        // Add current file name to input buffer to force a reload when file has been moved.
        std::stringstream buffer;
        buffer << file.rdbuf() << configFilePath_;

        std::string sha256;
        picosha2::hash256_hex_string(buffer.str(), sha256);

        log().trace("Config file SHA256: {}, last SHA256: {}", sha256, lastConfigSHA256_);

        if (sha256 == lastConfigSHA256_)
        {
            log().trace("Config file unchanged. No need to reload.");
            return;
        }

        YAML::Node config = YAML::LoadFile(configFilePath_);
        {
            std::lock_guard memberAccessLock(memberAccessMutex_);
            validateDataSourceConfig(config);
            currentConfig_.clear();
            dataSourceConfigStats_ = {};
            if (auto sourcesNode = config["sources"]) {
                for (auto const& node : sourcesNode)
                {
                    ++dataSourceConfigStats_.configured;
                    const bool enabled = !node["enabled"].IsDefined() || node["enabled"].as<bool>(true);
                    if (enabled) {
                        ++dataSourceConfigStats_.enabled;
                    }
                    else {
                        ++dataSourceConfigStats_.disabled;
                    }
                    currentConfig_.push_back(node);
                }
            }
            else {
                log().debug(fmt::format("The config file {} does not have a sources node.", configFilePath_));
            }
            lastConfigSHA256_ = sha256;
            log().debug("Notifying {} subscribers", subscriptions_.size());
            for (const auto& [subId, subCb] : subscriptions_) {
                log().debug("Calling subscriber {}", subId);
                subCb.success_(currentConfig_);
            }
        }
    }
    catch (const std::invalid_argument& validationError)
    {
        error = fmt::format("Failed to validate YAML config {}: {}", configFilePath_, validationError.what());
    }
    catch (const YAML::Exception& yamlError)
    {
        error = fmt::format("Failed to parse YAML config {}: {}", configFilePath_, yamlError.what());
    }

    if (error) {
        log().error(*error);
        for (const auto& [subId, subCb] : subscriptions_) {
            if (subCb.error_)
                subCb.error_(*error);
        }
    }
}

DataSource::Ptr DataSourceConfigService::makeDataSource(YAML::Node const& descriptor)
{
    DataSourceInitContext initContext;
    return makeDataSource(descriptor, initContext);
}

DataSource::Ptr DataSourceConfigService::makeDataSource(YAML::Node const& descriptor, DataSourceInitContext& initContext)
{
    try {
        if (auto enabledNode = descriptor["enabled"];
            enabledNode.IsDefined() && !enabledNode.as<bool>(true))
        {
            return nullptr;
        }
    }
    catch (std::exception const& e) {
        auto message = fmt::format("Invalid datasource `enabled` value: {}", e.what());
        log().error("{}", message);
        reportInitStatus(initContext, std::move(message));
        return nullptr;
    }

    if (auto typeNode = descriptor["type"]) {
        auto type = typeNode.as<std::string>();
        DataSourceConstructor constructor;
        {
            std::lock_guard memberAccessLock(memberAccessMutex_);
            auto it = constructors_.find(type);
            if (it != constructors_.end()) {
                constructor = it->second.constructor_;
            }
        }

        if (!constructor) {
            auto message = fmt::format("No constructor is registered for datasource type: {}", type);
            log().error("{}", message);
            reportInitStatus(initContext, std::move(message));
            return nullptr;
        }

        try {
            if (initCancelled(initContext)) {
                reportInitStatus(initContext, "Datasource construction was cancelled.");
                return nullptr;
            }
            if (auto result = constructor(descriptor, initContext)) {
                if (initCancelled(initContext)) {
                    reportInitStatus(initContext, "Datasource construction was cancelled.");
                    return nullptr;
                }
                applyGenericDataSourceOptions(*result, descriptor);
                return result;
            }
            if (initCancelled(initContext)) {
                reportInitStatus(initContext, "Datasource construction was cancelled.");
                return nullptr;
            }
            auto message = fmt::format("Datasource constructor for type {} returned NULL.", type);
            log().error("{}", message);
            return nullptr;
        }
        catch (std::exception const& e) {
            auto message = fmt::format("Exception while making `{}` datasource: {}", type, e.what());
            log().error("{}", message);
            reportInitStatus(initContext, std::move(message));
            return nullptr;
        }
    }
    auto message = std::string("A YAML datasource descriptor is missing the `type` key!");
    log().error("{}", message);
    reportInitStatus(initContext, std::move(message));
    return nullptr;
}

DataSourceDescriptor DataSourceConfigService::describeDataSource(YAML::Node const& descriptor, uint32_t configIndex) const
{
    DataSourceDescriptor result;
    result.configIndex = configIndex;
    result.type = scalarString(descriptor, "type").value_or("");
    result.sourceId = scalarString(descriptor, "id").value_or("");
    if (result.sourceId.empty()) {
        result.sourceId = scalarString(descriptor, "sourceId").value_or(fmt::format("config:{}", configIndex));
    }
    result.configuredMapId = scalarString(descriptor, "mapId");
    if (!result.configuredMapId) {
        result.configuredMapId = scalarString(descriptor, "configuredMapId");
    }

    try {
        result.addOn = descriptor["addOn"].as<bool>(false);
    }
    catch (std::exception const&) {
        result.addOn = false;
    }

    try {
        result.authHeaderAlternatives = parseAuthHeaderAlternatives(descriptor);
    }
    catch (std::exception const& e) {
        log().warn(
            "Could not parse auth-header options for datasource config index {}: {}",
            configIndex,
            e.what());
    }

    DataSourceDescribeFn describe;
    {
        std::lock_guard memberAccessLock(memberAccessMutex_);
        auto it = constructors_.find(result.type);
        if (it != constructors_.end()) {
            describe = it->second.describe_;
        }
    }

    if (describe) {
        DataSourceDescriptor described;
        try {
            described = describe(descriptor, configIndex);
        }
        catch (std::exception const& e) {
            log().warn(
                "Datasource descriptor callback for type {} at index {} failed: {}",
                result.type,
                configIndex,
                e.what());
            return result;
        }
        if (described.sourceId.empty()) {
            described.sourceId = result.sourceId;
        }
        if (described.type.empty()) {
            described.type = result.type;
        }
        described.configIndex = configIndex;
        if (!described.configuredMapId) {
            described.configuredMapId = result.configuredMapId;
        }
        if (described.authHeaderAlternatives.empty()) {
            described.authHeaderAlternatives = std::move(result.authHeaderAlternatives);
        }
        return described;
    }

    return result;
}

void DataSourceConfigService::registerDataSourceType(
    std::string const& typeName,
    LegacyDataSourceConstructor constructor,
    nlohmann::json schema,
    DataSourceDescribeFn describe)
{
    if (!constructor) {
        log().warn("Refusing to register NULL constructor for datasource type {}", typeName);
        return;
    }
    registerDataSourceType(
        typeName,
        [constructor = std::move(constructor)](YAML::Node const& node, DataSourceInitContext&) {
            return constructor(node);
        },
        std::move(schema),
        std::move(describe));
}

void DataSourceConfigService::registerDataSourceType(
    std::string const& typeName,
    DataSourceConstructor constructor,
    nlohmann::json schema,
    DataSourceDescribeFn describe)
{
    if (!constructor) {
        log().warn("Refusing to register NULL constructor for datasource type {}", typeName);
        return;
    }
    std::lock_guard memberAccessLock(memberAccessMutex_);
    constructors_[typeName] = {std::move(constructor), std::move(schema), std::move(describe)};
    schema_.reset();
    validator_.reset();
    log().info("Registered data source type {}.", typeName);
}

nlohmann::json yamlToJson(
    const YAML::Node& yamlNode,
    bool maskSecrets,
    std::unordered_map<std::string, std::string>* maskedSecretMap,
    bool maskCurrentNode)
{
    if (maskSecrets && maskCurrentNode) {
        if (!maskedSecretMap)
            raise("maskedSecretMap must be provided when maskSecrets=true");
        auto value = yamlNode.as<std::string>();
        std::string valueHash;
        picosha2::hash256_hex_string(value, valueHash);
        auto token = fmt::format("MASKED:{}:{}", maskedSecretMap->size(), valueHash);
        maskedSecretMap->emplace(token, value);
        return token;
    }

    if (yamlNode.IsScalar()) {
        try { return yamlNode.as<bool>(); } catch (...) {}
        try { return yamlNode.as<int64_t>(); } catch (...) {}
        try { return yamlNode.as<double>(); } catch (...) {}
        return yamlNode.as<std::string>();
    }

    if (yamlNode.IsSequence()) {
        nlohmann::json arrayJson = nlohmann::json::array();
        for (const auto& elem : yamlNode) {
            arrayJson.push_back(yamlToJson(elem, maskSecrets, maskedSecretMap));
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
                maskSecrets,
                maskedSecretMap,
                isSecretConfigKey(key));
        }
        return objectJson;
    }

    log().warn("Could not convert {} to JSON!", YAML::Dump(yamlNode));
    return {};
}

nlohmann::json parseStructuredDocument(
    std::string_view content,
    std::string_view sourceName)
{
    auto trimmed = content;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
        trimmed.remove_prefix(1);
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
        trimmed.remove_suffix(1);

    if (trimmed.empty()) {
        raise(fmt::format("Structured document `{}` is empty.", sourceName));
    }

    if (trimmed.front() == '{' || trimmed.front() == '[') {
        try {
            return nlohmann::json::parse(trimmed);
        }
        catch (const std::exception&) {
            // Fall back to YAML parsing below. YAML is a superset of JSON,
            // so this keeps error handling consistent for malformed input.
        }
    }

    try {
        return yamlToJson(YAML::Load(std::string(content)), false);
    }
    catch (const YAML::Exception& e) {
        raise(fmt::format(
            "Failed to parse structured document `{}` as YAML or JSON: {}",
            sourceName,
            e.what()));
    }
}

nlohmann::json loadStructuredDocumentFile(std::string const& path)
{
    std::ifstream file(path);
    if (!file) {
        raise(fmt::format("Failed to open structured document file `{}`.", path));
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parseStructuredDocument(buffer.str(), path);
}

YAML::Node jsonToYaml(
    const nlohmann::json& json,
    const std::unordered_map<std::string, std::string>& maskedSecretMap)
{
    YAML::Node node;
    if (json.is_object()) {
        for (auto it = json.begin(); it != json.end(); ++it) {
            if (isSecretConfigKey(it.key()) && it.value().is_string())
            {
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
        node = json.get<int64_t>();
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

nlohmann::json mergeJsonObjects(nlohmann::json base, nlohmann::json const& patch)
{
    if (!base.is_object() || !patch.is_object())
        return patch;
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        auto key = it.key();
        if (base.contains(key) && base[key].is_object() && it->is_object()) {
            base[key] = mergeJsonObjects(base[key], *it);
        }
        else {
            base[key] = *it;
        }
    }
    return base;
}

nlohmann::json DataSourceConfigService::getDataSourceConfigSchema() const
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    if (schema_) {
        return *schema_;
    }

    nlohmann::json typeEnums = nlohmann::json::array();
    nlohmann::json oneOf = nlohmann::json::array();

    for (const auto& [typeName, registration] : constructors_) {
        typeEnums.push_back(typeName);
        nlohmann::json schema = registration.schema_.is_object()
            ? registration.schema_
            : nlohmann::json::object();

        if (!schema.contains("type"))
            schema["type"] = "object";

        auto& properties = schema["properties"];
        if (!properties.is_object())
            properties = nlohmann::json::object();

        properties["type"] = {
            {"type", "string"},
            {"enum", nlohmann::json::array({typeName})}
        };

        if (!properties.contains("enabled"))
            properties["enabled"] = enabledSchema();
        if (!properties.contains("ttl"))
            properties["ttl"] = ttlSchema();
        if (!properties.contains("auth-header"))
            properties["auth-header"] = authHeaderSchema();

        auto& required = schema["required"];
        if (!required.is_array())
            required = nlohmann::json::array();
        if (std::find(required.begin(), required.end(), "type") == required.end())
            required.push_back("type");

        oneOf.push_back(schema);
    }

    nlohmann::json typeProperty = {{"type", "string"}};
    if (!typeEnums.empty()) {
        typeProperty["enum"] = typeEnums;
    }

    nlohmann::json sourcesItems = {
        {"type", "object"},
        {"properties", {
            {"type", typeProperty},
            {"enabled", enabledSchema()},
            {"ttl", ttlSchema()},
            {"auth-header", authHeaderSchema()}
        }},
        {"required", nlohmann::json::array({"type"})},
        {"additionalProperties", true}
    };

    if (!oneOf.empty()) {
        sourcesItems["oneOf"] = oneOf;
    }

    nlohmann::json result = {
        {"type", "object"},
        {"properties", {
            {"sources", nlohmann::json{
                {"type", "array"},
                {"title", "Sources"},
                {"items", sourcesItems}
            }},
        }},
        {"required", nlohmann::json::array({"sources"})},
        {"additionalProperties", false}
    };

    schema_ = result;
    if (schemaPatch_) {
        schema_ = mergeJsonObjects(*schema_, *schemaPatch_);
    }
    validator_ = std::make_unique<nlohmann::json_schema::json_validator>();
    validator_->set_root_schema(*schema_);
    return *schema_;
}

void DataSourceConfigService::setDataSourceConfigSchemaPatch(nlohmann::json schemaPatch)
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    schemaPatch_ = schemaPatch;
    schema_.reset();
    validator_.reset();
}

std::vector<std::string> DataSourceConfigService::topLevelDataSourceConfigKeys() const
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    std::vector<std::string> keys;
    // Ensure that schema_ is properly built.
    auto _ = getDataSourceConfigSchema();
    if (schema_->contains("properties") && (*schema_)["properties"].is_object()) {
        keys.clear();
        for (auto it = (*schema_)["properties"].begin(); it != (*schema_)["properties"].end(); ++it)
            keys.push_back(it.key());
    }
    return keys;
}

DataSourceConfigStats DataSourceConfigService::getDataSourceConfigStats() const
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    return dataSourceConfigStats_;
}

void DataSourceConfigService::registerPublicConfigSection(
    std::string name,
    PublicConfigSectionSerializer serializer)
{
    if (name.empty()) {
        log().warn("Refusing to register public config section with empty name.");
        return;
    }
    if (!serializer) {
        log().warn("Refusing to register public config section {} with NULL serializer.", name);
        return;
    }

    std::lock_guard memberAccessLock(memberAccessMutex_);
    publicConfigSectionSerializers_[std::move(name)] = std::move(serializer);
}

nlohmann::json DataSourceConfigService::getPublicConfigSections(YAML::Node const& fullConfig) const
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    auto result = nlohmann::json::object();
    for (auto const& [name, serializer] : publicConfigSectionSerializers_) {
        nlohmann::json section = nlohmann::json::object();
        if (!serializer) {
            result[name] = std::move(section);
            continue;
        }

        try {
            auto serialized = serializer(fullConfig);
            if (serialized.is_object()) {
                section = std::move(serialized);
            } else {
                log().warn(
                    "Public config section {} serializer returned non-object payload. Replacing with empty object.",
                    name);
            }
        }
        catch (std::exception const& e) {
            log().warn(
                "Public config section {} serializer failed: {}. Replacing with empty object.",
                name,
                e.what());
        }
        catch (...) {
            log().warn(
                "Public config section {} serializer failed with unknown error. Replacing with empty object.",
                name);
        }

        result[name] = std::move(section);
    }
    return result;
}

void DataSourceConfigService::validateDataSourceConfig(nlohmann::json json) const
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    if (!validator_) {
        // Ensure that data source schema/validator are created.
        auto _ = getDataSourceConfigSchema();
        if (!validator_)
            return;
    }
    auto _ = validator_->validate(json);
}

void DataSourceConfigService::validateDataSourceConfig(YAML::Node yaml) const
{
    nlohmann::json filtered = nlohmann::json::object();
    for (auto const& key : topLevelDataSourceConfigKeys()) {
        if (auto n = yaml[key])
            filtered[key] = yamlToJson(n, false);
    }
    validateDataSourceConfig(filtered);
}

void DataSourceConfigService::startConfigFileWatchThread()
{
    namespace fs = std::filesystem;

    if (watchThread_) {
        // If there's already a thread running, wait for it to finish before starting a new one.
        watching_ = false;
        watchThread_->join();
    }

    watching_ = true;
    watchThread_ = std::thread(
        [this, path = configFilePath_]()
        {
            log().debug("Starting watch thread for {}.", path);

            auto toStr = [](fs::file_time_type const& time){
                std::stringstream ss;
                ss << std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
                return ss.str();
            };

            auto modTime = [](std::string const& checkPath) -> std::optional<fs::file_time_type> {
                try {
                    std::error_code e;
                    if (fs::exists(checkPath)) {
                        auto result = fs::last_write_time(checkPath, e);
                        if (!e)
                            return result;
                    }
                }
                catch (...) {
                    // Nothing to do.
                }
                return {};
            };

            auto lastModTime = modTime(path);

            while (watching_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto currentModTime = modTime(path);

                if (currentModTime && !lastModTime) {
                    // The file has appeared since the last check.
                    log().debug("The config file exists now (t={}).", toStr(*currentModTime));
                }
                else if (!currentModTime && lastModTime) {
                    log().debug("The config file disappeared.");
                }
                else if (currentModTime && lastModTime) {
                    if (*currentModTime != *lastModTime) {
                        // The file exists and has been modified since the last check.
                        log().debug(
                            "The config file changed (t0={} vs t1={}).",
                            toStr(*currentModTime),
                            toStr(*lastModTime));
                    }
                    else
                        log().trace(
                            "The config file is unchanged (t0={} vs t1={}).",
                            toStr(*currentModTime),
                            toStr(*lastModTime));
                }

                if (currentModTime) {
                    // Reload by content hash, not only by timestamp. Fast rewrites can
                    // otherwise be missed if the file system timestamp granularity is
                    // coarse or the watch thread starts after the rewrite has landed.
                    loadConfig();
                }

                lastModTime = currentModTime;
            }
        });
}

DataSourceConfigService::DataSourceConfigService() = default;

DataSourceConfigService::~DataSourceConfigService()
{
    end();
}

void DataSourceConfigService::end()
{
    // Signal the watching thread to stop.
    watching_ = false;
    if (watchThread_ && watchThread_->joinable()) {
        watchThread_->join();  // Wait for the thread to finish.
    }
    watchThread_ = {};
}

std::optional<std::string> DataSourceConfigService::getConfigFilePath() const
{
    if (!configFilePath_.empty())
        return configFilePath_;
    return {};
}

void DataSourceConfigService::reset() {
    subscriptions_.clear();
    constructors_.clear();
    currentConfig_.clear();
    configFilePath_.clear();
    schema_.reset();
    validator_.reset();
    publicConfigSectionSerializers_.clear();
    dataSourceConfigStats_ = {};
    end();
}

}  // namespace mapget
