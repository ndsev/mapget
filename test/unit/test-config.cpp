#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>

#include "utility.h"
#include "mapget/http-service/cli.h"
#include "mapget/log.h"
#include "mapget/service/config.h"
#include "mapget/service/datasource.h"
#include "mapget/service/memcache.h"
#include "mapget/service/service.h"

namespace fs = std::filesystem;
using namespace mapget;

struct TestDataSource : public DataSource
{
    DataSourceInfo info() override
    {
        return DataSourceInfo::fromJson(R"(
        {
            "mapId": "Catan",
            "layers": {}
        }
        )"_json);
    };

    void fill(TileFeatureLayer::Ptr const&) override {};
    void fill(TileSourceDataLayer::Ptr const&) override {};
};

void waitForUpdate(std::future<void>& future)
{
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        throw std::runtime_error("Timeout waiting for configuration update.");
    }
}

template<typename Predicate>
void waitForCondition(Predicate pred, std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > timeout) {
            throw std::runtime_error("Timeout waiting for condition.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

TEST_CASE("Mapget Config", "[MapgetConfig]")
{
    auto tempDir = fs::temp_directory_path() / test::generateTimestampedDirectoryName("mapget_test_config");
    fs::create_directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";

    SECTION("Bad Config")
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources: [" << std::endl;
        REQUIRE(mapget::runFromCommandLine({std::string("--config"), tempConfigPath.string()}) == 1);
    }

    SECTION("Good Config")
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << R"(
        sources:
          - type: TestDataSource
        )" << std::endl;
        REQUIRE(mapget::runFromCommandLine({std::string("--config"), tempConfigPath.string()}, false) == 0);
    }
}

TEST_CASE("Datasource Config", "[DataSourceConfig]")
{
    setLogLevel("trace", log());

    auto tempDir = fs::temp_directory_path() / test::generateTimestampedDirectoryName("mapget_test_ds_config");
    fs::create_directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";

    log().info("Created temp directory at: {}", tempDir.string());
    log().info("Config file path: {}", tempConfigPath.string());

    DataSourceConfigService::get().reset();
    DataSourceConfigService::get().registerDataSourceType(
        "TestDataSource",
        [](const YAML::Node&) -> DataSource::Ptr
        { return std::make_shared<TestDataSource>(); });

    auto cache = std::make_shared<MemCache>();
    Service service(cache, true);
    log().info(service.info().empty() ? "Info is empty." : service.info()[0].toJson().dump(4));
    REQUIRE(service.info().empty());

    std::atomic<bool> updateReceived{false};
    std::promise<void> updatePromise;
    auto updateFuture = updatePromise.get_future();
    auto prepareNextUpdate = [&]()
    {
        updateReceived = false;
        updatePromise = std::promise<void>();
        updateFuture = updatePromise.get_future();
    };

    auto subscription = DataSourceConfigService::get().subscribe(
        [&](auto&&)
        {
            log().debug("Configuration update detected.");
            if (!updateReceived.exchange(true)) {
                updatePromise.set_value();
            }
        });

    // Initial empty configuration
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        if (!out.is_open()) {
            log().error("Failed to open config file for writing");
            FAIL("Could not open config file");
        }
        out << "sources: []" << std::endl;
        out.close();
        
        if (!fs::exists(tempConfigPath)) {
            log().error("Config file was not created");
            FAIL("Config file does not exist after writing");
        }
        
        log().info("Written initial empty config, file size: {} bytes", fs::file_size(tempConfigPath));
    }

    prepareNextUpdate();
    DataSourceConfigService::get().loadConfig(tempConfigPath.string());
    waitForUpdate(updateFuture);
    waitForCondition([&service]() { return service.info().empty(); });
    REQUIRE(service.info().empty());

    // Adding a datasource
    prepareNextUpdate();
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources:\n  - type: TestDataSource\n";
        out.close();
    }
    waitForUpdate(updateFuture);
    waitForCondition([&service]() { return service.info().size() == 1; });
    auto dataSourceInfos = service.info();
    REQUIRE(dataSourceInfos.size() == 1);
    REQUIRE(dataSourceInfos[0].mapId_ == "Catan");

    // Removing the datasource
    prepareNextUpdate();
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources: []" << std::endl;
        out.close();
    }
    waitForUpdate(updateFuture);
    waitForCondition([&service]() { return service.info().empty(); });
    REQUIRE(service.info().empty());

    // Cleanup
    fs::remove_all(tempDir);
    DataSourceConfigService::get().end();
}
