#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <atomic>

#include "mapget/service/service.h"
#include "mapget/service/datasource.h"
#include "mapget/service/config.h"
#include "mapget/log.h"
#include "mapget/service/memcache.h"

namespace fs = std::filesystem;
using namespace mapget;

struct TestDataSource : public DataSource
{
    DataSourceInfo info() override {
        return DataSourceInfo::fromJson(R"(
        {
            "mapId": "Catan",
            "layers": {}
        }
        )"_json);
    };

    void fill(TileFeatureLayer::Ptr const& featureTile) override {};
};

std::string generateTimestampedDirectoryName(const std::string& baseName) {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return baseName + "_" + std::to_string(millis);
}

void waitForUpdate(std::atomic<bool>& flag, std::chrono::seconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (!flag.load() && std::chrono::steady_clock::now() - start < timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!flag.load()) {
        throw std::runtime_error("Timeout waiting for configuration update.");
    }
    flag.store(false);  // Reset flag for next wait
}

TEST_CASE("Load Config From File", "[DataSourceConfig]")
{
    setLogLevel("trace", log());

    auto tempDir = fs::temp_directory_path() / generateTimestampedDirectoryName("mapget_test");
    fs::create_directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";

    DataSourceConfigService::get().registerDataSourceType(
        "TestDataSource",
        [](const YAML::Node& config) -> DataSource::Ptr { return std::make_shared<TestDataSource>(); });

    auto cache = std::make_shared<MemCache>();
    Service service(cache, true);
    REQUIRE(service.info().empty());

    std::atomic<bool> updateOccurred(false);

    auto subscription = DataSourceConfigService::get().subscribe(
        [&](auto&&) {
            log().debug("Configuration update detected.");
            updateOccurred.store(true);
        });

    DataSourceConfigService::get().setConfigFilePath(tempConfigPath.string());

    // Initial empty configuration
    std::this_thread::sleep_for(std::chrono::seconds(2));
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources: []" << std::endl;
    }
    waitForUpdate(updateOccurred, std::chrono::seconds(5));
    REQUIRE(service.info().empty());

    // Adding a datasource
    std::this_thread::sleep_for(std::chrono::seconds(2));
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << R"(
        sources:
          - type: TestDataSource
        )" << std::endl;
    }
    waitForUpdate(updateOccurred, std::chrono::seconds(5));
    auto dataSourceInfos = service.info();
    REQUIRE(dataSourceInfos.size() == 1);
    REQUIRE(dataSourceInfos[0].mapId_ == "Catan");

    // Removing the datasource
    std::this_thread::sleep_for(std::chrono::seconds(2));
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources: []";
    }
    waitForUpdate(updateOccurred, std::chrono::seconds(5));
    REQUIRE(service.info().empty());

    // Cleanup
    fs::remove_all(tempDir);
    // Wait for "The config file disappeared" message :)
    std::this_thread::sleep_for(std::chrono::seconds(2));
    DataSourceConfigService::get().end();
}
