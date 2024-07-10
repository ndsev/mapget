
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>

#include "config.h"
#include "mapget/log.h"

namespace mapget
{

DataSourceConfig& DataSourceConfig::instance()
{
    static DataSourceConfig instance;
    return instance;
}

DataSourceConfig::Subscription::~Subscription()
{
    DataSourceConfig::instance().unsubscribe(id_);
}

std::unique_ptr<DataSourceConfig::Subscription>
DataSourceConfig::subscribe(std::function<void(std::vector<YAML::Node> const&)> const& callback)
{
    if (!callback) {
        log().warn("Refusing to register config subscription with NULL callback.");
        return nullptr;
    }

    std::lock_guard memberAccessLock(memberAccessMutex_);
    auto sub = std::make_unique<Subscription>();
    sub->id_ = nextSubscriptionId_++;
    subscriptions_[sub->id_] = callback;
    // Optionally, trigger the callback with the current configuration immediately
    if (!currentConfig_.empty()) {
        callback(currentConfig_);
    }
    return sub;
}

void DataSourceConfig::unsubscribe(uint32_t id)
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    subscriptions_.erase(id);
}

void DataSourceConfig::setConfigFilePath(std::string const& path) {
    configFilePath_ = path;
    loadConfig();  // Initial load
    restartFileWatchThread();
}

void DataSourceConfig::loadConfig()
{
    try {
        YAML::Node config = YAML::LoadFile(configFilePath_);
        if (auto sourcesNode = config["sources"]) {
            std::lock_guard memberAccessLock(memberAccessMutex_);
            currentConfig_.clear();
            for (auto const& node : sourcesNode)
                currentConfig_.push_back(node);
            for (const auto& subscriber : subscriptions_) {
                subscriber.second(currentConfig_);
            }
        }
        else {
            log().warn("The config file {} does not have a sources node.");
        }
    }
    catch (const YAML::Exception& e) {
        log().error("Failed to load YAML config {}: {}", configFilePath_, e.what());
    }
}

DataSource::Ptr DataSourceConfig::instantiate(YAML::Node const& descriptor)
{
    if (auto typeNode = descriptor["type"]) {
        std::lock_guard memberAccessLock(memberAccessMutex_);
        auto type = typeNode.as<std::string>();
        auto it = constructors_.find(type);
        if (it != constructors_.end()) {
            if (auto result = it->second(descriptor)) {
                return result;
            }
            log().error("Datasource constructor for type {} returned NULL.", type);
            return nullptr;
        }
        log().error("No constructor registered for datasource type: ", type);
        return nullptr;
    }
    log().error("A YAML datasource descriptor is missing the `type` key!");
    return nullptr;
}

void DataSourceConfig::registerConstructor(
    std::string const& typeName,
    std::function<DataSource::Ptr(YAML::Node const&)> constructor)
{
    if (!constructor) {
        log().warn("Refusing to register NULL constructor for datasource type {}", typeName);
        return;
    }
    std::lock_guard memberAccessLock(memberAccessMutex_);
    constructors_[typeName] = std::move(constructor);
}

void DataSourceConfig::restartFileWatchThread()
{
    namespace fs = std::filesystem;

    if (watchThread_) {
        // If there's already a thread running, wait for it to finish before starting a new one.
        watching_ = false;
        watchThread_->join();
        watching_ = true;
    }

    watchThread_ = std::thread(
        [this, path= configFilePath_]()
        {
            auto lastModTime = fs::last_write_time(path);
            while (watching_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto currentModTime = fs::last_write_time(path);
                if (currentModTime != lastModTime) {
                    loadConfig();
                    lastModTime = currentModTime;
                }
            }
        });
}

DataSourceConfig::DataSourceConfig() = default;

DataSourceConfig::~DataSourceConfig() {
    // Signal the watching thread to stop.
    watching_ = false;
    if (watchThread_ && watchThread_->joinable()) {
        watchThread_->join();  // Wait for the thread to finish.
    }
}

}  // namespace mapget
