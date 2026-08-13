#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <unordered_map>
#include "datasource.h"
#include "yaml-cpp/yaml.h"
#include "nlohmann/json.hpp"
#include "nlohmann/json-schema.hpp"

namespace mapget
{

struct DataSourceConfigStats {
    size_t configured = 0;
    size_t enabled = 0;
    size_t disabled = 0;
    size_t constructionFailed = 0;
};

/** Progress/cancellation hooks passed to config-created datasource constructors. */
struct DataSourceInitContext {
    /** Report human-readable constructor progress without requiring a ready DataSource instance. */
    std::function<void(std::string)> setStatusMessage;

    /** Report optional constructor progress as a percentage in the inclusive range 0..100. */
    std::function<void(std::optional<float>)> setProgress;

    /** Allow long-running constructors to stop work after config reloads or service shutdown. */
    std::function<bool()> isCancelled;
};

/** Cheap, config-derived datasource facts available before construction starts. */
struct DataSourceDescriptor {
    /** Preserves `mapviewer.yaml` order and gives clear diagnostics for config-entry failures. */
    uint32_t configIndex = 0;

    /** Stable catalog identity used only for lifecycle and optional request assertions. */
    std::string sourceId;

    /** Needed for placeholder UI and error messages before a `DataSource` exists. */
    std::string type;

    /** Display-only placeholder used until construction provides authoritative `DataSourceInfo`. */
    std::string displayName;

    /** Prevents initializing/failed add-on sources from being shown as standalone maps. */
    bool addOn = false;

    /** Preserves current `/sources` authorization behavior before a ready DataSource exists. */
    AuthHeaderRegexMap authHeaderAlternatives;
};


    /**
 * Singleton class that watches a particular YAML config file path.
 * The config YAML must have a top-level `sources:` key, which hosts
 * a list of datasource descriptors. Each descriptor must have a `type:`
 * key, to describe the datasource constructor that is supposed to be called.
 * The whole descriptor will be passed into the lambda that is registered
 * as the constructor for the given type name when calling makeDataSource().
 * Services will call subscribe() to be notified about the currently
 * active set of services from the config.
 */
class DataSourceConfigService
{
public:
    using PublicConfigSectionSerializer =
        std::function<nlohmann::json(YAML::Node const& fullConfig)>;
    using DataSourceConstructor =
        std::function<DataSource::Ptr(YAML::Node const& arguments, DataSourceInitContext& initContext)>;
    using LegacyDataSourceConstructor =
        std::function<DataSource::Ptr(YAML::Node const& arguments)>;

    /**
     * Gets the singleton instance of the DataSourceConfig class.
     * @return Reference to the singleton instance.
     */
    static DataSourceConfigService& get();

    /**
     * Clear subscriptions, constructor, current config content and path,
     * stop the file watch thread.
     */
    void reset();

    /**
     * Class representing a subscription to the configuration changes.
     */
    class Subscription
    {
    public:
        /**
         * Destructor that ensures unsubscription.
         */
        ~Subscription();
        Subscription(Subscription const& other) = delete;
        Subscription(Subscription&& other) = default;
        Subscription& operator= (Subscription const& other) = delete;

    private:
        explicit Subscription(uint32_t id);
        uint32_t id_ = 0;

        friend std::unique_ptr<Subscription> std::make_unique<Subscription>(uint32_t&&);
        friend class DataSourceConfigService;
    };

    /**
     * Subscribes to configuration changes.
     * The callback will be triggered once immediately, then whenever
     * the config file path or content changes.
     * @param successCallback Function to call with the current (new) set of service config nodes.
     * @param errorCallback Function to call when applying the config failed.
     * @return Unique pointer to a Subscription object.
     */
    std::unique_ptr<Subscription> subscribe(
        std::function<void(std::vector<YAML::Node> const& serviceConfigNodes)> const& successCallback,
        std::function<void(std::string const& error)> const& errorCallback={});

    /**
     * Loads the configuration from the file.
     * @param path The file path to the YAML configuration file.
     * @param startWatchThread True to automatically reload of changes to config file occur.
     */
    void loadConfig(std::string const& path, bool startWatchThread = true);

    /**
     * Get the path to the YAML configuration file (if set).
     */
    std::optional<std::string> getConfigFilePath() const;

    /**
     * Instantiates a data source based on the provided descriptor.
     * @param descriptor The YAML node containing the data source descriptor.
     * @return Shared pointer to the instantiated data source, or nullptr if instantiation failed.
     */
    DataSource::Ptr makeDataSource(YAML::Node const& descriptor);

    /**
     * Instantiates a data source with progress/cancellation hooks for statusful startup.
     */
    DataSource::Ptr makeDataSource(YAML::Node const& descriptor, DataSourceInitContext& initContext);

    /**
     * Extract cheap, non-networked catalog metadata from a datasource config entry.
     */
    [[nodiscard]] DataSourceDescriptor describeDataSource(YAML::Node const& descriptor, uint32_t configIndex) const;

