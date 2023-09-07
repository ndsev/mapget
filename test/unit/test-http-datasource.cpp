#include <catch2/catch_test_macros.hpp>
#include "httplib.h"
#include "mapget/log.h"

#include "mapget/http-datasource/datasource-client.h"
#include "mapget/http-datasource/datasource-server.h"
#include "mapget/http-service/http-client.h"
#include "mapget/http-service/http-service.h"
#include "mapget/model/stream.h"

TEST_CASE("HttpDataSource", "[HttpDataSource]")
{
    mapget::setLogLevel("trace", mapget::log());

    // Create DataSourceInfo.
    auto info = mapget::DataSourceInfo::fromJson(R"(
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
    mapget::DataSourceServer ds(info);
    std::atomic_uint32_t dataSourceRequestCount = 0;
    ds.onTileRequest(
        [&](auto&& tile)
        {
            auto f = tile->newFeature("Way", {{"areaId", "Area42"}, {"wayId", 0}});
            auto g = f->geom()->newGeometry(mapget::GeomType::Line);
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
        auto fetchedInfo =
            mapget::DataSourceInfo::fromJson(nlohmann::json::parse(fetchedInfoJson->body));
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
        mapget::TileLayerStream::Reader reader(
            [&](auto&& mapId, auto&& layerId)
            {
                REQUIRE(mapId == info.mapId_);
                return info.getLayer(std::string(layerId));
            },
            [&](auto&& tile) { receivedTileCount++; });
        reader.read(tileResponse->body);

        REQUIRE(receivedTileCount == 1);
    }

    SECTION("Query mapget HTTP service")
    {
        mapget::HttpService service;
        service.add(std::make_shared<mapget::RemoteDataSource>("localhost", ds.port()));

        service.go();

        SECTION("Query through mapget HTTP service")
        {
            mapget::HttpClient client("localhost", service.port());

            auto receivedTileCount = 0;
            client.request(std::make_shared<mapget::LayerTilesRequest>(
                               "Tropico",
                               "WayLayer",
                               std::vector<mapget::TileId>{{1234, 5678, 9112, 1234}},
                               [&](auto&& tile) { receivedTileCount++; }))
                ->wait();

            REQUIRE(receivedTileCount == 4);
            // One tile requested twice, so the cache was used.
            REQUIRE(dataSourceRequestCount == 3);
        }

        SECTION("Trigger 400 responses")
        {
            mapget::HttpClient client("localhost", service.port());

            auto receivedTileCount = 0;
            auto unknownMapRequest = std::make_shared<mapget::LayerTilesRequest>(
                "UnknownMap",
                "WayLayer",
                std::vector<mapget::TileId>{{1234}},
                [&](auto&& tile) { receivedTileCount++; });
            client.request(unknownMapRequest)->wait();
            REQUIRE(unknownMapRequest->getStatus() == mapget::RequestStatus::NoDataSource);
            REQUIRE(receivedTileCount == 0);

            auto unknownLayerRequest = std::make_shared<mapget::LayerTilesRequest>(
                "Tropico",
                "UnknownLayer",
                std::vector<mapget::TileId>{{1234}},
                [&](auto&& tile) { receivedTileCount++; });
            client.request(unknownLayerRequest)->wait();
            REQUIRE(unknownLayerRequest->getStatus() == mapget::RequestStatus::NoDataSource);
            REQUIRE(receivedTileCount == 0);
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
