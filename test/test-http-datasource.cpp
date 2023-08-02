#include <catch2/catch_test_macros.hpp>
#include "httplib.h"
#include "mapget/log.h"

#include "mapget/http-datasource/datasource-server.h"
#include "mapget/http-datasource/datasource-client.h"
#include "mapget/http-service/http-service.h"
#include "mapget/http-service/http-client.h"
#include "mapget/model/stream.h"

using namespace mapget;

TEST_CASE("HttpDataSource", "[HttpDataSource]")
{
    // Log all messages to the console if MAPGET_LOG_FILE is not specified.
    if (getenv("MAPGET_LOG_FILE") == nullptr) {
        setenv("MAPGET_LOG_LEVEL", "trace", 1);
    }

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
            }
        }
    }
    )"_json);

    // Initialize a DataSource.
    DataSourceServer ds(info);
    auto dataSourceRequestCount = 0;
    ds.onTileRequest([&](auto&& tile) {
        auto f = tile->newFeature("Way", {{"areaId", "Area42"}, {"wayId", 0}});
        auto g = f->geom()->newGeometry(GeomType::Line);
        g->append({42., 11});
        g->append({42., 12});
        ++dataSourceRequestCount;
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
        auto fetchedInfo = DataSourceInfo::fromJson(nlohmann::json::parse(fetchedInfoJson->body));
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
            [&](auto&& tile) { receivedTileCount++; });
        reader.read(tileResponse->body);

        REQUIRE(receivedTileCount == 1);
    }

    SECTION("Stop the data source")
    {
        ds.stop();
        REQUIRE(ds.isRunning() == false);
    }

    SECTION("Wait for data source")
    {
        auto waitThread = std::thread([&]{ds.waitForSignal();});
        ds.stop();
        waitThread.join();
        REQUIRE(ds.isRunning() == false);
    }

    SECTION("Contact through HTTP service")
    {
        HttpService service;
        service.add(std::make_shared<RemoteDataSource>("localhost", ds.port()));
        service.go();

        HttpClient client("localhost", service.port());

        auto receivedTileCount = 0;
        client.request(std::make_shared<Request>(
            "Tropico",
            "WayLayer",
            std::vector<TileId>{{1234, 5678, 9112, 1234}},
            [&](auto&& tile) { receivedTileCount++; }
        ))->wait();

        REQUIRE(receivedTileCount == 4);
        REQUIRE(dataSourceRequestCount == 3); // One tile requested twice, so the cache was used.
        service.stop();
    }
}
