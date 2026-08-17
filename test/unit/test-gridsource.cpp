#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <string>

#include "gridsource/gridsource.h"
#include "mapget/model/featurelayer.h"

using namespace mapget;
using namespace mapget::gridsource;
using namespace std::chrono_literals;

namespace
{

YAML::Node trafficConfig(std::string_view overrides = {})
{
    auto source = std::string{R"yaml(
mapId: GridTrafficTest
layers:
  - name: DevSrc-RoadLayer
    featureType: DevSrc-Road
    geometry:
      type: line
  - name: DevSrc-TrafficLayer
    featureType: DevSrc-Traffic
    kind: traffic
    traffic:
      roadLayer: DevSrc-RoadLayer
      tileLevel: 13
      updateIntervalSeconds: 5
      seed: 7
  - name: DevSrc-IntersectionLayer
    featureType: DevSrc-Intersection
    geometry:
      type: point
)yaml"};
    source.append(overrides);
    return YAML::Load(source);
}

TileFeatureLayer::Ptr makeTile(
    DataSourceInfo const& info,
    std::string const& layerId,
    TileId tileId = TileId::fromWgs84(11.0, 48.0, 13))
{
    return std::make_shared<TileFeatureLayer>(
        tileId,
        info.stringPoolId_,
        info.mapId_,
        info.getLayer(layerId),
        std::make_shared<StringPool>(info.stringPoolId_));
}

} // namespace

TEST_CASE("Grid traffic configuration validates its road affinity", "[gridsource][traffic]")
{
    REQUIRE_NOTHROW(GridDataSource(trafficConfig()));

    SECTION("defaults are applied")
    {
        auto config = YAML::Load(R"yaml(
layers:
  - name: roads
    featureType: Road
    geometry: {type: line}
  - name: traffic
    featureType: Traffic
    kind: traffic
    traffic: {roadLayer: roads}
)yaml");
        REQUIRE_NOTHROW(GridDataSource(config));
    }

    SECTION("missing road target is rejected")
    {
        auto config = trafficConfig();
        config["layers"][1]["traffic"]["roadLayer"] = "missing";
        REQUIRE_THROWS_WITH(
            GridDataSource(config),
            Catch::Matchers::ContainsSubstring("must resolve exactly once"));
    }

    SECTION("traffic object and kind must agree")
    {
        auto config = trafficConfig();
        config["layers"][1].remove("traffic");
        REQUIRE_THROWS_WITH(
            GridDataSource(config),
            Catch::Matchers::ContainsSubstring("requires a 'traffic' object"));

        config = trafficConfig();
        config["layers"][1]["kind"] = "auto";
        REQUIRE_THROWS_WITH(
            GridDataSource(config),
            Catch::Matchers::ContainsSubstring("kind 'auto'"));
    }

    SECTION("disabled road target is rejected")
    {
        auto config = trafficConfig();
        config["layers"][0]["enabled"] = false;
        REQUIRE_THROWS_WITH(
            GridDataSource(config),
            Catch::Matchers::ContainsSubstring("is disabled"));
    }

    SECTION("self and non-line road targets are rejected")
    {
        auto config = trafficConfig();
        config["layers"][1]["traffic"]["roadLayer"] = "DevSrc-TrafficLayer";
        REQUIRE_THROWS_WITH(
            GridDataSource(config),
            Catch::Matchers::ContainsSubstring("cannot reference itself"));

        config = trafficConfig();
        config["layers"][0]["geometry"]["type"] = "point";
        REQUIRE_THROWS_WITH(
            GridDataSource(config),
            Catch::Matchers::ContainsSubstring("non-traffic line layer"));
    }

    SECTION("traffic cannot redefine its fixed payload")
    {
        auto config = trafficConfig();
        config["layers"][1]["attributes"]["top"] = YAML::Load("[]");
        REQUIRE_THROWS_WITH(
            GridDataSource(config),
            Catch::Matchers::ContainsSubstring("generic attributes or relations"));
    }

    SECTION("traffic geometry is an untuned line")
    {
        auto config = trafficConfig();
        config["layers"][1]["geometry"]["density"] = 0.1;
        REQUIRE_THROWS_WITH(
            GridDataSource(config),
            Catch::Matchers::ContainsSubstring("untuned"));
    }

    SECTION("cadence and level are bounded")
    {
        auto config = trafficConfig();
        config["layers"][1]["traffic"]["updateIntervalSeconds"] = 0;
        REQUIRE_THROWS_WITH(
            GridDataSource(config),
            Catch::Matchers::ContainsSubstring("between 1 and 60"));

        config = trafficConfig();
        config["layers"][1]["traffic"]["tileLevel"] = 12;
        REQUIRE_THROWS_WITH(
            GridDataSource(config),
            Catch::Matchers::ContainsSubstring("between 13 and 15"));

        config = trafficConfig();
        config["layers"][1]["traffic"]["seed"] = uint64_t{1} << 32U;
        REQUIRE_THROWS_WITH(
            GridDataSource(config),
            Catch::Matchers::ContainsSubstring("unsigned 32-bit"));
    }

    SECTION("traffic name and type collisions are rejected without changing auto layers")
    {
        auto config = trafficConfig();
        config["layers"].push_back(config["layers"][0]);
        REQUIRE_THROWS_WITH(
            GridDataSource(config),
            Catch::Matchers::ContainsSubstring("must resolve exactly once"));

        auto legacy = YAML::Load(R"yaml(
layers:
  - {name: duplicate, featureType: Duplicate, geometry: {type: line}}
  - {name: duplicate, featureType: Duplicate, geometry: {type: line}}
)yaml");
        REQUIRE_NOTHROW(GridDataSource(legacy));
    }
}

