#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <thread>
#include <chrono>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif
#include "httplib.h"
#include "mapget/log.h"
#include "nlohmann/json.hpp"

#include "utility.h"
#include "mapget/http-datasource/datasource-client.h"
#include "mapget/http-datasource/datasource-server.h"
#include "mapget/http-service/http-client.h"
#include "mapget/http-service/http-service.h"
#include "mapget/model/stream.h"
#include "mapget/service/config.h"
#include "mapget/http-service/cli.h"

using namespace mapget;
namespace fs = std::filesystem;

TEST_CASE("HttpDataSource", "[HttpDataSource]")
{
    setLogLevel("trace", log());

    // Create DataSourceInfo.
    auto info = DataSourceInfo::fromJson(R"(
    {
        "mapId": "Tropico",
        "layers": {
            "WayLayer": {
                "featureTypes":
                [
                    {
                        "name": "Way",
                        "uniqueIdCompositions":
                        [
                            [
                                {
                                    "partId": "areaId",
                                    "description": "String which identifies the map area.",
                                    "datatype": "STR"
                                },
                                {
                                    "partId": "wayId",
                                    "description": "Globally Unique 32b integer.",
                                    "datatype": "U32"
                                }
                            ]
                        ]
                    }
                ]
            },
            "SourceData-WayLayer": {
                "type": "SourceData"
            }
        }
    }
    )"_json);

    // Initialize a DataSource.
    DataSourceServer ds(info);
    std::atomic_uint32_t dataSourceFeatureRequestCount = 0;
    std::atomic_uint32_t dataSourceSourceDataRequestCount = 0;
    ds.onTileFeatureRequest(
        [&](const auto& tile)
        {
            auto f = tile->newFeature("Way", {{"areaId", "Area42"}, {"wayId", 0}});
            auto g = f->geom()->newGeometry(GeomType::Line);
            g->append({42., 11});
            g->append({42., 12});
            ++dataSourceFeatureRequestCount;
        });
    ds.onTileSourceDataRequest(
        [&](const auto& tile) {
            ++dataSourceSourceDataRequestCount;
        });
    ds.onLocateRequest(
        [&](LocateRequest const& request) -> std::vector<LocateResponse>
        {
            REQUIRE(request.mapId_ == "Tropico");
            REQUIRE(request.typeId_ == "Way");
            REQUIRE(request.featureId_ == KeyValuePairs{{"wayId", 0}});

            LocateResponse response(request);
            response.tileKey_.layerId_ = "WayLayer";
            response.tileKey_.tileId_.value_ = 1;
            return {response};
        });

    // Launch the DataSource on a separate thread.
    ds.go();

    // Ensure the DataSource is running.
    REQUIRE(ds.isRunning() == true);

    SECTION("Fetch /info")
    {
        // Initialize an httplib client.
        httplib::Client cli("localhost", ds.port());

        // Send a GET info request.
        auto fetchedInfoJson = cli.Get("/info");
        auto fetchedInfo =
            DataSourceInfo::fromJson(nlohmann::json::parse(fetchedInfoJson->body));
        REQUIRE(fetchedInfo.toJson() == info.toJson());
    }

    SECTION("Fetch /tile")
    {
        // Initialize an httplib client.
        httplib::Client cli("localhost", ds.port());

        // Send a GET tile request.
        auto tileResponse = cli.Get("/tile?layer=WayLayer&tileId=1");

        // Check that the response is OK.
        REQUIRE(tileResponse != nullptr);
        REQUIRE(tileResponse->status == 200);

        // Check the response body for expected content.
        auto receivedTileCount = 0;
        TileLayerStream::Reader reader(
            [&](auto&& mapId, auto&& layerId)
            {
                REQUIRE(mapId == info.mapId_);
                return info.getLayer(std::string(layerId));
            },
            [&](auto&& tile) {
                REQUIRE(tile->id().layer_ == LayerType::Features);
                receivedTileCount++;
            });
        reader.read(tileResponse->body);

        REQUIRE(receivedTileCount == 1);
    }

    SECTION("Fetch /tile SourceData")
    {
        // Initialize an httplib client.
        httplib::Client cli("localhost", ds.port());

        // Send a GET tile request
        auto tileResponse = cli.Get("/tile?layer=SourceData-WayLayer&tileId=1");

        // Check that the response is OK.
        REQUIRE(tileResponse != nullptr);
        REQUIRE(tileResponse->status == 200);

        // Check the response body for expected content.
        auto receivedTileCount = 0;
        TileLayerStream::Reader reader(
            [&](auto&& mapId, auto&& layerId)
            {
                REQUIRE(mapId == info.mapId_);
                return info.getLayer(std::string(layerId));
            },
            [&](auto&& tile) {
                REQUIRE(tile->id().layer_ == LayerType::SourceData);
                receivedTileCount++;
            });
        reader.read(tileResponse->body);

        REQUIRE(receivedTileCount == 1);
    }

    SECTION("Fetch /locate")
    {
        // Initialize an httplib client.
        httplib::Client cli("localhost", ds.port());

        // Send a POST locate request.
        auto response = cli.Post("/locate", R"({
            "mapId": "Tropico",
            "typeId": "Way",
            "featureId": ["wayId", 0]
        })", "application/json");

        // Check that the response is OK.
        REQUIRE(response != nullptr);
        REQUIRE(response->status == 200);

        // Check the response body for expected content.
        LocateResponse responseParsed(nlohmann::json::parse(response->body)[0]);
        REQUIRE(responseParsed.tileKey_.mapId_ == "Tropico");
        REQUIRE(responseParsed.tileKey_.layer_ == LayerType::Features);
        REQUIRE(responseParsed.tileKey_.layerId_ == "WayLayer");
        REQUIRE(responseParsed.tileKey_.tileId_.value_ == 1);
    }

    SECTION("Query mapget HTTP service")
    {
        auto countReceivedTiles = [](auto& client, auto mapId, auto layerId, auto tiles) {
            auto tileCount = 0;

            auto request = std::make_shared<LayerTilesRequest>(mapId, layerId, tiles);
            request->onFeatureLayer([&](auto&& tile) { tileCount++; });
            //request->onSourceDataLayer([&](auto&& tile) { tileCount++; });

            client.request(request)->wait();
            return std::make_tuple(request, tileCount);
        };

        HttpService service;
        auto remoteDataSource = std::make_shared<RemoteDataSource>("localhost", ds.port());
        service.add(remoteDataSource);

        service.go();

        SECTION("Query through mapget HTTP service")
        {
            HttpClient client("localhost", service.port());

            auto [request, receivedTileCount] = countReceivedTiles(
                client,
                "Tropico",
                "WayLayer",
                std::vector<TileId>{{1234, 5678, 9112, 1234}});

            REQUIRE(receivedTileCount == 4);
            // One tile requested twice, so the cache was used.
            REQUIRE(dataSourceFeatureRequestCount == 3);
        }

        SECTION("Trigger 400 responses")
        {
            HttpClient client("localhost", service.port());

            {
                auto [request, receivedTileCount] = countReceivedTiles(
                    client,
                    "UnknownMap",
                    "WayLayer",
                    std::vector<TileId>{{1234}});
                REQUIRE(request->getStatus() == RequestStatus::NoDataSource);
                REQUIRE(receivedTileCount == 0);
            }

            {
                auto [request, receivedTileCount] = countReceivedTiles(
                    client,
                    "Tropico",
                    "UnknownLayer",
                    std::vector<TileId>{{1234}});
                REQUIRE(request->getStatus() == RequestStatus::NoDataSource);
                REQUIRE(receivedTileCount == 0);
            }
        }

        SECTION("Run /locate through service")
        {
            httplib::Client client("localhost", service.port());

            // Send a POST locate request.
            auto response = client.Post("/locate", R"({
                "requests": [{
                    "mapId": "Tropico",
                    "typeId": "Way",
                    "featureId": ["wayId", 0]
                }]
            })", "application/json");

            // Check that the response is OK.
            REQUIRE(response != nullptr);
            REQUIRE(response->status == 200);

            // Check the response body for expected content.
            auto responseJsonLists = nlohmann::json::parse(response->body)["responses"];
            REQUIRE(responseJsonLists.size() == 1);
            auto responseJsonList = responseJsonLists[0];
            REQUIRE(responseJsonList.size() == 1);
            LocateResponse responseParsed(responseJsonList[0]);
            REQUIRE(responseParsed.tileKey_.mapId_ == "Tropico");
            REQUIRE(responseParsed.tileKey_.layer_ == LayerType::Features);
            REQUIRE(responseParsed.tileKey_.layerId_ == "WayLayer");
            REQUIRE(responseParsed.tileKey_.tileId_.value_ == 1);
        }

        SECTION("Test auth header requirement")
        {
            remoteDataSource->requireAuthHeaderRegexMatchOption(
                "X-USER-ROLE",
                std::regex("\\bTropico-Viewer\\b"));

            HttpClient badClient("localhost", service.port());
            HttpClient goodClient("localhost", service.port(), {{"X-USER-ROLE", "Tropico-Viewer"}});

            // Check sources
            REQUIRE(badClient.sources().empty());
            REQUIRE(goodClient.sources().size() == 1);

            // Try to load tiles with bad client
            {
                auto [request, receivedTileCount] = countReceivedTiles(
                    badClient,
                    "Tropico",
                    "WayLayer",
                    std::vector<TileId>{{1234}});
                REQUIRE(request->getStatus() == RequestStatus::Unauthorized);
                REQUIRE(receivedTileCount == 0);
            }

            // Try to load tiles with good client
            {
                auto [request, receivedTileCount] = countReceivedTiles(
                    goodClient,
                    "Tropico",
                    "WayLayer",
                    std::vector<TileId>{{1234}});
                REQUIRE(request->getStatus() == RequestStatus::Success);
                REQUIRE(receivedTileCount == 1);
            }
        }

        service.stop();
        REQUIRE(service.isRunning() == false);
    }

    SECTION("Wait for data source")
    {
        auto waitThread = std::thread([&] { ds.waitForSignal(); });
        ds.stop();
        waitThread.join();
        REQUIRE(ds.isRunning() == false);
    }

    ds.stop();
    REQUIRE(ds.isRunning() == false);
}

