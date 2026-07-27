#include <atomic>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <utility>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

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

struct NamedTestDataSource : public DataSource
{
    explicit NamedTestDataSource(std::string mapId) : mapId_(std::move(mapId)) {}

    DataSourceInfo info() override
    {
        return DataSourceInfo::fromJson(nlohmann::json::object({
            {"mapId", mapId_},
            {"layers", nlohmann::json::object()},
        }));
    }

    void fill(TileFeatureLayer::Ptr const&) override {};
    void fill(TileSourceDataLayer::Ptr const&) override {};

    std::string mapId_;
};

void syncFile(const fs::path& path)
{
    log().trace("Syncing file: {}", path.string());
#ifdef _WIN32
    // Windows doesn't have fsync, but closing the file should be sufficient
#else
    // On Unix systems, use fsync to ensure data is written to disk
    int fd = open(path.c_str(), O_RDONLY);
    if (fd != -1) {
        fsync(fd);
        close(fd);
    }
#endif
    // Small delay to ensure file system visibility
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void waitForUpdate(std::future<void>& future)
{
    log().trace("Waiting for update future...");
    auto status = future.wait_for(std::chrono::seconds(30));
    if (status != std::future_status::ready) {
        log().error("Timeout waiting for configuration update after 30 seconds");
        throw std::runtime_error("Timeout waiting for configuration update.");
    }
    log().trace("Update future ready");
}

template<typename Predicate>
void waitForCondition(Predicate pred, std::chrono::milliseconds timeout = std::chrono::seconds(30))
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
        out.flush();
        out.close();
        syncFile(tempConfigPath);
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
        [&](auto&& configNodes)
        {
            log().debug("Configuration update detected. Nodes count: {}", configNodes.size());
            if (!updateReceived.exchange(true)) {
                log().trace("Setting promise value for config update");
                updatePromise.set_value();
            } else {
                log().trace("Update already received, skipping promise set");
            }
        });

    // Initial empty configuration
    {
        log().trace("Writing initial empty config to: {}", tempConfigPath.string());
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        if (!out.is_open()) {
            log().error("Failed to open config file for writing");
            FAIL("Could not open config file");
        }
        out << "sources: []" << std::endl;
        out.flush();
        out.close();
        
        syncFile(tempConfigPath);
        
        if (!fs::exists(tempConfigPath)) {
            log().error("Config file was not created");
            FAIL("Config file does not exist after writing");
        }
        
        log().info("Written initial empty config, file size: {} bytes", fs::file_size(tempConfigPath));
    }

    prepareNextUpdate();
    log().debug("About to call loadConfig with path: {}", tempConfigPath.string());
    DataSourceConfigService::get().loadConfig(tempConfigPath.string());
    log().debug("loadConfig returned, waiting for update");
    waitForUpdate(updateFuture);
    log().debug("Update received, waiting for service info to be empty");
    waitForCondition([&service]() { return service.info().empty(); });
    REQUIRE(service.info().empty());

    // Adding a datasource
    prepareNextUpdate();
    {
        log().trace("Writing config with TestDataSource");
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources:\n  - type: TestDataSource\n";
        out.flush();
        out.close();
        syncFile(tempConfigPath);
    }
    waitForUpdate(updateFuture);
    waitForCondition([&service]() { return service.info().size() == 1; });
    auto dataSourceInfos = service.info();
    REQUIRE(dataSourceInfos.size() == 1);
    REQUIRE(dataSourceInfos[0].mapId_ == "Catan");

    // Removing the datasource
    prepareNextUpdate();
    {
        log().trace("Writing empty config to remove datasources");
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources: []" << std::endl;
        out.flush();
        out.close();
        syncFile(tempConfigPath);
    }
    waitForUpdate(updateFuture);
    waitForCondition([&service]() { return service.info().empty(); });
    REQUIRE(service.info().empty());

    // Cleanup
    fs::remove_all(tempDir);
    DataSourceConfigService::get().end();
}

