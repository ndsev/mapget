#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "mapget/model/featurelayer.h"
#include "mapget/model/mapschema.h"
#include "mapget/model/sourcedata.h"
#include "mapget/service/memcache.h"
#include "mapget/service/service.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
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

    void fill(PartitionFeatureLayer::Ptr const& tile) override
    {
        ++fillCount_;
        if (tileTtlOverride_) {
            tile->setTtl(tileTtlOverride_);
        }
        tile->setIdPrefix({{"areaId", "Area"}});
        tile->newFeature("Way", {{"wayId", 1}});
    }

    void fill(PartitionSourceDataLayer::Ptr const&) override
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

/** Controlled empty source covering tile/object payloads, raw data and add-on composition. */
class EmptyTtlDataSource : public TestTtlDataSource
{
public:
    LayerType layerType = LayerType::Features;
    PartitionKind partitionKind = PartitionKind::Tile;
    std::optional<std::chrono::milliseconds> producerTtl;
    bool empty = true;
    bool failed = false;
    bool addOn = false;
    std::string mapId = "Tropico";
    std::atomic_int fills = 0;
    std::vector<MapPartitionKey> expiredKeys;

    /** Reuse the feature definition but select the requested payload kind and source role. */
    DataSourceInfo info() override
    {
        auto result = TestTtlDataSource::info();
        result.mapId_ = mapId;
        result.isAddOn_ = addOn;
        result.stringPoolId_ = (addOn ? "EmptyAddOnPool" : "EmptyTtlPool") + mapId;
        auto layer = std::make_shared<LayerInfo>(*result.layers_.at("WayLayer"));
        layer->type_ = layerType;
        layer->partitionKind_ = partitionKind;
        if (partitionKind == PartitionKind::Object)
            layer->tileAssociationLevel_ = 13;
        result.layers_["WayLayer"] = std::move(layer);
        if (layerType == LayerType::SourceData) {
            auto owner = std::make_shared<MapSchema>();
            (void)owner->importLayer("WayLayer", LayerType::SourceData, nlohmann::json::object());
            owner->finalize();
            result.schemaId_.clear();
            result.setMapSchema(std::move(owner));
        }
        return result;
    }

    /** Emit no features until the test simulates data becoming available. */
    void fill(PartitionFeatureLayer::Ptr const& tile) override
    {
        ++fills;
        tile->setTtl(producerTtl);
        if (failed)
            tile->setError("synthetic failure");
        else if (!empty)
            TestTtlDataSource::fill(tile);
    }

    /** Emit a genuine source-data root only when the test makes data available. */
    void fill(PartitionSourceDataLayer::Ptr const& tile) override
    {
        ++fills;
        tile->setTtl(producerTtl);
        if (failed)
            tile->setError("synthetic failure");
        else if (!empty)
            tile->addRoot(tile->newCompound(0));
    }

    /** Record the normal expiry notification used by adapters to invalidate raw caches. */
    void onCacheExpired(MapPartitionKey const& key, std::chrono::system_clock::time_point) override
    {
        expiredKeys.push_back(key);
    }