TEST_CASE("Grid traffic is logically reproducible across sources and signed tile ids", "[gridsource][traffic]")
{
    const auto now = std::chrono::system_clock::time_point{500s};
    auto config = trafficConfig();
    config["layers"][1]["traffic"]["tileLevel"] = 15;
    GridDataSource firstSource(config, [&] { return now; });
    GridDataSource restartedSource(config, [&] { return now; });
    const auto tileId = TileId::fromWgs84(179.9, -80.0, 15);
    auto first = makeTile(firstSource.info(), "DevSrc-TrafficLayer", tileId);
    auto restarted = makeTile(restartedSource.info(), "DevSrc-TrafficLayer", tileId);

    firstSource.fill(first);
    restartedSource.fill(restarted);

    REQUIRE(first->toJson()["features"] == restarted->toJson()["features"]);
    REQUIRE(first->numRoots() > 0);
    REQUIRE(tileId.value() < 0);
}

TEST_CASE("Grid traffic metadata and locate expose the traffic layer", "[gridsource][traffic]")
{
    GridDataSource source(trafficConfig());
    const auto info = source.info();
    const auto layer = info.getLayer("DevSrc-TrafficLayer");
    REQUIRE(layer != nullptr);
    REQUIRE(layer->zoomLevels_ == std::vector<int>{13});
    REQUIRE(layer->featureModelSchema_ != nullptr);

    const auto trafficType = std::find_if(
        layer->featureTypes_.begin(),
        layer->featureTypes_.end(),
        [](const auto& type) { return type.name_ == "DevSrc-Traffic"; });
    REQUIRE(trafficType != layer->featureTypes_.end());
    REQUIRE(trafficType->uniqueIdCompositions_.front().back().idPartLabel_ ==
        "DevSrc-TrafficId");

    const auto tileId = TileId::fromWgs84(11.0, 48.0, 13).value();
    auto candidates = source.locate(LocateRequest{
        "GridTrafficTest",
        "DevSrc-Traffic",
        {{"tileId", int64_t{tileId}}, {"DevSrc-TrafficId", int64_t{1000}}}});
    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates.front().tileKey_.layerId_ == "DevSrc-TrafficLayer");
    REQUIRE(candidates.front().tileKey_.tileId_.value() == tileId);
}

