#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <semaphore>

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

TEST_CASE("Load Config From File", "[DataSourceConfig]")
{
    setLogLevel("trace", log());

    auto tempDir = fs::temp_directory_path() / generateTimestampedDirectoryName("mapget_test");
    fs::create_directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";

    DataSourceConfigService::get().registerDataSourceType(
        "TestDataSource",
        [](const YAML::Node& config) -> DataSource::Ptr
        { return std::make_shared<TestDataSource>(); });

    auto cache = std::make_shared<MemCache>();
    Service service(cache, true);
    REQUIRE(service.info().empty());

    std::binary_semaphore semaphore(0);

    auto subscription = DataSourceConfigService::get().subscribe(
        [&](auto&&) {
            log().debug("Release the semaphore!");
            semaphore.release();
        });

    DataSourceConfigService::get().setConfigFilePath(tempConfigPath.string());

    // Initial empty configuration
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources: []" << std::endl;
    }
    REQUIRE(semaphore.try_acquire_for(std::chrono::seconds(5)));
    REQUIRE(service.info().empty());
    log().debug("Gate passed!");

    // Adding a datasource
    std::this_thread::sleep_for(std::chrono::seconds(1));
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << R"(
        sources:
          - type: TestDataSource
        )" << std::endl;
    }
    REQUIRE(semaphore.try_acquire_for(std::chrono::seconds(5)));
    log().debug("Gate passed!");

    auto dataSourceInfos = service.info();
    REQUIRE(dataSourceInfos.size() == 1);
    REQUIRE(dataSourceInfos[0].mapId_ == "Catan");

    // Removing the datasource
    std::this_thread::sleep_for(std::chrono::seconds(1));
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources: []";
    }
    REQUIRE(semaphore.try_acquire_for(std::chrono::seconds(5)));
    REQUIRE(service.info().empty());
    log().debug("Gate passed!");

    // Cleanup
    fs::remove_all(tempDir);
    // Wait for "The config file disappeared" message :)
    std::this_thread::sleep_for(std::chrono::seconds(1));
    DataSourceConfigService::get().end();
}