    /** Fetch through normal service admission, cache and partition serialization paths. */
    PartitionLayer::Ptr fetch(Service& service)
    {
        auto partition = partitionKind == PartitionKind::Object ?
            PartitionId::object(42) :
            PartitionId::fromValue(kTtlTileIdValue);
        auto request =
            std::make_shared<LayerTilesRequest>(mapId, "WayLayer", std::vector{partition});
        PartitionLayer::Ptr result;
        request->onFeatureLayer([&](auto tile) { result = std::move(tile); });
        request->onSourceDataLayer([&](auto tile) { result = std::move(tile); });
        REQUIRE(service.request({request}));
        request->wait();
        REQUIRE(request->getStatus() == RequestStatus::Success);
        REQUIRE(result);
        return result;
    }
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
        info_.setMapSchema(layerInfo_->schemaOwner());
        layerInfo_ = info_.getLayer("MutableLayer");
    }

    DataSourceInfo info() override { return info_; }

    void fill(PartitionFeatureLayer::Ptr const& tile) override
    {
        tile->newFeature("Way", {{"wayId", int64_t(1)}});
    }

    void fill(PartitionSourceDataLayer::Ptr const&) override
    {
        throw UnsupportedSourceDataLayerError(
            "SourceDataLayer not supported in MutableInfoDataSource");
    }

    void mutateAdvertisedInfo()
    {
        layerInfo_->layerId_ = "MutatedLayer";
        info_.layers_.clear();
        info_.mapSchema_.reset();
        info_.schemaId_.clear();
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
        info_.setMapSchema(info_.getLayer("Features")->schemaOwner());
    }

    /** Return the synthetic source metadata used for scheduler admission. */
    DataSourceInfo info() override { return info_; }

    /** Block in the shared probe so tests can inspect scheduler concurrency. */
    void fill(PartitionFeatureLayer::Ptr const&) override { probe_->enter(); }

    /** Reject source-data requests because this fixture serves only features. */
    void fill(PartitionSourceDataLayer::Ptr const&) override
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

    std::vector<PartitionId> tiles;
    for (int32_t x = 0; x < 4; ++x) {
        tiles.push_back(TileId::fromTileXY(x, 0, 3));
    }
    auto first = std::make_shared<LayerTilesRequest>("MapA", "Features", tiles);
    auto second = std::make_shared<LayerTilesRequest>("MapB", "Features", tiles);
    REQUIRE(service.request({first, second}));

    auto const filledCap = probe->waitForEntries(2, 2s);
    auto const peakBeforeRelease = probe->peak();
    auto const activeStatistics = service.getStatistics(false, false);
    probe->release();
    first->wait();
    second->wait();

    REQUIRE(filledCap);
    REQUIRE(peakBeforeRelease == 2);
    REQUIRE(activeStatistics["queued-tile-work-items"] == 6);
    REQUIRE(activeStatistics["in-flight-tile-jobs"] == 2);
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

    std::vector<PartitionId> tiles;
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

TEST_CASE("Service skips requests while their work admission is closed", "[Service][backpressure]")
{
    auto blockedProbe = std::make_shared<GlobalConcurrencyProbe>();
    auto liveProbe = std::make_shared<GlobalConcurrencyProbe>();
    Service service(std::make_shared<MemCache>(32), false, 0ms, 1);
    service.add(
        std::make_shared<ConcurrencyTestDataSource>("BlockedMap", "BlockedPool", blockedProbe));
    service.add(std::make_shared<ConcurrencyTestDataSource>("LiveMap", "LivePool", liveProbe));

    auto admissionOpen = std::make_shared<std::atomic_bool>(false);
    auto blocked = std::make_shared<LayerTilesRequest>(
        "BlockedMap",
        "Features",
        std::vector<PartitionId>{TileId::fromTileXY(0, 0, 3)});
    blocked->setWorkAdmissionGate(admissionOpen);
    auto live = std::make_shared<LayerTilesRequest>(
        "LiveMap",
        "Features",
        std::vector<PartitionId>{TileId::fromTileXY(0, 0, 3)});

    REQUIRE(service.request({blocked, live}));
    auto const liveStarted = liveProbe->waitForEntries(1, 2s);
    liveProbe->release();
    live->wait();
    auto const blockedStayedQueued = !blockedProbe->waitForEntries(1, 100ms);

    admissionOpen->store(true);
    service.notifyWorkAvailable();
    auto const blockedStarted = blockedProbe->waitForEntries(1, 2s);
    blockedProbe->release();
    blocked->wait();

    REQUIRE(liveStarted);
    REQUIRE(blockedStayedQueued);
    REQUIRE(blockedStarted);
    REQUIRE(live->getStatus() == RequestStatus::Success);
    REQUIRE(blocked->getStatus() == RequestStatus::Success);
}

TEST_CASE("Shared work still completes requests with closed admission", "[Service][backpressure]")
{
    Service service(std::make_shared<MemCache>(32), false, 0ms, 1);
    auto source = std::make_shared<TestTtlDataSource>();
    service.add(source);
    auto const tileId = TileId::fromValue(kTtlTileIdValue);

    auto sharedResultCount = std::atomic_size_t{0};
    auto blocked = std::make_shared<
        LayerTilesRequest>("Tropico", "WayLayer", std::vector<PartitionId>{tileId});
    blocked->setWorkAdmissionGate(std::make_shared<std::atomic_bool>(false));
    blocked->onFeatureLayer([&](PartitionFeatureLayer::Ptr) { ++sharedResultCount; });
    auto live = std::make_shared<
        LayerTilesRequest>("Tropico", "WayLayer", std::vector<PartitionId>{tileId});
    live->onFeatureLayer([&](PartitionFeatureLayer::Ptr) { ++sharedResultCount; });

    REQUIRE(service.request({blocked, live}));
    blocked->wait();
    live->wait();

    REQUIRE(blocked->getStatus() == RequestStatus::Success);
    REQUIRE(live->getStatus() == RequestStatus::Success);
    REQUIRE(sharedResultCount == 2);
    REQUIRE(source->fillCount() == 1);
}

