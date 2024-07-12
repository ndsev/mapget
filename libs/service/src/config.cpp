
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#include "config.h"
#include "mapget/log.h"

namespace mapget
{

DataSourceConfigService& DataSourceConfigService::get()
{
    static DataSourceConfigService instance;
    return instance;
}

DataSourceConfigService::Subscription::~Subscription()
{
    DataSourceConfigService::get().unsubscribe(id_);
}

std::unique_ptr<DataSourceConfigService::Subscription> DataSourceConfigService::subscribe(
    std::function<void(std::vector<YAML::Node> const&)> const& callback)
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

void DataSourceConfigService::unsubscribe(uint32_t id)
{
    std::lock_guard memberAccessLock(memberAccessMutex_);
    subscriptions_.erase(id);
}

void DataSourceConfigService::setConfigFilePath(std::string const& path)
{
    configFilePath_ = path;
    restartFileWatchThread();
}

void DataSourceConfigService::loadConfig()
{
    try {
        YAML::Node config = YAML::LoadFile(configFilePath_);
        if (auto sourcesNode = config["sources"]) {
            std::lock_guard memberAccessLock(memberAccessMutex_);
            currentConfig_.clear();
            for (auto const& node : sourcesNode)
                currentConfig_.push_back(node);
            for (const auto& [subId, subCb] : subscriptions_) {
                log().debug("Calling subscriber {}", subId);
                subCb(currentConfig_);
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

DataSource::Ptr DataSourceConfigService::makeDataSource(YAML::Node const& descriptor)
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
        log().error("No constructor registered for datasource type: {}", type);
        return nullptr;
    }
    log().error("A YAML datasource descriptor is missing the `type` key!");
    return nullptr;
}

void DataSourceConfigService::registerDataSourceType(
    std::string const& typeName,
    std::function<DataSource::Ptr(YAML::Node const&)> constructor)
{
    if (!constructor) {
        log().warn("Refusing to register NULL constructor for datasource type {}", typeName);
        return;
    }
    std::lock_guard memberAccessLock(memberAccessMutex_);
    constructors_[typeName] = std::move(constructor);
    log().info("Registered data source type {}.", typeName);
}

void DataSourceConfigService::restartFileWatchThread()
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
            auto toStr = [](auto&& time){
                std::stringstream ss;
                ss << time;
                return ss.str();
            };

            log().debug("Starting watch thread for {}.", path);

            fs::file_time_type lastModTime;
            bool fileExisted = fs::exists(path);

            if (fileExisted) {
                lastModTime = fs::last_write_time(path);
                log().debug("The config file exists (t={}).", toStr(lastModTime));
                loadConfig();
            }
            else {
                log().debug("The config file does not exist yet.");
            }

            while (watching_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (fs::exists(path)) {
                    if (!fileExisted) {
                        // The file has appeared since the last check.
                        lastModTime = fs::last_write_time(path);
                        log().debug("The config file exists now (t={}).", toStr(lastModTime));
                        fileExisted = true;
                        loadConfig();
                    }
                    else {
                        auto currentModTime = fs::last_write_time(path);

                        if (currentModTime != lastModTime) {
                            // The file exists and has been modified since the last check.
                            log().debug(
                                "The config file changed ({} vs {}).",
                                toStr(currentModTime),
                                toStr(lastModTime));
                            lastModTime = currentModTime;
                            loadConfig();
                        }
                        else {
                            log().trace(
                                "The config file is unchanged ({} vs {}).",
                                toStr(currentModTime),
                                toStr(lastModTime));
                        }
                    }
                }
                else if (fileExisted) {
                    log().debug("The config file disappeared.");
                    fileExisted = false;
                }
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
}

}  // namespace mapget
