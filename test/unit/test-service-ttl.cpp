#include <catch2/catch_test_macros.hpp>

#include "mapget/service/service.h"
#include "mapget/service/memcache.h"
#include "mapget/model/featurelayer.h"
#include "nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace mapget;
using namespace std::chrono_literals;


namespace
{
constexpr int32_t kTtlTileIdValue = 131073;
constexpr int32_t kTtlPriorityTileIdValue = 131076;
}

class UnsupportedSourceDataLayerError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

DataSourceInfo makeTestTtlDataSourceInfo()
{
    return DataSourceInfo::fromJson(R"(
    {
        "nodeId": "TtlTestNode",
        "mapId": "Tropico",
        "maxParallelJobs": 1,
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
}

class TestTtlDataSource : public DataSource
{
public:
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
        throw UnsupportedSourceDataLayerError("SourceDataLayer not supported in TestTtlDataSource");
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
    DataSourceInfo info_ = makeTestTtlDataSourceInfo();
    std::atomic<int> fillCount_{0};
    std::optional<std::chrono::milliseconds> tileTtlOverride_;
};

class TestLayerTilesRequest : public LayerTilesRequest
{
public:
    using LayerTilesRequest::LayerTilesRequest;
    using LayerTilesRequest::toJson;
};

class MutableInfoDataSource : public DataSource
{
public:
    MutableInfoDataSource()
    {
        layerInfo_ = LayerInfo::fromJson(nlohmann::json{
            {"layerId", "MutableLayer"},
            {"type", "Features"},
            {"featureTypes", nlohmann::json::array({
                nlohmann::json{
                    {"name", "Way"},
                    {"uniqueIdCompositions", nlohmann::json::array({
                        nlohmann::json::array({
                            nlohmann::json{
                                {"partId", "wayId"},
                                {"description", "Synthetic way id."},
                                {"datatype", "U32"},
                            },
                        }),
                    })},
                },
            })},
        });
        info_.nodeId_ = "MutableInfoNode";
        info_.mapId_ = "MutableMap";
        info_.layers_.try_emplace("MutableLayer", layerInfo_);
    }

    DataSourceInfo info() override { return info_; }

    void fill(TileFeatureLayer::Ptr const& tile) override
    {
        tile->newFeature("Way", {{"wayId", int64_t(1)}});
    }

    void fill(TileSourceDataLayer::Ptr const&) override
    {
        throw UnsupportedSourceDataLayerError("SourceDataLayer not supported in MutableInfoDataSource");
    }

    void mutateAdvertisedInfo()
    {
        layerInfo_->layerId_ = "MutatedLayer";
        layerInfo_->featureTypes_.clear();
        info_.layers_.clear();
    }

private:
    std::shared_ptr<LayerInfo> layerInfo_;
    DataSourceInfo info_;
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
            std::vector<TileId>{TileId::fromValue(kTtlTileIdValue)});

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
            std::vector<TileId>{TileId::fromValue(kTtlTileIdValue)});

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
            std::vector<TileId>{TileId::fromValue(kTtlTileIdValue)});

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
            std::vector<TileId>{TileId::fromValue(kTtlTileIdValue)});

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

TEST_CASE("Service info uses detached datasource metadata snapshots", "[Service][DataSourceInfo]")
{
    auto service = Service(std::make_shared<MemCache>(32), false);
    auto dataSource = std::make_shared<MutableInfoDataSource>();
    service.add(dataSource);

    dataSource->mutateAdvertisedInfo();

    auto firstSnapshot = service.info();
    REQUIRE(firstSnapshot.size() == 1);
    REQUIRE(firstSnapshot.front().layers_.size() == 1);
    REQUIRE(firstSnapshot.front().layers_.at("MutableLayer")->layerId_ == "MutableLayer");
    REQUIRE(firstSnapshot.front().layers_.at("MutableLayer")->featureTypes_.size() == 1);

    firstSnapshot.front().layers_.at("MutableLayer")->layerId_ = "CallerMutation";

    auto secondSnapshot = service.info();
    REQUIRE(secondSnapshot.size() == 1);
    REQUIRE(secondSnapshot.front().layers_.at("MutableLayer")->layerId_ == "MutableLayer");
}

TEST_CASE("LayerTilesRequest preserves staged intent in JSON", "[Service][JSON]")
{
    SECTION("Legacy unstaged requests serialize as tileIds")
    {
        auto request = std::make_shared<TestLayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector<TileId>{TileId::fromValue(kTtlTileIdValue)});

        REQUIRE(request->toJson() == nlohmann::json{
            {"mapId", "Tropico"},
            {"layerId", "WayLayer"},
            {"tileIds", nlohmann::json::array({kTtlTileIdValue})},
        });
    }

    SECTION("Single-bucket staged requests serialize as tileIdsByNextStage")
    {
        auto request = std::make_shared<TestLayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector<std::vector<TileId>>{{TileId::fromValue(kTtlTileIdValue)}});

        REQUIRE(request->toJson() == nlohmann::json{
            {"mapId", "Tropico"},
            {"layerId", "WayLayer"},
            {"tileIdsByNextStage", nlohmann::json::array({
                nlohmann::json::array({kTtlTileIdValue})
            })},
        });
    }

    SECTION("Priority tile IDs are serialized as scheduling hints")
    {
        auto request = std::make_shared<TestLayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector<std::vector<TileId>>{{TileId::fromValue(kTtlTileIdValue), TileId::fromValue(kTtlPriorityTileIdValue)}},
            std::vector<TileId>{TileId::fromValue(kTtlPriorityTileIdValue)});

        REQUIRE(request->toJson() == nlohmann::json{
            {"mapId", "Tropico"},
            {"layerId", "WayLayer"},
            {"tileIdsByNextStage", nlohmann::json::array({
                nlohmann::json::array({kTtlTileIdValue, kTtlPriorityTileIdValue})
            })},
            {"priorityTileIds", nlohmann::json::array({kTtlPriorityTileIdValue})},
        });
    }
}