TEST_CASE("Service rejects an empty homogeneous worker pool", "[Service][concurrency]")
{
    REQUIRE_THROWS_AS(Service(std::make_shared<MemCache>(1), false, 0ms, 0), std::runtime_error);
}

/** Deterministic cancellation probe: keep the first fill alive while replacements run. */
class CancellableTestDataSource : public TestTtlDataSource
{
public:
    /** Allow two jobs so a replacement can finish before an abandoned job unwinds. */
    DataSourceInfo info() override
    {
        auto result = TestTtlDataSource::info();
        result.maxParallelJobs_ = 2;
        return result;
    }
    /** Observe cancellation, but deliberately postpone unwinding to exercise late completion. */
    void fill(PartitionFeatureLayer::Ptr const& tile, TileCancellationCheck const& cancelled) override
    {
        if (++started_ == 1) {
            entered_.set_value();
            while (!release_) {
                if (cancelled())
                    observedCancellation_ = true;
                std::this_thread::sleep_for(1ms);
            }
            if (cancelled()) {
                observedCancellation_ = true;
                exited_.set_value();
                throw TileLoadCancelled();
            }
        }
        TestTtlDataSource::fill(tile);
    }
    std::promise<void> entered_;
    std::promise<void> exited_;
    std::atomic_bool release_{false};
    std::atomic_bool observedCancellation_{false};
    std::atomic_size_t started_{0};
};

TEST_CASE("Shared tile cancellation follows all consumers and does not poison replacements", "[Service][cancellation]")
{
    auto cache = std::make_shared<MemCache>(32);
    auto source = std::make_shared<CancellableTestDataSource>();
    Service service(cache, false, 0ms, 2);
    service.add(source);
    auto makeRequest = [] {
        return std::make_shared<LayerTilesRequest>("Tropico", "WayLayer",
            std::vector<PartitionId>{TileId::fromValue(kTtlTileIdValue)});
    };
    auto first = makeRequest();
    REQUIRE(service.request({first}));
    auto started = source->entered_.get_future().wait_for(2s) == std::future_status::ready;

    SECTION("One abort does not cancel a coalesced consumer, even with closed admission") {
        auto second = makeRequest();
        second->setWorkAdmissionGate(std::make_shared<std::atomic_bool>(false));
        auto accepted = service.request({second});
        service.abort(first);
        source->release_ = true;
        second->wait();
        REQUIRE(started);
        REQUIRE(accepted);
        REQUIRE_FALSE(source->observedCancellation_);
        REQUIRE(second->getStatus() == RequestStatus::Success);
        REQUIRE(source->started_ == 1);
    }
    SECTION("Last-consumer abort and retainOutputs both detach abandoned jobs") {
        bool retain = false;
        SECTION("abort") { service.abort(first); }
        SECTION("retain nothing") { retain = true; service.retainOutputs(first, {}); }
        auto replacement = makeRequest();
        auto accepted = service.request({replacement});
        replacement->wait();
        source->release_ = true;
        auto exited = source->exited_.get_future().wait_for(2s) == std::future_status::ready;
        // The late old failure must neither remove the new tile nor cache a cancellation error.
        auto cached = makeRequest();
        auto cachedAccepted = service.request({cached});
        cached->wait();
        INFO(retain);
        REQUIRE(started);
        REQUIRE(accepted);
        REQUIRE(exited);
        REQUIRE(source->observedCancellation_);
        REQUIRE(cachedAccepted);
        REQUIRE(replacement->getStatus() == RequestStatus::Success);
        REQUIRE(cached->getStatus() == RequestStatus::Success);
        REQUIRE(source->started_ == 2);
    }
}

