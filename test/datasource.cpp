#include <catch2/catch_test_macros.hpp>

#include "mapget/datasource/datasource.h"
#include "httplib.h"
#include "mapget/model/stream.h"

using namespace mapget;

TEST_CASE("DataSource", "[DataSource]")
{
    // Create DataSourceInfo
    auto info = DataSourceInfo::fromJson(R"(
    {
        "mapId": "GarlicChickenMap",
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
    // configure info

    // Initialize a DataSource
    DataSource ds(info);

    ds.onTileRequest([](TileFeatureLayer& tile) {
        auto f = tile.newFeature("Way", {{"areaId", "Area42"}, {"wayId", 0}});
        auto g = f->geom()->newGeometry(GeomType::Line);
        g->append({42., 11});
        g->append({42., 12});
    });

    // Launch the DataSource on a separate thread
    ds.go();

    // Ensure the DataSource is running
    REQUIRE(ds.isRunning() == true);

    SECTION("Fetch /info")
    {
        // Initialize an httplib client
        httplib::Client cli("localhost", ds.port());

        // Send a GET info request
        auto fetchedInfoJson = cli.Get("/info");
        auto fetchedInfo = DataSourceInfo::fromJson(nlohmann::json::parse(fetchedInfoJson->body));
        REQUIRE(fetchedInfo.toJson() == info.toJson());
    }

    SECTION("Fetch /tile")
    {
        // Initialize an httplib client
        httplib::Client cli("localhost", ds.port());

        // Send a GET tile request
        auto tileResponse = cli.Get("/tile?layer=WayLayer&tileId=1");

        // Check that the response is OK
        REQUIRE(tileResponse != nullptr);
        REQUIRE(tileResponse->status == 200);

        // Check the response body for expected content
        auto receivedTileCount = 0;
        TileLayerStream::Reader reader(
            [&](auto&& mapId, auto&& layerId)
            {
                REQUIRE(mapId == info.mapId_);
                return info.layers_[std::string(layerId)];
            },
            [&](auto&& tile) { receivedTileCount++; });
        reader.read({tileResponse->body.begin(), tileResponse->body.end()});

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
}
