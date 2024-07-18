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
#include "datasource.h"
#include "yaml-cpp/yaml.h"

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
     * @param callback Function to call with the current set of service config nodes.
     * @return Unique pointer to a Subscription object.
     */
    std::unique_ptr<Subscription> subscribe(
        std::function<void(std::vector<YAML::Node> const& serviceConfigNodes)> const& callback);

    /**
     * Sets the path to the YAML configuration file to watch.
     * @param path The file path to the YAML configuration file.
     */
    void setConfigFilePath(std::string const& path);

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
     */
    void registerDataSourceType(
        std::string const& typeName,
        std::function<DataSource::Ptr(YAML::Node const& arguments)> constructor);

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
     * Loads the configuration from the file.
     */
    void loadConfig();

    /**
     * Starts watching the configuration file for changes.
     * @param path The file path to the YAML configuration file.
     */
    void restartFileWatchThread();

    // Path to the configuration file.
    std::string configFilePath_;

    // Map of subscription IDs to their respective callback functions.
    std::map<uint32_t, std::function<void(std::vector<YAML::Node> const&)>> subscriptions_;

    // Map of data source type names to their respective constructor functions.
    std::map<std::string, std::function<DataSource::Ptr(YAML::Node const&)>> constructors_;

    // Current configuration nodes.
    std::vector<YAML::Node> currentConfig_;

    // Next available subscription ID.
    uint32_t nextSubscriptionId_ = 0;

    // Atomic flag to control the file watching thread.
    std::atomic<bool> watching_ = false;

    // Thread which is watching the config file changed-timestamp.
    std::optional<std::thread> watchThread_;

    // Mutex to ensure that currentConfig_ and subscriptions_ are safely accessed.
    std::recursive_mutex memberAccessMutex_;
};

}  // namespace mapget