TEST_CASE("Service shutdown contains request callback exceptions", "[Service][concurrency]")
{
    auto probe = std::make_shared<GlobalConcurrencyProbe>();
    auto service = std::make_unique<Service>(std::make_shared<MemCache>(1), false, 0ms, 1);
    service->add(std::make_shared<ConcurrencyTestDataSource>("Map", "Pool", probe));

    auto request = std::make_shared<LayerTilesRequest>(
        "Map",
        "Features",
        std::vector<PartitionId>{TileId::fromTileXY(0, 0, 3)});
    std::promise<void> callbackEntered;
    auto callbackEnteredFuture = callbackEntered.get_future();
    request->onDone_ = [&callbackEntered](RequestStatus) {
        callbackEntered.set_value();
        throw std::runtime_error("Test callback failure");
    };

    REQUIRE(service->request({request}));
    REQUIRE(probe->waitForEntries(1, 2s));
    auto destruction = std::async(
        std::launch::async,
        [service = std::move(service)]() mutable { service.reset(); });

    auto const callbackWasContained = callbackEnteredFuture.wait_for(2s) == std::future_status::ready;
    probe->release();

    REQUIRE(callbackWasContained);
    REQUIRE(destruction.wait_for(2s) == std::future_status::ready);
    REQUIRE_NOTHROW(destruction.get());
    REQUIRE(request->getStatus() == RequestStatus::Aborted);
}

TEST_CASE("Service TTL behavior", "[Service][TTL]")
{
    auto cache = std::make_shared<MemCache>(1024);

    SECTION("Global TTL default applied when datasource TTL is not set")
    {
        const auto globalTtl = 10ms;
        // A separate error cap must not change the successful-tile fallback.
        Service service(cache, false, globalTtl, 1, 7s);
        auto ds = std::make_shared<TestTtlDataSource>();
        service.add(ds);

        auto request1 = std::make_shared<LayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector<PartitionId>{TileId::fromValue(kTtlTileIdValue)});

        PartitionFeatureLayer::Ptr tile1;
        request1->onFeatureLayer([&](PartitionFeatureLayer::Ptr const& tile) { tile1 = tile; });

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
            std::vector<PartitionId>{TileId::fromValue(kTtlTileIdValue)});

        PartitionFeatureLayer::Ptr tile2;
        request2->onFeatureLayer([&](PartitionFeatureLayer::Ptr const& tile) { tile2 = tile; });

        REQUIRE(service.request({request2}));
        request2->wait();

        REQUIRE(tile2);
        REQUIRE(ds->fillCount() == 1);

        // Wait long enough for TTL to expire and request again.
        std::this_thread::sleep_for(20ms);

        auto request3 = std::make_shared<LayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector<PartitionId>{TileId::fromValue(kTtlTileIdValue)});

        PartitionFeatureLayer::Ptr tile3;
        request3->onFeatureLayer([&](PartitionFeatureLayer::Ptr const& tile) { tile3 = tile; });

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
            std::vector<PartitionId>{TileId::fromValue(kTtlTileIdValue)});

        PartitionFeatureLayer::Ptr tile;
        request->onFeatureLayer([&](PartitionFeatureLayer::Ptr const& t) { tile = t; });

        REQUIRE(service.request({request}));
        request->wait();

        REQUIRE(tile);
        REQUIRE(tile->ttl().has_value());
        REQUIRE(tile->ttl().value() == datasourceTtl);
        REQUIRE(ds->fillCount() == 1);
    }
}

TEST_CASE("Empty source lifetimes are bounded independently of normal defaults", "[Service][TTL]")
{
    auto const layerType = GENERATE(LayerType::Features, LayerType::SourceData);
    auto const partitionKind = GENERATE(PartitionKind::Tile, PartitionKind::Object);
    auto const ttlMs = GENERATE(-1, 0, 1000, 60000);
    auto const emptyTtl = std::chrono::milliseconds(GENERATE(250, 5000, 12000));
    auto source = std::make_shared<EmptyTtlDataSource>();
    source->layerType = layerType;
    source->partitionKind = partitionKind;
    source->producerTtl = ttlMs < 0 ?
        std::nullopt :
        std::optional(std::chrono::milliseconds(ttlMs));
    source->setTtl(1h);
    Service service(std::make_shared<MemCache>(16), false, 2h, 1, 9s, emptyTtl);
    service.add(source);
    auto const partition = partitionKind == PartitionKind::Object ?
        PartitionId::object(UINT64_MAX) :
        PartitionId::fromValue(kTtlTileIdValue);
    auto request =
        std::make_shared<LayerTilesRequest>("Tropico", "WayLayer", std::vector{partition});
    PartitionLayer::Ptr result;
    request->onFeatureLayer([&](auto tile) { result = std::move(tile); });
    request->onSourceDataLayer([&](auto tile) { result = std::move(tile); });
    REQUIRE(service.request({request}));
    request->wait();
    REQUIRE(result);
    REQUIRE_FALSE(result->error());
    REQUIRE(result->partitionId() == partition);
    REQUIRE(result->ttl() == (ttlMs > 0 ? std::min(*source->producerTtl, emptyTtl) : emptyTtl));
}