TEST_CASE("Datasource enabled flag is handled generically", "[DataSourceConfig]")
{
    auto tempDir = fs::current_path() / test::generateTimestampedDirectoryName("mapget_test_ds_enabled");
    fs::create_directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";

    DataSourceConfigService::get().reset();
    DataSourceConfigService::get().registerDataSourceType(
        "TestDataSource",
        [](const YAML::Node&) -> DataSource::Ptr
        { return std::make_shared<TestDataSource>(); });

    auto cache = std::make_shared<MemCache>();
    Service service(cache, true);

    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << R"(
sources:
  - type: TestDataSource
    enabled: false
  - type: TestDataSource
    enabled: true
)";
        out.flush();
        out.close();
        syncFile(tempConfigPath);
    }

    DataSourceConfigService::get().loadConfig(tempConfigPath.string());
    waitForCondition([&service]() { return service.info().size() == 1; });

    auto stats = DataSourceConfigService::get().getDataSourceConfigStats();
    REQUIRE(stats.configured == 2);
    REQUIRE(stats.enabled == 1);
    REQUIRE(stats.disabled == 1);

    auto serviceStats = service.getStatistics();
    REQUIRE(serviceStats["datasource-config"]["configured"].get<size_t>() == 2);
    REQUIRE(serviceStats["datasource-config"]["enabled"].get<size_t>() == 1);
    REQUIRE(serviceStats["datasource-config"]["disabled"].get<size_t>() == 1);
    REQUIRE(serviceStats["datasource-config"]["construction-failed"].get<size_t>() == 0);

    fs::remove_all(tempDir);
    DataSourceConfigService::get().end();
}

