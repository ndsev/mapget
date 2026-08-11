#include <catch2/catch_test_macros.hpp>

#include "mapget/model/featurelayer.h"
#include "mapget/service/memcache.h"
#include "mapget/service/service.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace mapget;
using namespace std::chrono_literals;

namespace
{
constexpr int32_t kTtlTileIdValue = 131073;
constexpr int32_t kTtlPriorityTileIdValue = 131076;
}  // namespace

class UnsupportedSourceDataLayerError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

DataSourceInfo makeTestTtlDataSourceInfo()
{
    return DataSourceInfo::fromJson(R"(
    {
        "stringPoolId": "TtlTestNode",
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
    DataSourceInfo info() override { return info_; }

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

    int fillCount() const { return fillCount_.load(); }

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
            {"featureTypes",
             nlohmann::json::array({
                 nlohmann::json{
                     {"name", "Way"},
                     {"uniqueIdCompositions",
                      nlohmann::json::array({
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
        info_.stringPoolId_ = "MutableInfoNode";
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
        throw UnsupportedSourceDataLayerError(
            "SourceDataLayer not supported in MutableInfoDataSource");
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

/** Blocks datasource fills while recording service-wide concurrent entry. */
class GlobalConcurrencyProbe
{
public:
    /** Enter one blocking operation and update its concurrent peak. */
    void enter()
    {
        std::unique_lock lock(mutex_);
        ++active_;
        ++entered_;
        peak_ = std::max(peak_, active_);
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
        --active_;
    }

    /** Wait until at least `count` operations have entered the probe. */
    bool waitForEntries(size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return entered_ >= count; });
    }

    /** Release every operation currently blocked in enter(). */
    void release()
    {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

    /** Return the highest number of simultaneous operations observed. */
    [[nodiscard]] size_t peak() const
    {
        std::lock_guard lock(mutex_);
        return peak_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    size_t active_ = 0;
    size_t entered_ = 0;
    size_t peak_ = 0;
    bool released_ = false;
};

/** Datasource with a deliberately loose permit limit for testing the global cap. */
class ConcurrencyTestDataSource : public DataSource
{
public:
    /** Construct a datasource with an explicit per-source permit limit. */
    ConcurrencyTestDataSource(
        std::string mapId,
        std::string stringPoolId,
        std::shared_ptr<GlobalConcurrencyProbe> probe,
        int maxParallelJobs = 8)
        : probe_(std::move(probe))
    {
        info_.mapId_ = std::move(mapId);
        info_.stringPoolId_ = std::move(stringPoolId);
        info_.maxParallelJobs_ = maxParallelJobs;
        info_.layers_.try_emplace(
            "Features",
            LayerInfo::fromJson(nlohmann::json{
                {"layerId", "Features"},
                {"type", "Features"},
                {"featureTypes", nlohmann::json::array()},
            }));
    }

    /** Return the synthetic source metadata used for scheduler admission. */
    DataSourceInfo info() override { return info_; }

    /** Block in the shared probe so tests can inspect scheduler concurrency. */
    void fill(TileFeatureLayer::Ptr const&) override { probe_->enter(); }

    /** Reject source-data requests because this fixture serves only features. */
    void fill(TileSourceDataLayer::Ptr const&) override
    {
        throw UnsupportedSourceDataLayerError(
            "SourceDataLayer not supported in ConcurrencyTestDataSource");
    }

private:
    DataSourceInfo info_;
    std::shared_ptr<GlobalConcurrencyProbe> probe_;
};

TEST_CASE("Service uses one configurable worker cap across datasources", "[Service][concurrency]")
{
    auto probe = std::make_shared<GlobalConcurrencyProbe>();
    Service service(std::make_shared<MemCache>(32), false, 0ms, 2);
    service.add(std::make_shared<ConcurrencyTestDataSource>("MapA", "PoolA", probe));
    service.add(std::make_shared<ConcurrencyTestDataSource>("MapB", "PoolB", probe));

    std::vector<TileId> tiles;
    for (int32_t x = 0; x < 4; ++x) {
        tiles.push_back(TileId::fromTileXY(x, 0, 3));
    }
    auto first = std::make_shared<LayerTilesRequest>("MapA", "Features", tiles);
    auto second = std::make_shared<LayerTilesRequest>("MapB", "Features", tiles);
    REQUIRE(service.request({first, second}));

    auto const filledCap = probe->waitForEntries(2, 2s);
    auto const peakBeforeRelease = probe->peak();
    probe->release();
    first->wait();
    second->wait();

    REQUIRE(filledCap);
    REQUIRE(peakBeforeRelease == 2);
    REQUIRE(first->getStatus() == RequestStatus::Success);
    REQUIRE(second->getStatus() == RequestStatus::Success);
    auto const statistics = service.getStatistics(false, false);
    REQUIRE(statistics["workers"]["configured"] == 2);
}

TEST_CASE("Service preserves per-datasource permits below the global cap", "[Service][concurrency]")
{
    auto constrained = std::make_shared<GlobalConcurrencyProbe>();
    auto unconstrained = std::make_shared<GlobalConcurrencyProbe>();
    Service service(std::make_shared<MemCache>(32), false, 0ms, 4);
    service.add(std::make_shared<ConcurrencyTestDataSource>("MapA", "PoolA", constrained, 1));
    service.add(std::make_shared<ConcurrencyTestDataSource>("MapB", "PoolB", unconstrained, 8));

    std::vector<TileId> tiles;
    for (int32_t x = 0; x < 4; ++x) {
        tiles.push_back(TileId::fromTileXY(x, 0, 3));
    }
    auto first = std::make_shared<LayerTilesRequest>("MapA", "Features", tiles);
    auto second = std::make_shared<LayerTilesRequest>("MapB", "Features", tiles);
    REQUIRE(service.request({first, second}));

    auto const constrainedStarted = constrained->waitForEntries(1, 2s);
    auto const otherWorkersStayedBusy = unconstrained->waitForEntries(3, 2s);
    auto const constrainedPeak = constrained->peak();
    constrained->release();
    unconstrained->release();
    first->wait();
    second->wait();

    REQUIRE(constrainedStarted);
    REQUIRE(otherWorkersStayedBusy);
    REQUIRE(constrainedPeak == 1);
    REQUIRE(first->getStatus() == RequestStatus::Success);
    REQUIRE(second->getStatus() == RequestStatus::Success);

    auto const statistics = service.getStatistics(false, false);
    auto const constrainedRow = std::ranges::find_if(
        statistics["datasources"],
        [](auto const& row) { return row["name"] == "MapA"; });
    REQUIRE(constrainedRow != statistics["datasources"].end());
    REQUIRE((*constrainedRow)["parallel-limit"] == 1);
}

TEST_CASE("Service rejects an empty homogeneous worker pool", "[Service][concurrency]")
{
    REQUIRE_THROWS_AS(Service(std::make_shared<MemCache>(1), false, 0ms, 0), std::runtime_error);
}

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

TEST_CASE("LayerTilesRequest preserves tile order and priority hints in JSON", "[Service][JSON]")
{
    SECTION("Requests serialize as tileIds")
    {
        auto request = std::make_shared<TestLayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector<TileId>{TileId::fromValue(kTtlTileIdValue)});

        REQUIRE(
            request->toJson() ==
            nlohmann::json{
                {"mapId", "Tropico"},
                {"layerId", "WayLayer"},
                {"tileIds", nlohmann::json::array({kTtlTileIdValue})},
            });
    }

    SECTION("Priority tile IDs are serialized as scheduling hints")
    {
        auto request = std::make_shared<TestLayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector<TileId>{
                TileId::fromValue(kTtlTileIdValue),
                TileId::fromValue(kTtlPriorityTileIdValue)},
            std::vector<TileId>{TileId::fromValue(kTtlPriorityTileIdValue)});

        REQUIRE(
            request->toJson() ==
            nlohmann::json{
                {"mapId", "Tropico"},
                {"layerId", "WayLayer"},
                {"tileIds", nlohmann::json::array({kTtlTileIdValue, kTtlPriorityTileIdValue})},
                {"priorityTileIds", nlohmann::json::array({kTtlPriorityTileIdValue})},
            });
    }

    SECTION("Priority hints cannot add tiles to the request")
    {
        REQUIRE_THROWS(TestLayerTilesRequest(
            "Tropico",
            "WayLayer",
            std::vector<TileId>{TileId::fromValue(kTtlTileIdValue)},
            std::vector<TileId>{TileId::fromValue(kTtlPriorityTileIdValue)}));
    }
}