TEST_CASE("Empty cached values recover through ordinary expiry", "[Service][TTL]")
{
    auto source = std::make_shared<EmptyTtlDataSource>();
    source->layerType = GENERATE(LayerType::Features, LayerType::SourceData);
    auto cache = std::make_shared<MemCache>(16);
    Service service(cache, false, 60s, 1);
    service.add(source);
    auto fetch = [&]
    {
        auto request = std::make_shared<LayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector{PartitionId::fromValue(kTtlTileIdValue)});
        PartitionLayer::Ptr result;
        request->onFeatureLayer([&](auto tile) { result = std::move(tile); });
        request->onSourceDataLayer([&](auto tile) { result = std::move(tile); });
        REQUIRE(service.request({request}));
        request->wait();
        REQUIRE(result);
        return result;
    };
    auto first = fetch();
    REQUIRE(first->ttl() == Service::DEFAULT_EMPTY_TILE_TTL);
    REQUIRE(first->ttl() == 1h);
    source->empty = false;
    REQUIRE(fetch()->timestamp() == first->timestamp());
    REQUIRE(source->fills == 1);
    REQUIRE(source->expiredKeys.empty());

    first->setTimestamp(std::chrono::system_clock::now() - Service::DEFAULT_EMPTY_TILE_TTL - 1s);
    cache->putTileLayer(first);
    auto recovered = fetch();
    REQUIRE(recovered->ttl() == 60s);
    REQUIRE(source->fills == 2);
    REQUIRE(source->expiredKeys == std::vector{first->id()});
}

TEST_CASE("Empty errors use the error lifetime rather than the empty lifetime", "[Service][TTL]")
{
    auto source = std::make_shared<EmptyTtlDataSource>();
    source->layerType = GENERATE(LayerType::Features, LayerType::SourceData);
    source->failed = true;
    Service service(std::make_shared<MemCache>(16), false, 60s, 1, 7s, 2s);
    service.add(source);
    auto request = std::make_shared<LayerTilesRequest>(
        "Tropico",
        "WayLayer",
        std::vector{PartitionId::fromValue(kTtlTileIdValue)});
    PartitionLayer::Ptr result;
    request->onFeatureLayer([&](auto tile) { result = std::move(tile); });
    request->onSourceDataLayer([&](auto tile) { result = std::move(tile); });
    REQUIRE(service.request({request}));
    request->wait();
    REQUIRE(result);
    REQUIRE(result->error());
    REQUIRE(result->ttl() == 7s);
}

TEST_CASE("Empty filter and inspection matches inherit their source lifetime", "[Service][TTL]")
{
    Service service(std::make_shared<MemCache>(16), false, 60s, 1, 7s, 2s);
    auto source = std::make_shared<EmptyTtlDataSource>();
    source->empty = GENERATE(true, false);
    service.add(source);
    auto const expectedTtl = source->empty ? 2s : 60s;
    auto const partitions = std::vector{PartitionId::fromValue(kTtlTileIdValue)};
    FeatureLayerFilterRequest filter;
    filter.filterId_ = "match-none";
    filter.channels_ = {FeatureLayerFilterChannel{.channelId_ = "none", .entryFilter_ = "false"}};
    auto filtered =
        std::make_shared<FeatureLayerFilterTilesRequest>("Tropico", "WayLayer", partitions, filter);
    PartitionSubsetLayer::Ptr subset;
    filtered->onFilterResult([&](auto tile) { subset = std::move(tile); });
    REQUIRE(service.request(filtered));
    filtered->wait();
    REQUIRE(subset);
    REQUIRE(subset->size() == 1);
    REQUIRE(subset->at(0)->entryCount() == 0);
    REQUIRE(subset->ttl() == expectedTtl);

    auto inspection = std::make_shared<LayerTilesRequest>("Tropico", "WayLayer", partitions);
    inspection->featureIdsByTile_[partitions.front()] = {"Way.Area.999"};
    PartitionFeatureLayer::Ptr tile;
    inspection->onFeatureLayer([&](auto result) { tile = std::move(result); });
    REQUIRE(service.request({inspection}));
    inspection->wait();
    REQUIRE(tile);
    REQUIRE(tile->numRoots() == 0);
    REQUIRE(tile->ttl() == expectedTtl);
}