    /**
     * Registers a constructor for a given data source type.
     * @param typeName The name of the data source type.
     * @param constructor The constructor function to call for this data source type.
     * @param schema Config JSON schema for the received YAML node.
     */
    void registerDataSourceType(
        std::string const& typeName,
        LegacyDataSourceConstructor constructor,
        nlohmann::json schema = {});

    /**
     * Registers a constructor that can report startup progress and observe cancellation.
     */
    void registerDataSourceType(
        std::string const& typeName,
        DataSourceConstructor constructor,
        nlohmann::json schema = {});

    /** Get (and lazily build) JSON schema that describes registered datasource types. */
    [[nodiscard]] nlohmann::json getDataSourceConfigSchema() const;

    /**
     * Validate the given config object against the config schema. Note: When validating
     * YAML, only the top-level nodes mentioned in the JSON schema are validated.
     */
    void validateDataSourceConfig(nlohmann::json json) const;
    void validateDataSourceConfig(YAML::Node yaml) const;

    /** Merge the provided patch into the current schema and refresh validator. */
    void setDataSourceConfigSchemaPatch(nlohmann::json schemaPatch);

    /** Top-level JSON keys allowed by current schema (properties keys). */
    [[nodiscard]] std::vector<std::string> topLevelDataSourceConfigKeys() const;

    /** Latest datasource config statistics from the most recent load. */
    [[nodiscard]] DataSourceConfigStats getDataSourceConfigStats() const;

    /**
     * Register an additional public top-level config section for GET /config.
     * The serializer receives the full YAML config document.
     */
    void registerPublicConfigSection(
        std::string name,
        PublicConfigSectionSerializer serializer);

    /**
     * Serialize registered public config sections.
     * Every registered key is present in the result.
     */
    [[nodiscard]] nlohmann::json getPublicConfigSections(YAML::Node const& fullConfig) const;

    /**
     * Call this to stop the config file watching thread.
     */
    void end();

private:
    // Private constructor to enforce the singleton pattern.
    DataSourceConfigService();

    // Destructor to clean up resources.
    ~DataSourceConfigService();

    /**
     * Unsubscribes a subscription based on its ID.
     * @param id The subscription ID to remove.
     */
    void unsubscribe(uint32_t id);

    /**
     * Starts watching the configuration file for changes. Will load
     * the config immmediately if not yet occurred.
     */
    void startConfigFileWatchThread();

    /**
     * Uses @see configFilePath_ to load the configuration.
     */
    void loadConfig();

    // Path to the configuration file.
    std::string configFilePath_;

    // Last config loaded.
    std::string lastConfigSHA256_;

    // Map of subscription IDs to their respective callback functions.
    // Note: This MUST be an ORDERED map, because the subscription call order is important.
    struct SubscriptionCallbacks {
        std::function<void(std::vector<YAML::Node> const& serviceConfigNodes)> success_;
        std::function<void(std::string const& error)> error_;
    };
    std::map<uint32_t, SubscriptionCallbacks> subscriptions_;

    struct DataSourceRegistration {
        DataSourceConstructor constructor_;
        nlohmann::json schema_;
    };

    // Map of data source type names to their respective constructor functions.
    std::unordered_map<std::string, DataSourceRegistration> constructors_;

    // Current configuration nodes.
    std::vector<YAML::Node> currentConfig_;

    // Next available subscription ID.
    uint32_t nextSubscriptionId_ = 0;

    // Optional schema and validator used when loading configs.
    std::optional<nlohmann::json> schemaPatch_;
    mutable std::optional<nlohmann::json> schema_;
    mutable std::unique_ptr<nlohmann::json_schema::json_validator> validator_;
    std::map<std::string, PublicConfigSectionSerializer> publicConfigSectionSerializers_;
    DataSourceConfigStats dataSourceConfigStats_;

    // Atomic flag to control the file watching thread.
    std::atomic<bool> watching_ = false;

    // Thread which polls the config file for content changes.
    std::optional<std::thread> watchThread_;

    // Mutex to ensure that currentConfig_ and subscriptions_ are safely accessed.
    mutable std::recursive_mutex memberAccessMutex_;
};

/** Convert YAML to JSON, with optional secret masking. */
nlohmann::json yamlToJson(
    const YAML::Node& yamlNode,
    bool maskSecrets,
    std::unordered_map<std::string, std::string>* maskedSecretMap = nullptr,
    bool maskCurrentNode = false);

/** Parse a YAML or JSON document from a string buffer into JSON. */
[[nodiscard]] nlohmann::json parseStructuredDocument(
    std::string_view content,
    std::string_view sourceName = "<memory>");

/** Load and parse a YAML or JSON document from disk into JSON. */
[[nodiscard]] nlohmann::json loadStructuredDocumentFile(std::string const& path);

/** Convert JSON to YAML, resolving masked secrets if provided. */
YAML::Node jsonToYaml(
    const nlohmann::json& json,
    const std::unordered_map<std::string, std::string>& maskedSecretMap = {});

}  // namespace mapget
