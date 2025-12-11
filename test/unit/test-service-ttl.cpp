#include <catch2/catch_test_macros.hpp>

#include "mapget/service/service.h"
#include "mapget/service/memcache.h"
#include "mapget/model/featurelayer.h"
#include "nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace mapget;
using namespace std::chrono_literals;

class TestTtlDataSource : public DataSource
{
public:
    TestTtlDataSource()
        : info_(DataSourceInfo::fromJson(R"(
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
        )"_json))
    {}

    DataSourceInfo info() override
    {
        return info_;
    }

    void fill(TileFeatureLayer::Ptr const& tile) override
    {
        ++fillCount_;
        if (tileTtlOverride_) {
            tile->setTtl(tileTtlOverride_);
        }
        tile->setIdPrefix({{"areaId", "Area"}});
        tile->newFeature("Way", {{"wayId", 1}});
    }

    void fill(TileSourceDataLayer::Ptr const&) override
    {
        throw std::runtime_error("SourceDataLayer not supported in TestTtlDataSource");
    }

    void setTileTtlOverride(std::optional<std::chrono::milliseconds> ttl)
    {
        tileTtlOverride_ = ttl;
    }

    int fillCount() const
    {
        return fillCount_.load();
    }

private:
    DataSourceInfo info_;
    std::atomic<int> fillCount_{0};
    std::optional<std::chrono::milliseconds> tileTtlOverride_;
};

TEST_CASE("Service TTL behavior", "[Service][TTL]")
{
    auto cache = std::make_shared<MemCache>(1024);

    SECTION("Global TTL default applied when datasource TTL is not set")
    {
        const auto globalTtl = 10ms;
        Service service(cache, false, globalTtl);
        auto ds = std::make_shared<TestTtlDataSource>();
        service.add(ds);

        auto request1 = std::make_shared<LayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector<TileId>{TileId(12345)});

        TileFeatureLayer::Ptr tile1;
        request1->onFeatureLayer([&](TileFeatureLayer::Ptr const& tile) { tile1 = tile; });

        REQUIRE(service.request({request1}));
        request1->wait();

        REQUIRE(tile1);
        REQUIRE(tile1->ttl().has_value());
        REQUIRE(tile1->ttl().value() == globalTtl);
        REQUIRE(ds->ttl() == std::nullopt);
        REQUIRE(ds->fillCount() == 1);

        // Second request shortly after should hit the cache (TTL not expired).
        auto request2 = std::make_shared<LayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector<TileId>{TileId(12345)});

        TileFeatureLayer::Ptr tile2;
        request2->onFeatureLayer([&](TileFeatureLayer::Ptr const& tile) { tile2 = tile; });

        REQUIRE(service.request({request2}));
        request2->wait();

        REQUIRE(tile2);
        REQUIRE(ds->fillCount() == 1);

        // Wait long enough for TTL to expire and request again.
        std::this_thread::sleep_for(20ms);

        auto request3 = std::make_shared<LayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector<TileId>{TileId(12345)});

        TileFeatureLayer::Ptr tile3;
        request3->onFeatureLayer([&](TileFeatureLayer::Ptr const& tile) { tile3 = tile; });

        REQUIRE(service.request({request3}));
        request3->wait();

        REQUIRE(tile3);
        REQUIRE(ds->fillCount() == 2);
    }

    SECTION("Datasource-specific TTL overrides global default")
    {
        const auto globalTtl = 10ms;
        const auto datasourceTtl = 50ms;

        Service service(cache, false, globalTtl);
        auto ds = std::make_shared<TestTtlDataSource>();
        ds->setTtl(datasourceTtl);
        service.add(ds);

        auto request = std::make_shared<LayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector<TileId>{TileId(12345)});

        TileFeatureLayer::Ptr tile;
        request->onFeatureLayer([&](TileFeatureLayer::Ptr const& t) { tile = t; });

        REQUIRE(service.request({request}));
        request->wait();

        REQUIRE(tile);
        REQUIRE(tile->ttl().has_value());
        REQUIRE(tile->ttl().value() == datasourceTtl);
        REQUIRE(ds->fillCount() == 1);
    }
}