TEST_CASE("Empty source contributors limit populated add-on composites", "[Service][TTL][add-on]")
{
    auto base = std::make_shared<EmptyTtlDataSource>();
    auto addOn = std::make_shared<EmptyTtlDataSource>();
    addOn->addOn = true;
    base->empty = GENERATE(true, false);
    addOn->empty = !base->empty;
    Service service(std::make_shared<MemCache>(16), false, 60s, 1, 7s, 3s);
    service.add(base);
    service.add(addOn);
    auto request = std::make_shared<LayerTilesRequest>(
        "Tropico",
        "WayLayer",
        std::vector{PartitionId::fromValue(kTtlTileIdValue)});
    PartitionFeatureLayer::Ptr tile;
    request->onFeatureLayer([&](auto result) { tile = std::move(result); });
    REQUIRE(service.request({request}));
    request->wait();
    REQUIRE(tile);
    REQUIRE(tile->numRoots() == 1);
    REQUIRE(tile->ttl() == 3s);
}

TEST_CASE("Service rejects negative empty tile TTLs", "[Service][TTL]")
{
    REQUIRE_THROWS_WITH(
        Service(std::make_shared<MemCache>(), false, 0ms, 1, 5s, -1ms),
        "Empty tile TTL must not be negative.");
}

TEST_CASE("Retry TTL overrides preserve zero and inherit only when absent", "[Service][TTL]")
{
    auto const failed = GENERATE(false, true);
    auto const global = std::chrono::milliseconds(GENERATE(0, 7000));
    auto const overrideMs = GENERATE(-1, 0, 2000, 12000);
    auto const producerMs = GENERATE(-1, 1000, 30000);
    auto source = std::make_shared<EmptyTtlDataSource>();
    source->failed = failed;
    source->producerTtl = producerMs < 0 ?
        std::nullopt :
        std::optional{std::chrono::milliseconds(producerMs)};
    auto override = overrideMs < 0 ?
        std::nullopt :
        std::optional{std::chrono::milliseconds(overrideMs)};
    source->setErrorTileTtl(failed ? override : std::optional{3ms});
    source->setEmptyTileTtl(failed ? std::optional{3ms} : override);
    source->setTtl(1ms);  // Nonempty fallback must never leak into either policy.
    auto cache = std::make_shared<MemCache>(16);
    Service service(cache, false, 1ms, 1, global, global);
    service.add(source);

    auto policy = override.value_or(global);
    auto expected = policy == 0ms || producerMs <= 0 ?
        policy :
        std::min(policy, std::chrono::milliseconds(producerMs));
    auto first = source->fetch(service);
    REQUIRE(first->ttl() == expected);
    REQUIRE(source->fills == 1);
    // Age the stored wire payload instead of sleeping. A positive producer TTL
    // cannot reintroduce expiry after an explicit zero policy has been applied.
    first->setTimestamp(std::chrono::system_clock::now() - 24h);
    cache->putTileLayer(first);
    source->failed = false;
    source->empty = false;
    source->fetch(service);
    REQUIRE(source->fills == (expected == 0ms ? 1 : 2));
    REQUIRE(source->expiredKeys.size() == (expected == 0ms ? 0 : 1));
}

TEST_CASE(
    "Non-expiring error and empty partitions remain cached until invalidated",
    "[Service][TTL]")
{
    auto source = std::make_shared<EmptyTtlDataSource>();
    source->failed = GENERATE(false, true);
    source->layerType = GENERATE(LayerType::Features, LayerType::SourceData);
    source->partitionKind = GENERATE(PartitionKind::Tile, PartitionKind::Object);
    source->producerTtl = 1ms;
    auto cache = std::make_shared<MemCache>(16);
    Service service(cache, false, 1ms, 1, 0ms, 0ms);
    service.add(source);
    auto first = source->fetch(service);
    REQUIRE(first->ttl() == 0ms);
    first->setTimestamp(std::chrono::system_clock::now() - 24h);
    cache->putTileLayer(first);
    source->failed = false;
    source->empty = false;
    for (int repeat = 0; repeat < 3; ++repeat)
        REQUIRE(
            source->fetch(service)->timestamp() ==
            std::chrono::time_point_cast<std::chrono::microseconds>(first->timestamp()));
    REQUIRE(source->fills == 1);
    REQUIRE(source->expiredKeys.empty());

    REQUIRE(service.resetMapCache(source->mapId));
    auto fresh = source->fetch(service);
    REQUIRE_FALSE(fresh->error());
    REQUIRE(fresh->ttl() == 1ms);  // Healthy nonempty producer freshness is unchanged.
    REQUIRE(source->fills == 2);
    REQUIRE(source->expiredKeys.empty());  // Manual invalidation is not expiry.
}

