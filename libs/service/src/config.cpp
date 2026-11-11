
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <chrono>
#include <cctype>
#include <unordered_set>
#include <nlohmann/json-schema.hpp>
#include <fmt/format.h>

#include "picosha2.h"
#include "config.h"
#include "mapget/log.h"

namespace mapget
{
namespace {

/** Encode YAML structure without lossy scalar conversion or map presentation order. */
nlohmann::json configStructure(YAML::Node const& node)
{
    // Quoting changes the non-specific tag (! versus ?), not yaml-cpp's scalar
    // value. Explicit application/type tags remain part of constructor input.
    auto tag = node.Tag();
    if (tag == "!" || tag == "?") {
        tag.clear();
    }
    auto result = nlohmann::json::array({static_cast<int>(node.Type()), std::move(tag)});
    if (node.IsScalar()) {
        result.push_back(node.Scalar());
    }
    else if (node.IsSequence()) {
        for (auto const& child : node) {
            result.push_back(configStructure(child));
        }
    }
    else if (node.IsMap()) {
        auto fields = nlohmann::json::array();
        for (auto const& field : node) {
            fields.push_back(nlohmann::json::array(
                {configStructure(field.first), configStructure(field.second)}));
        }
        auto sorted = fields;
        std::sort(
            sorted.begin(),
            sorted.end(),
            [](auto const& a, auto const& b) { return a[0] < b[0]; });
        // Duplicate YAML keys make order observable to readers. Only normalize
        // ordinary mappings with distinct keys; keep ambiguous input conservative.
        auto duplicate = std::adjacent_find(
            sorted.begin(),
            sorted.end(),
            [](auto const& a, auto const& b) { return a[0] == b[0]; });
        result.push_back(duplicate == sorted.end() ? std::move(sorted) : std::move(fields));
    }
    return result;
}

nlohmann::json ttlSchema()
{
    return {
        {"type", "integer"},
        {"title", "TTL (seconds)"},
        {"minimum", 0},
        {"description",
         "Fallback lifetime in seconds for successful nonempty tiles/objects. "
         "0 = infinite; omitted = mapget.serve.ttl. Explicit payload TTLs win. "
         "Errors and empty source layers instead use error-tile-ttl "
         "and empty-tile-ttl (per-source overrides or mapget.serve defaults)."}};
}

/** Shared source options, independent of any provider's constructor schema. */
nlohmann::json retryTtlSchema(std::string const& kind)
{
    return {
        {"type", "integer"},
        {"minimum", 0},
        {"maximum",
         std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds::max())
             .count()},
        {"description",
         "Maximum " + kind +
             " tile lifetime in seconds. Omitted = inherit "
             "mapget.serve." +
             kind +
             "-tile-ttl; 0 disables expiry-based retries, overriding "
             "the producer TTL. Positive values preserve shorter producer lifetimes."}};
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

nlohmann::json authClaimsSchema()
{
    return {
        {"type", "object"},
        {"title", "Required identity claims"},
        {"description", "JSON-pointer keys with exact string or array-member requirements. All must match, in addition to auth-header."},
        {"propertyNames", {{"pattern", "^(/([^~]|~[01])*)+$"}}},
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

/**
 * Build a display-only name for a catalog row that has no authoritative
 * DataSourceInfo yet. A configured map ID wins; otherwise top-level scalars
 * not matched by isSecretConfigKey are concatenated in YAML order. That key
 * heuristic does not establish that the resulting display name is secret-free.
 */
[[nodiscard]] std::string dataSourceDisplayName(
    YAML::Node const& descriptor,
    uint32_t configIndex)
{
    std::string suffix;
    if (auto mapId = scalarString(descriptor, "mapId"); mapId && !mapId->empty()) {
        suffix = std::move(*mapId);
    }
    else if (descriptor.IsMap()) {
        for (auto const& item : descriptor) {
            if (!item.first.IsScalar() || !item.second.IsScalar()) {
                continue;
            }
            auto const key = item.first.as<std::string>();
            if (isSecretConfigKey(key)) {
                continue;
            }
            auto const value = item.second.as<std::string>();
            if (value.empty()) {
                continue;
            }
            if (!suffix.empty()) {
                suffix += '-';
            }
            suffix += value;
        }
    }

    return suffix.empty()
        ? fmt::format("datasource-{}", configIndex)
        : fmt::format("datasource-{}-{}", configIndex, suffix);
}

[[nodiscard]] std::unordered_map<std::string, std::regex> parseAuthHeaderAlternatives(YAML::Node const& descriptor)
{
    std::unordered_map<std::string, std::regex> result;
    auto const options = descriptor["auth-header"];
    if (!options)
        return result;
    if (!options.IsMap())
        throw std::invalid_argument("auth-header must be a map of header names to regular expressions.");
    for (auto authOption : options) {
        auto key = authOption.first.as<std::string>();
        auto value = authOption.second.as<std::string>();
        if (!addAuthHeaderRegexMatchOption(result, key, std::regex(value)))
            throw std::invalid_argument("auth-header names must be unique ignoring case.");
    }
    return result;
}

AuthClaimRequirements parseAuthClaims(YAML::Node const& descriptor)
{
    AuthClaimRequirements result;
    auto const claims = descriptor["auth-claims"];
    if (!claims)
        return result;
    if (!claims.IsMap())
        throw std::invalid_argument("auth-claims must be a map of JSON pointers to strings.");
    for (auto const& entry : claims) {
        if (!entry.first.IsScalar() || !entry.second.IsScalar())
            throw std::invalid_argument("auth-claims requires JSON-pointer keys and string values.");
        auto pointer = nlohmann::json::json_pointer(entry.first.as<std::string>());
        if (pointer.empty() || !result.emplace(std::move(pointer), entry.second.as<std::string>()).second)
            throw std::invalid_argument("auth-claims requires unique nonempty JSON pointers.");
    }
    return result;
}

void applyGenericDataSourceOptions(DataSource& dataSource, YAML::Node const& descriptor)
{
    auto readTtl = [&](char const* name)
    {
        auto seconds = descriptor[name].as<int64_t>();
        auto maxSeconds =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds::max())
                .count();
        if (seconds < 0 || seconds > maxSeconds) {
            throw std::runtime_error(
                std::string(name) + " must be non-negative and fit a millisecond duration.");
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds(seconds));
    };
    if (descriptor["ttl"])
        dataSource.setTtl(readTtl("ttl"));
    if (descriptor["error-tile-ttl"])
        dataSource.setErrorTileTtl(readTtl("error-tile-ttl"));
    if (descriptor["empty-tile-ttl"])
        dataSource.setEmptyTileTtl(readTtl("empty-tile-ttl"));

    for (auto const& [key, value] : parseAuthHeaderAlternatives(descriptor)) {
        dataSource.requireAuthHeaderRegexMatchOption(key, value);
    }
    for (auto const& [pointer, value] : parseAuthClaims(descriptor)) {
        dataSource.requireAuthClaim(pointer.to_string(), value);
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
    if (!successCallback) return nullptr;
    return subscribeWithReload(
        [successCallback](auto const& nodes, bool) { successCallback(nodes); }, errorCallback);
}

std::unique_ptr<DataSourceConfigService::Subscription> DataSourceConfigService::subscribeWithReload(
    std::function<void(std::vector<YAML::Node> const&, bool)> const& successCallback,
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
    // Replay accepted state and its current rejection, including an accepted empty catalog.
    if (hasAcceptedConfig_) {
        log().debug("Triggering immediate callback for subscription {} with {} config nodes", 
                    sub->id_, currentConfig_.size());
        successCallback(currentConfig_, false);
    }
    if (lastConfigError_ && errorCallback) errorCallback(*lastConfigError_);
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
    {
        auto configMutationLock = lockConfigMutation();
        configFilePath_ = path;

        // Reconcile synchronously before starting the polling thread so subscribers
        // can enqueue initial construction without waiting for the first poll.
        reloadConfig(true);
    }

    if (startWatchThread)
        startConfigFileWatchThread();
}

std::string DataSourceConfigService::configContentFingerprint(std::string const& content) const
{
    return picosha2::hash256_hex_string(
        nlohmann::json::array({configFilePath_, content, schemaRevision_}).dump());
}

void DataSourceConfigService::reloadConfig(bool retryFailedSources)
{
    auto configMutationLock = lockConfigMutation();
    std::lock_guard memberAccessLock(memberAccessMutex_);
    std::optional<std::string> error;

    log().trace("loadConfig() called, configFilePath: {}", configFilePath_);

    try {
        std::ifstream file(configFilePath_);
        if (!file) {
            // A read failure must not suppress recovery when the same bytes reappear.
            lastConfigSHA256_.clear();
            log().trace("Config file does not exist or cannot be opened: {}", configFilePath_);
            throw YAML::Exception(YAML::Mark::null_mark(), "The file does not exist.");
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        if (file.bad()) {
            lastConfigSHA256_.clear();
            throw YAML::Exception(YAML::Mark::null_mark(), "Could not read the configuration file.");
        }
        auto content = buffer.str();
        auto sha256 = configContentFingerprint(content);

        log().trace("Config file SHA256: {}, last SHA256: {}", sha256, lastConfigSHA256_);

        if (!retryFailedSources && sha256 == lastConfigSHA256_)
        {
            log().trace("Config file unchanged. No need to reload.");
            return;
        }

        lastConfigSHA256_ = std::move(sha256);
        YAML::Node config = YAML::Load(content);
        {
            validateDataSourceConfig(config);
            auto sharedConfig = YAML::Clone(config);
            sharedConfig.remove("sources");
            picosha2::hash256_hex_string(
                nlohmann::json::array({configFilePath_, configStructure(sharedConfig)}).dump(),
                dataSourceContextFingerprint_);
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
            hasAcceptedConfig_ = true;
            lastConfigError_.reset();
            log().debug("Notifying {} subscribers", subscriptions_.size());
            for (const auto& [subId, subCb] : subscriptions_) {
                log().debug("Calling subscriber {}", subId);
                subCb.success_(currentConfig_, retryFailedSources);
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
    catch (const std::exception& configError)
    {
        // Semantic validators (for example regexes and JSON pointers) use their
        // own exception types. A rejected file must not terminate the watcher.
        error = fmt::format("Failed to load YAML config {}: {}", configFilePath_, configError.what());
    }

    if (error && error != lastConfigError_) {
        lastConfigError_ = error;
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
            auto message = fmt::format(
                "Datasource type '{}' is not supported by this build. Automatic retries are disabled.", type);
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
    result.sourceId = scalarString(descriptor, "sourceId")
        .value_or(fmt::format("datasource-{}", configIndex));
    if (result.sourceId.empty()) {
        result.sourceId = fmt::format("datasource-{}", configIndex);
    }
    result.type = scalarString(descriptor, "type").value_or("");
    result.displayName = dataSourceDisplayName(descriptor, configIndex);

    try {
        result.addOn = descriptor["addOn"].as<bool>(false);
    }
    catch (std::exception const&) {
        result.addOn = false;
    }

    // Invalid gates must not publish an unrestricted initializing/failed catalog entry.
    result.authHeaderAlternatives = parseAuthHeaderAlternatives(descriptor);
    result.requiredClaims = parseAuthClaims(descriptor);

    return result;
}

std::string DataSourceConfigService::dataSourceConfigFingerprint(YAML::Node const& descriptor) const
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    std::string fingerprint;
    auto type = scalarString(descriptor, "type").value_or("");
    auto registration = constructors_.find(type);
    picosha2::hash256_hex_string(
        nlohmann::json::array({dataSourceContextFingerprint_, configStructure(descriptor),
            registration == constructors_.end() ? uint64_t{0} : registration->second.revision_}).dump(),
        fingerprint);
    return fingerprint;
}

void DataSourceConfigService::registerDataSourceType(
    std::string const& typeName,
    LegacyDataSourceConstructor constructor,
    nlohmann::json schema)
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
        std::move(schema));
}

void DataSourceConfigService::registerDataSourceType(
    std::string const& typeName,
    DataSourceConstructor constructor,
    nlohmann::json schema)
{
    if (!constructor) {
        log().warn("Refusing to register NULL constructor for datasource type {}", typeName);
        return;
    }
    std::lock_guard memberAccessLock(memberAccessMutex_);
    constructors_[typeName] = {std::move(constructor), std::move(schema), ++schemaRevision_};
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
            // A leading brace/bracket can also start YAML flow syntax. Try the
            // YAML parser after JSON rejects it; conversion uses YAML scalar coercion.
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
        node = YAML::Node(YAML::NodeType::Map);
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
        // Empty containers must remain containers (not YAML null), e.g. sources: [].
        node = YAML::Node(YAML::NodeType::Sequence);
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
        properties["error-tile-ttl"] = retryTtlSchema("error");
        properties["empty-tile-ttl"] = retryTtlSchema("empty");
        if (!properties.contains("auth-header"))
            properties["auth-header"] = authHeaderSchema();
        properties["auth-claims"] = authClaimsSchema();

        auto& required = schema["required"];
        if (!required.is_array())
            required = nlohmann::json::array();
        if (std::find(required.begin(), required.end(), "type") == required.end())
            required.push_back("type");

        oneOf.push_back(schema);
    }

    nlohmann::json typeProperty = {{"type", "string"}, {"minLength", 1}};

    nlohmann::json sourcesItems = {
        {"type", "object"},
        {"properties",
         {{"type", typeProperty},
          {"mapId", {{"type", "string"}}},
          {"addOn", {{"type", "boolean"}}},
          {"sourceId",
           {
               {"type", "string"},
               {"minLength", 1},
           }},
          {"enabled", enabledSchema()},
          {"ttl", ttlSchema()},
          {"error-tile-ttl", retryTtlSchema("error")},
          {"empty-tile-ttl", retryTtlSchema("empty")},
          {"auth-header", authHeaderSchema()},
          {"auth-claims", authClaimsSchema()}}},
        {"required", nlohmann::json::array({"type"})},
        {"additionalProperties", true}};

    if (!oneOf.empty()) {
        // Only unavailable types use the generic descriptor contract. Known types
        // must still satisfy their registered (possibly closed) schema. Keep this
        // branch without a type enum so editors offer only registered types.
        oneOf.push_back({{"properties", {{"type", {{"not", {{"enum", typeEnums}}}}}}}});
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
    ++schemaRevision_;
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

void DataSourceConfigService::registerPublicConfigFieldWriter(
    std::string path,
    PublicConfigFieldWriter writer)
{
    if (path.empty() || !writer) {
        log().warn("Refusing to register an invalid public config field writer.");
        return;
    }
    std::lock_guard memberAccessLock(memberAccessMutex_);
    publicConfigFieldWriters_[std::move(path)] = std::move(writer);
}

DataSourceConfigService::PublicConfigWriteResult
DataSourceConfigService::applyPublicConfigFieldWrite(
    std::string const& path,
    YAML::Node& fullConfig,
    nlohmann::json const& requestedValue) const
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    auto const writer = publicConfigFieldWriters_.find(path);
    if (writer == publicConfigFieldWriters_.end() || !writer->second) {
        return {.error = "No writer is registered for public config field '" + path + "'."};
    }
    try {
        return writer->second(fullConfig, requestedValue);
    }
    catch (std::exception const& error) {
        return {.error = error.what()};
    }
    catch (...) {
        return {.error = "The public config writer failed with an unknown error."};
    }
}

bool DataSourceConfigService::hasPublicConfigFieldWriter(std::string const& path) const
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    return publicConfigFieldWriters_.contains(path);
}

std::optional<std::string> DataSourceConfigService::getConfigFileRevision() const
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    if (configFilePath_.empty()) {
        return std::nullopt;
    }
    std::ifstream file(configFilePath_, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream content;
    content << file.rdbuf();
    std::string revision;
    picosha2::hash256_hex_string(content.str(), revision);
    return revision;
}

void DataSourceConfigService::acknowledgePublicConfigWrite(
    std::string const& completeFileContents)
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    lastConfigSHA256_ = configContentFingerprint(completeFileContents);
}

std::unique_lock<std::recursive_mutex> DataSourceConfigService::lockConfigMutation() const
{
    return std::unique_lock<std::recursive_mutex>{configMutationMutex_};
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
    for (auto const& source : json.at("sources")) {
        auto const descriptor = jsonToYaml(source);
        (void)parseAuthHeaderAlternatives(descriptor);
        (void)parseAuthClaims(descriptor);
    }
}

void DataSourceConfigService::validateDataSourceConfig(YAML::Node yaml) const
{
    // YAML-to-JSON conversion would otherwise discard duplicate gate keys.
    for (auto const& source : yaml["sources"]) {
        std::unordered_set<std::string> gates;
        if (source.IsMap()) {
            for (auto const& field : source) {
                if (!field.first.IsScalar()) continue;
                auto key = field.first.Scalar();
                if ((key == "auth-header" || key == "auth-claims") && !gates.insert(key).second)
                    throw std::invalid_argument("Datasource authorization gate keys must be unique.");
            }
        }
        (void)parseAuthHeaderAlternatives(source);
        (void)parseAuthClaims(source);
    }
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
                    reloadConfig();
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
    dataSourceContextFingerprint_.clear();
    lastConfigSHA256_.clear();
    lastConfigError_.reset();
    hasAcceptedConfig_ = false;
    schemaRevision_ = 0;
    schemaPatch_.reset();
    schema_.reset();
    validator_.reset();
    publicConfigSectionSerializers_.clear();
    publicConfigFieldWriters_.clear();
    dataSourceConfigStats_ = {};
    end();
}

}  // namespace mapget
