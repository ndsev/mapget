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
    ds.onLocateRequest(
        [&](mapget::LocateRequest const& request) -> std::optional<mapget::LocateResponse>
        {
            REQUIRE(request.mapId_ == "Tropico");
            REQUIRE(request.typeId_ == "Way");
            REQUIRE(request.featureId_ == mapget::KeyValuePairs{{"wayId", 0}});

            mapget::LocateResponse response(request);
            response.tileKey_.layerId_ = "WayLayer";
            response.tileKey_.tileId_.value_ = 1;
            return response;
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
        mapget::LocateResponse responseParsed(nlohmann::json::parse(response->body));
        REQUIRE(responseParsed.tileKey_.mapId_ == "Tropico");
        REQUIRE(responseParsed.tileKey_.layer_ == mapget::LayerType::Features);
        REQUIRE(responseParsed.tileKey_.layerId_ == "WayLayer");
        REQUIRE(responseParsed.tileKey_.tileId_.value_ == 1);
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
            mapget::LocateResponse responseParsed(responseJsonList[0]);
            REQUIRE(responseParsed.tileKey_.mapId_ == "Tropico");
            REQUIRE(responseParsed.tileKey_.layer_ == mapget::LayerType::Features);
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