TEST_CASE("Retry policies are isolated between datasources", "[Service][TTL]")
{
    auto cache = std::make_shared<MemCache>(16);
    Service service(cache, false, 1h, 1, 5s, 1h);
    auto persistent = std::make_shared<EmptyTtlDataSource>();
    persistent->failed = GENERATE(false, true);
    persistent->setErrorTileTtl(0ms);
    persistent->setEmptyTileTtl(0ms);
    auto inherited = std::make_shared<EmptyTtlDataSource>();
    inherited->mapId = "Inherited";
    inherited->failed = persistent->failed;
    service.add(persistent);
    service.add(inherited);
    for (auto const& source : {persistent, inherited}) {
        auto first = source->fetch(service);
        first->setTimestamp(std::chrono::system_clock::now() - 24h);
        cache->putTileLayer(first);
        source->fetch(service);
    }
    REQUIRE(persistent->fills == 1);
    REQUIRE(inherited->fills == 2);
    REQUIRE(persistent->errorTileTtl() == 0ms);
    REQUIRE_FALSE(inherited->errorTileTtl());
}

TEST_CASE("Add-on retry policies retain the earliest composite expiry", "[Service][TTL][add-on]")
{
    auto base = std::make_shared<EmptyTtlDataSource>();
    base->empty = false;
    base->producerTtl = std::chrono::milliseconds(GENERATE(0, 30000));
    auto addOn = std::make_shared<EmptyTtlDataSource>();
    addOn->addOn = true;
    addOn->failed = GENERATE(false, true);
    addOn->producerTtl = 1s;
    auto const global = std::chrono::milliseconds(GENERATE(0, 7000));
    auto const overrideMs = GENERATE(-1, 0, 2000);
    auto const override = overrideMs < 0 ?
        std::nullopt :
        std::optional{std::chrono::milliseconds(overrideMs)};
    addOn->setErrorTileTtl(override);
    addOn->setEmptyTileTtl(override);
    auto cache = std::make_shared<MemCache>(16);
    Service service(cache, false, 1h, 1, global, global);
    service.add(base);
    service.add(addOn);

    auto tile = base->fetch(service);
    auto const policy = override.value_or(global);
    REQUIRE(tile->ttl() == (policy == 0ms ? *base->producerTtl : 1s));
    REQUIRE_FALSE(tile->error());
    REQUIRE(tile->warnings().empty() == !addOn->failed);
    REQUIRE(base->fills == 1);
    REQUIRE(addOn->fills == 1);

    tile->setTimestamp(std::chrono::system_clock::now() - 24h);
    cache->putTileLayer(tile);
    base->fetch(service);
    // Cache entries contain whole compositions, not retained native contributions.
    // A finite primary lifetime refreshes even a zero-policy add-on with it.
    auto const loads = tile->ttl() == 0ms ? 1 : 2;
    REQUIRE(base->fills == loads);
    REQUIRE(addOn->fills == loads);
}

TEST_CASE("Filtered error and empty sources do not regain a retry lifetime", "[Service][TTL]")
{
    auto source = std::make_shared<EmptyTtlDataSource>();
    source->failed = GENERATE(false, true);
    source->producerTtl = 1ms;
    Service service(std::make_shared<MemCache>(16), false, 1ms, 1, 0ms, 0ms);
    service.add(source);
    FeatureLayerFilterRequest filter;
    filter.filterId_ = "no-retry";
    filter.channels_ = {FeatureLayerFilterChannel{.channelId_ = "all", .entryFilter_ = "true"}};
    for (int repeat = 0; repeat < 2; ++repeat) {
        auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector{PartitionId::fromValue(kTtlTileIdValue)},
            filter);
        PartitionSubsetLayer::Ptr subset;
        request->onFilterResult([&](auto tile) { subset = std::move(tile); });
        REQUIRE(service.request(request));
        request->wait();
        REQUIRE(subset);
        REQUIRE((!subset->ttl() || subset->ttl() == 0ms));
    }
    REQUIRE(source->fills == 1);
    REQUIRE(source->expiredKeys.empty());
}