TEST_CASE("Grid traffic snapshots are stable within an epoch", "[gridsource][traffic]")
{
    auto now = std::chrono::system_clock::time_point{100s};
    GridDataSource source(trafficConfig(), [&] { return now; });
    const auto info = source.info();

    auto first = makeTile(info, "DevSrc-TrafficLayer");
    auto second = makeTile(info, "DevSrc-TrafficLayer", first->tileId());
    source.fill(first);
    source.fill(second);

    REQUIRE_FALSE(first->error());
    REQUIRE(first->numRoots() > 0);
    REQUIRE(first->toJson()["features"] == second->toJson()["features"]);
    REQUIRE(first->timestamp() == std::chrono::system_clock::time_point{100s});
    REQUIRE(first->ttl() == 5s);
    REQUIRE_NOTHROW(first->validateSchema());

    const auto firstJson = first->toJson();
    const auto& feature = firstJson["features"].front();
    REQUIRE(feature["properties"]["trafficEpoch"] == 20);
    REQUIRE(feature["properties"]["relativeSpeedPercent"].get<int>() >= 0);
    REQUIRE(feature["properties"]["relativeSpeedPercent"].get<int>() <= 100);
    REQUIRE(feature["relations"].size() == 1);
    for (const auto& current : firstJson["features"]) {
        const auto average = current["properties"]["estimatedAverageSpeedKph"].get<int>();
        const auto freeFlow = current["properties"]["freeFlowSpeedKph"].get<int>();
        const auto relative = current["properties"]["relativeSpeedPercent"].get<int>();
        REQUIRE(average >= 0);
        REQUIRE(freeFlow > 0);
        REQUIRE(average <= freeFlow);
        REQUIRE(relative >= 0);
        REQUIRE(relative <= 100);
    }

    now = std::chrono::system_clock::time_point{105s};
    auto next = makeTile(info, "DevSrc-TrafficLayer", first->tileId());
    source.fill(next);
    REQUIRE(next->numRoots() == first->numRoots());
    REQUIRE(next->timestamp() == std::chrono::system_clock::time_point{105s});
    REQUIRE(next->toJson()["features"].front()["properties"]["trafficEpoch"] == 21);
    REQUIRE(next->toJson()["features"].front()["id"] == feature["id"]);
    REQUIRE(next->toJson()["features"].front()["geometry"] == feature["geometry"]);
}

TEST_CASE("Grid traffic generation is thread-safe and bounded below its minimum cadence", "[gridsource][traffic]")
{
    const auto now = std::chrono::system_clock::time_point{100s};
    GridDataSource source(trafficConfig(), [&] { return now; });
    const auto info = source.info();
    const auto tileId = TileId::fromWgs84(11.0, 48.0, 13);
    auto road = makeTile(info, "DevSrc-RoadLayer", tileId);
    auto traffic = makeTile(info, "DevSrc-TrafficLayer", tileId);
    const auto started = std::chrono::steady_clock::now();
    auto roadFill = std::async(std::launch::async, [&] { source.fill(road); });
    auto trafficFill = std::async(std::launch::async, [&] { source.fill(traffic); });
    roadFill.get();
    trafficFill.get();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(road->error());
    REQUIRE_FALSE(traffic->error());
    REQUIRE(road->numRoots() == traffic->numRoots());
    REQUIRE(elapsed < 1s);

    std::vector<std::future<TileFeatureLayer::Ptr>> fanout;
    for (size_t index = 0; index < 8; ++index) {
        fanout.push_back(std::async(std::launch::async, [&source, &info, tileId] {
            auto tile = makeTile(info, "DevSrc-TrafficLayer", tileId);
            source.fill(tile);
            return tile;
        }));
    }
    for (auto& future : fanout) {
        auto tile = future.get();
        REQUIRE_FALSE(tile->error());
        REQUIRE(tile->timestamp() == std::chrono::system_clock::time_point{100s});
        REQUIRE(tile->numRoots() == traffic->numRoots());
    }
}

TEST_CASE("Grid traffic corrects a bucket crossed during sampling", "[gridsource][traffic]")
{
    size_t clockCall = 0;
    GridDataSource source(trafficConfig(), [&] {
        return clockCall++ == 0
            ? std::chrono::system_clock::time_point{104999ms}
            : std::chrono::system_clock::time_point{105000ms};
    });
    const auto info = source.info();
    auto tile = makeTile(info, "DevSrc-TrafficLayer");
    source.fill(tile);

    REQUIRE(tile->timestamp() == std::chrono::system_clock::time_point{105s});
    for (const auto& feature : tile->toJson()["features"]) {
        REQUIRE(feature["properties"]["trafficEpoch"] == 21);
    }
}

TEST_CASE("Grid traffic rejects unsupported request levels without renewal", "[gridsource][traffic]")
{
    GridDataSource source(trafficConfig());
    const auto info = source.info();
    auto tile = makeTile(
        info,
        "DevSrc-TrafficLayer",
        TileId::fromWgs84(11.0, 48.0, 12));
    source.fill(tile);

    REQUIRE(tile->error());
    REQUIRE(tile->numRoots() == 0);
    REQUIRE(tile->ttl() == 0ms);
}
