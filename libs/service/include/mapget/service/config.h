#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
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
     * Registers a constructor for a given data source type.
     * @param typeName The name of the data source type.
     * @param constructor The constructor function to call for this data source type.
     * @param schema Config JSON schema for the received YAML node.
     */
    void registerDataSourceType(
        std::string const& typeName,
        std::function<DataSource::Ptr(YAML::Node const& arguments)> constructor,
        nlohmann::json schema = {});

    /** Get (and lazily build) JSON schema that describes registered datasource types. */
    [[nodiscard]] nlohmann::json schema() const;

    /**
     * Validate the given config object against the config schema. Note: When validating
     * YAML, only the top-level nodes mentioned in the JSON schema are validated.
     */
    void validate(nlohmann::json json) const;
    void validate(YAML::Node yaml) const;

    /** Merge the provided patch into the current schema and refresh validator. */
    void setSchemaPatch(nlohmann::json schemaPatch);

    /** Top-level JSON keys allowed by current schema (properties keys). */
    [[nodiscard]] std::vector<std::string> topLevelJsonKeys() const;

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
    struct SubscriptionCallbacks {
        std::function<void(std::vector<YAML::Node> const& serviceConfigNodes)> success_;
        std::function<void(std::string const& error)> error_;
    };
    std::unordered_map<uint32_t, SubscriptionCallbacks> subscriptions_;

    struct DataSourceRegistration {
        std::function<DataSource::Ptr(YAML::Node const&)> constructor_;
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

    // Atomic flag to control the file watching thread.
    std::atomic<bool> watching_ = false;

    // Thread which is watching the config file changed-timestamp.
    std::optional<std::thread> watchThread_;

    // Mutex to ensure that currentConfig_ and subscriptions_ are safely accessed.
    mutable std::recursive_mutex memberAccessMutex_;
};

/** Convert YAML to JSON, with optional secret masking. */
nlohmann::json yamlToJson(
    const YAML::Node& yamlNode,
    std::unordered_map<std::string, std::string>* maskedSecretMap = nullptr,
    bool maskSecret = false);

/** Convert JSON to YAML, resolving masked secrets if provided. */
YAML::Node jsonToYaml(
    const nlohmann::json& json,
    const std::unordered_map<std::string, std::string>& maskedSecretMap = {});

}  // namespace mapget