TEST_CASE(
    "Datasource retry overrides reject negative values without changing policy",
    "[Service][TTL]")
{
    EmptyTtlDataSource source;
    source.setErrorTileTtl(0ms);
    source.setEmptyTileTtl(2s);
    REQUIRE_THROWS_AS(source.setErrorTileTtl(-1ms), std::invalid_argument);
    REQUIRE_THROWS_AS(source.setEmptyTileTtl(-1ms), std::invalid_argument);
    REQUIRE(source.errorTileTtl() == 0ms);
    REQUIRE(source.emptyTileTtl() == 2s);
    source.setErrorTileTtl(std::nullopt);
    source.setEmptyTileTtl(std::nullopt);
    REQUIRE_FALSE(source.errorTileTtl());
    REQUIRE_FALSE(source.emptyTileTtl());
}

TEST_CASE("Service info uses detached datasource metadata snapshots", "[Service][DataSourceInfo]")
{
    auto service = Service(std::make_shared<MemCache>(32), false);
    auto dataSource = std::make_shared<MutableInfoDataSource>();
    auto schema = dataSource->info().mapSchema_;
    service.add(dataSource);

    dataSource->mutateAdvertisedInfo();

    auto firstSnapshot = service.info();
    REQUIRE(firstSnapshot.size() == 1);
    REQUIRE(firstSnapshot.front().layers_.size() == 1);
    REQUIRE(firstSnapshot.front().layers_.at("MutableLayer")->layerId_ == "MutableLayer");
    REQUIRE(firstSnapshot.front().mapSchema_->featureIdentities().size() == 1);
    REQUIRE(firstSnapshot.front().mapSchema_ == schema);

    firstSnapshot.front().layers_.at("MutableLayer")->layerId_ = "CallerMutation";

    auto secondSnapshot = service.info();
    REQUIRE(secondSnapshot.size() == 1);
    REQUIRE(secondSnapshot.front().layers_.at("MutableLayer")->layerId_ == "MutableLayer");
    REQUIRE(secondSnapshot.front().mapSchema_ == schema);
}

TEST_CASE("LayerTilesRequest preserves tile order and priority hints in JSON", "[Service][JSON]")
{
    SECTION("Requests serialize as tagged partitions")
    {
        auto request = std::make_shared<TestLayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector<PartitionId>{TileId::fromValue(kTtlTileIdValue)});

        REQUIRE(
            request->toJson() ==
            nlohmann::json{
                {"mapId", "Tropico"},
                {"layerId", "WayLayer"},
                {"partitions",
                 nlohmann::json::array({PartitionId::fromValue(kTtlTileIdValue).toJson()})},
            });
    }

    SECTION("Priority tile IDs are serialized as scheduling hints")
    {
        auto request = std::make_shared<TestLayerTilesRequest>(
            "Tropico",
            "WayLayer",
            std::vector<PartitionId>{
                TileId::fromValue(kTtlTileIdValue),
                TileId::fromValue(kTtlPriorityTileIdValue)},
            std::vector<PartitionId>{TileId::fromValue(kTtlPriorityTileIdValue)});

        REQUIRE(
            request->toJson() ==
            nlohmann::json{
                {"mapId", "Tropico"},
                {"layerId", "WayLayer"},
                {"partitions",
                 nlohmann::json::array(
                     {PartitionId::fromValue(kTtlTileIdValue).toJson(),
                      PartitionId::fromValue(kTtlPriorityTileIdValue).toJson()})},
                {"priorityPartitions",
                 nlohmann::json::array({PartitionId::fromValue(kTtlPriorityTileIdValue).toJson()})},
            });
    }

    SECTION("Priority hints cannot add tiles to the request")
    {
        REQUIRE_THROWS(TestLayerTilesRequest(
            "Tropico",
            "WayLayer",
            std::vector<PartitionId>{TileId::fromValue(kTtlTileIdValue)},
            std::vector<PartitionId>{TileId::fromValue(kTtlPriorityTileIdValue)}));
    }
}