TEST_CASE("Datasource catalog display names are generic and display-only", "[DataSourceConfig]")
{
    auto& configService = DataSourceConfigService::get();

    auto const configuredMap = configService.describeDataSource(YAML::Load(R"(
type: TestDataSource
mapId: ConfiguredMap
uri: /ignored/when/mapId/is/present
)"), 3);
    REQUIRE(configuredMap.displayName == "datasource-3-ConfiguredMap");

    auto const scalarFallback = configService.describeDataSource(YAML::Load(R"(
type: TestDataSource
uri: filestore:/tmp/Example.Map
serverIndex: 2
apiKey: must-not-leak
auth-header:
  Authorization: "^Bearer .+$"
)"), 4);
    REQUIRE(
        scalarFallback.displayName
        == "datasource-4-TestDataSource-filestore:/tmp/Example.Map-2");
}

TEST_CASE("Datasource catalog tracks config order and async startup status", "[DataSourceConfig]")
{
    auto tempDir = fs::current_path() / test::generateTimestampedDirectoryName("mapget_test_ds_catalog");
    fs::create_directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";

    DataSourceConfigService::get().reset();

    std::promise<void> slowStartedPromise;
    auto slowStarted = slowStartedPromise.get_future().share();
    std::promise<void> releaseSlowPromise;
    auto releaseSlow = releaseSlowPromise.get_future().share();
    std::atomic_bool slowStartedReported{false};

    DataSourceConfigService::get().registerDataSourceType(
        "SlowDataSource",
        [&](const YAML::Node&, DataSourceInitContext& initContext) -> DataSource::Ptr
        {
            initContext.setStatusMessage("Waiting for test release.");
            initContext.setProgress(25.0f);
            if (!slowStartedReported.exchange(true)) {
                slowStartedPromise.set_value();
            }
            while (releaseSlow.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
                if (initContext.isCancelled && initContext.isCancelled()) {
                    return nullptr;
                }
            }
            return std::make_shared<NamedTestDataSource>("SlowMap");
        });

    DataSourceConfigService::get().registerDataSourceType(
        "FastDataSource",
        [](const YAML::Node&, DataSourceInitContext&) -> DataSource::Ptr
        {
            return std::make_shared<NamedTestDataSource>("FastMap");
        });

    DataSourceConfigService::get().registerDataSourceType(
        "FailingDataSource",
        [](const YAML::Node&, DataSourceInitContext& initContext) -> DataSource::Ptr
        {
            initContext.setStatusMessage("Intentional test failure.");
            return nullptr;
        });

    auto cache = std::make_shared<MemCache>();
    Service service(cache, true);

    std::mutex changesMutex;
    std::vector<DataSourceCatalogChange> changes;
    auto sourceChanges = service.subscribeToSourceCatalogChanges(
        [&](DataSourceCatalogChange const& change) {
            std::lock_guard lock(changesMutex);
            changes.push_back(change);
        });

    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << R"(
sources:
  - type: SlowDataSource
    mapId: SlowConfiguredMap
  - type: FailingDataSource
    mapId: FailingConfiguredMap
  - type: FastDataSource
    mapId: FastConfiguredMap
)";
        out.flush();
        out.close();
        syncFile(tempConfigPath);
    }

    DataSourceConfigService::get().loadConfig(tempConfigPath.string());
    REQUIRE(slowStarted.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    waitForCondition([&service]() {
        auto catalog = service.sourceCatalog();
        return catalog.sources.size() == 3
            && catalog.sources[0].status == DataSourceCatalogStatus::Initializing
            && catalog.sources[1].status == DataSourceCatalogStatus::Failed
            && catalog.sources[2].status == DataSourceCatalogStatus::Ready;
    });

    auto catalog = service.sourceCatalog();
    REQUIRE(catalog.sources.size() == 3);
    REQUIRE(catalog.sources[0].descriptor.displayName == "datasource-0-SlowConfiguredMap");
    REQUIRE(catalog.sources[1].descriptor.displayName == "datasource-1-FailingConfiguredMap");
    REQUIRE(catalog.sources[2].descriptor.displayName == "datasource-2-FastConfiguredMap");
    REQUIRE(catalog.sources[0].descriptor.configIndex == 0);
    REQUIRE(catalog.sources[1].descriptor.configIndex == 1);
    REQUIRE(catalog.sources[2].descriptor.configIndex == 2);
    REQUIRE(catalog.sources[0].statusMessage == "Waiting for test release.");
    REQUIRE(catalog.sources[0].progress == std::optional<float>{25.0f});
    REQUIRE(catalog.sources[1].statusMessage == "Intentional test failure.");
    REQUIRE(catalog.sources[2].info.has_value());
    REQUIRE(catalog.sources[2].info->mapId_ == "FastMap");

    auto readyInfos = service.info();
    REQUIRE(readyInfos.size() == 1);
    REQUIRE(readyInfos.front().mapId_ == "FastMap");

    auto blockingCatalogFuture = std::async(std::launch::async, [&service] {
        return service.sourceCatalog({}, true);
    });
    REQUIRE(blockingCatalogFuture.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);

    releaseSlowPromise.set_value();
    REQUIRE(blockingCatalogFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    waitForCondition([&service]() {
        auto catalog = service.sourceCatalog();
        return catalog.sources.size() == 3
            && catalog.sources[0].status == DataSourceCatalogStatus::Ready
            && catalog.sources[1].status == DataSourceCatalogStatus::Failed
            && catalog.sources[2].status == DataSourceCatalogStatus::Ready;
    });

    catalog = service.sourceCatalog();
    auto blockingCatalog = blockingCatalogFuture.get();
    REQUIRE(blockingCatalog.sources.size() == 3);
    REQUIRE(blockingCatalog.sources[0].status == DataSourceCatalogStatus::Ready);
    REQUIRE(catalog.sources[0].info.has_value());
    REQUIRE(catalog.sources[0].info->mapId_ == "SlowMap");
    REQUIRE(service.info().size() == 2);
    {
        std::lock_guard lock(changesMutex);
        REQUIRE_FALSE(changes.empty());
        REQUIRE(std::ranges::any_of(changes, [](auto const& change) {
            return change.sourceUpdate
                && change.sourceUpdate->descriptor.configIndex == 0
                && change.sourceUpdate->statusMessage == "Waiting for test release.";
        }));
        REQUIRE(std::ranges::any_of(changes, [](auto const& change) {
            return change.sourceUpdate
                && change.sourceUpdate->descriptor.configIndex == 0
                && change.sourceUpdate->progress == std::optional<float>{25.0f};
        }));
        REQUIRE(changes.back().revision == service.sourceCatalogRevision());
    }

    fs::remove_all(tempDir);
    DataSourceConfigService::get().end();
}
