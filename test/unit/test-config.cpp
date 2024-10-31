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

    DataSourceConfigService::get().reset();
    DataSourceConfigService::get().registerDataSourceType(
        "TestDataSource",
        [](const YAML::Node& config) -> DataSource::Ptr
        { return std::make_shared<TestDataSource>(); });

    auto cache = std::make_shared<MemCache>();
    Service service(cache, true);
    log().info(service.info().empty() ? "Info is empty." : service.info()[0].toJson().dump(4));
    REQUIRE(service.info().empty());

    std::promise<void> updatePromise;
    auto updateFuture = updatePromise.get_future();
    auto prepareNextUpdate = [&]()
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        updatePromise = std::promise<void>();
        updateFuture = updatePromise.get_future();
    };

    auto subscription = DataSourceConfigService::get().subscribe(
        [&](auto&&)
        {
            log().debug("Configuration update detected.");
            updatePromise.set_value();
        });

    DataSourceConfigService::get().setConfigFilePath(tempConfigPath.string());

    // Initial empty configuration
    prepareNextUpdate();
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources: []" << std::endl;
    }
    waitForUpdate(updateFuture);
    REQUIRE(service.info().empty());

    // Adding a datasource
    prepareNextUpdate();
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << R"(
        sources:
          - type: TestDataSource
        )" << std::endl;
    }
    waitForUpdate(updateFuture);
    auto dataSourceInfos = service.info();
    REQUIRE(dataSourceInfos.size() == 1);
    REQUIRE(dataSourceInfos[0].mapId_ == "Catan");

    // Removing the datasource
    prepareNextUpdate();
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources: []" << std::endl;
    }
    waitForUpdate(updateFuture);
    REQUIRE(service.info().empty());

    // Cleanup
    fs::remove_all(tempDir);
    // Wait for "The config file disappeared" message :)
    std::this_thread::sleep_for(std::chrono::seconds(2));
    DataSourceConfigService::get().end();
}
