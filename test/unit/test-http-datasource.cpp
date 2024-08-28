#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include "httplib.h"
#include "mapget/log.h"

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
        service.add(std::make_shared<RemoteDataSource>("localhost", ds.port()));

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
    auto tempDir = fs::temp_directory_path() / test::generateTimestampedDirectoryName("mapget_test");
    fs::create_directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";

    // Setting up the server and client.
    HttpService service;
    service.go();
    REQUIRE(service.isRunning() == true);
    httplib::Client cli("localhost", service.port());

    // Set up the config file.
    std::ofstream configFile(tempConfigPath);
    configFile << "initial: config";
    configFile.close();
    DataSourceConfigService::get().setConfigFilePath(tempConfigPath.string());

    SECTION("Get Configuration - Not Enabled") {
        auto res = cli.Get("/config");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 403);
    }

    SECTION("Get Configuration - Success") {
        setConfigEndpointEnabled(true);
        auto res = cli.Get("/config");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 200);
        REQUIRE(res->body == "initial: config");
    }

    SECTION("Post Configuration - Missing Datasources") {
        std::string newConfig = "true";
        auto res = cli.Post("/config", newConfig, "text/plain");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 500);
    }

    SECTION("Post Configuration - Datasource Entry") {
        std::string newConfig = "sources: {type: TestDataSource}";
        auto res = cli.Post("/config", newConfig, "text/plain");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 200);
    }

    service.stop();
    REQUIRE(service.isRunning() == false);

    // Clean up the test configuration file if necessary
    fs::remove(tempConfigPath);
}