TEST_CASE("Configuration Endpoint Tests", "[Configuration]")
{
    auto tempDir = fs::temp_directory_path() / test::generateTimestampedDirectoryName("mapget_test_http_config");
    fs::create_directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";

    // Setting up the server and client.
    HttpService service;
    service.go();
    REQUIRE(service.isRunning() == true);
    httplib::Client cli("localhost", service.port());

    // Set up the config file.
    DataSourceConfigService::get().reset();
    struct SchemaPatchGuard {
        ~SchemaPatchGuard() {
            DataSourceConfigService::get().setDataSourceConfigSchemaPatch(nlohmann::json::object());
        }
    } schemaPatchGuard;

    // Emulate the CLI-provided config-schema patch so http-settings participates in the auto schema.
    auto schemaPatch = nlohmann::json::parse(R"(
    {
        "properties": {
            "http-settings": {
                "type": "array"
            }
        },
        "required": ["sources", "http-settings"]
    }
    )");
    DataSourceConfigService::get().setDataSourceConfigSchemaPatch(schemaPatch);

    SECTION("Get Configuration - Config File Not Found") {
        DataSourceConfigService::get().loadConfig(tempConfigPath.string());
        auto res = cli.Get("/config");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 404);
        REQUIRE(res->body == "The server does not have a config file.");
    }

    // Create config file for tests that need it
    {
        std::ofstream configFile(tempConfigPath);
        configFile << "sources: []\nhttp-settings: [{'password': 'hunter2'}]";  // Update http-settings to an array.
        configFile.flush();
        configFile.close();
        
        // Ensure file is synced to disk
        #ifndef _WIN32
        int fd = open(tempConfigPath.c_str(), O_RDONLY);
        if (fd != -1) {
            fsync(fd);
            close(fd);
        }
        #endif
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Load the config after file is created
    DataSourceConfigService::get().loadConfig(tempConfigPath.string());
    
    // Give the config watcher time to detect the file
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    SECTION("Get Configuration - Not allowed") {
        setGetConfigEndpointEnabled(false);
        auto res = cli.Get("/config");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 403);
    }

    SECTION("Get Configuration - No Config File Path Set") {
        setGetConfigEndpointEnabled(true);
        DataSourceConfigService::get().loadConfig("");  // Simulate no config path set.
        auto res = cli.Get("/config");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 404);
        REQUIRE(res->body == "The config file path is not set. Check the server configuration.");
    }

    SECTION("Get Configuration - Success") {
        auto res = cli.Get("/config");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 200);
        REQUIRE(res->body.find("sources") != std::string::npos);
        REQUIRE(res->body.find("http-settings") != std::string::npos);

        // Ensure that the password is masked as SHA256.
        REQUIRE(res->body.find("hunter2") == std::string::npos);
        REQUIRE(res->body.find("MASKED:f52fbd32b2b3b86ff88ef6c490628285f482af15ddcb29541f94bcf526a3f6c7") != std::string::npos);
    }

    SECTION("Post Configuration - Not Enabled") {
        setPostConfigEndpointEnabled(false);
        auto res = cli.Post("/config", "", "application/json");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 403);
    }

    SECTION("Post Configuration - Invalid JSON Format") {
        setPostConfigEndpointEnabled(true);
        std::string invalidJson = "this is not valid json";
        auto res = cli.Post("/config", invalidJson, "application/json");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 400);
        REQUIRE(res->body.find("Invalid JSON format") != std::string::npos);
    }

    SECTION("Post Configuration - Missing Sources") {
        std::string newConfig = R"({"http-settings": []})";
        auto res = cli.Post("/config", newConfig, "application/json");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 500);
        REQUIRE(res->body.starts_with("Validation failed"));
    }

    SECTION("Post Configuration - Missing Http Settings") {
        std::string newConfig = R"({"sources": []})";
        auto res = cli.Post("/config", newConfig, "application/json");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 500);
        REQUIRE(res->body.starts_with("Validation failed"));
    }

    SECTION("Post Configuration - Valid JSON Config") {
        std::string newConfig = R"({
            "sources": [{"type": "TestDataSource"}],
            "http-settings": [{"scope": "https://example.com", "password": "MASKED:f52fbd32b2b3b86ff88ef6c490628285f482af15ddcb29541f94bcf526a3f6c7"}]
        })";
        log().set_level(spdlog::level::trace);
        auto res = cli.Post("/config", newConfig, "application/json");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 200);
        REQUIRE(res->body == "Configuration updated and applied successfully.");

        // Check that the password SHA was re-substituted.
        std::ifstream config(*mapget::DataSourceConfigService::get().getConfigFilePath());
        std::stringstream configContentStream;
        configContentStream << config.rdbuf();
        auto configContent = configContentStream.str();
        REQUIRE(configContent.find("hunter2") != std::string::npos);
    }

    service.stop();
    REQUIRE(service.isRunning() == false);

    // Clean up the test configuration files.
    fs::remove(tempConfigPath);
}
