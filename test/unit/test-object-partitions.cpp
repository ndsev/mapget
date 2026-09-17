#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <future>
#include <sstream>
#include "../../libs/http-service/src/tiles-request-json.h"
#include "mapget/http-service/http-client.h"
#include "mapget/model/featureid.h"
#include "mapget/model/sourcedata.h"
#include "mapget/model/stringpool.h"
#include "mapget/service/service.h"
#include "test-http-service-fixture.h"

using namespace mapget;

namespace
{
/** Small object datasource exercising repeated feature IDs and shared discovery associations. */
class ObjectSource : public DataSource
{
public:
    std::atomic_size_t fills{0};
    std::atomic_size_t discoveries{0};
    std::atomic_size_t locates{0};
    DataSourceInfo metadata = DataSourceInfo::fromJson(nlohmann::json::parse(R"({
        "mapId":"Objects", "stringPoolId":"ObjectPool", "maxParallelJobs":1,
        "layers":{"Road":{"partitionKind":"object", "tileAssociationLevel":13,
            "featureTypes":[{"name":"Road","uniqueIdCompositions":[[
                {"partId":"tileId","datatype":"U64"},{"partId":"localId","datatype":"U32"}]]}]}}
    })"));
    /** Return immutable layer metadata; no discovery is performed during startup. */
    DataSourceInfo info() override { return metadata; }
    /** Populate an object using an explicit geometry anchor and container-scoped feature ID. */
    void fill(TileFeatureLayer::Ptr const& layer) override
    {
        ++fills;
        layer->setGeometryAnchor(Point{11, 48, 0});
        auto feature = layer->newFeature(
            "Road",
            {{"tileId", std::bit_cast<int64_t>(layer->partitionId().objectId())},
             {"localId", int64_t{1}}});
        auto geometry = layer->newGeometry(GeomType::Points, 1);
        geometry->append({11, 48, 0});
        feature->addGeometry(geometry);
        auto peer = layer->newFeature(
            "Road",
            {{"tileId", std::bit_cast<int64_t>(layer->partitionId().objectId())},
             {"localId", int64_t{2}}});
        peer->addPoint({11.1, 48.1, 0});
        feature->addRelation(
            "local",
            "Road",
            {{"tileId", std::bit_cast<int64_t>(layer->partitionId().objectId())},
             {"localId", int64_t{2}}});
        peer->addRelation(
            "local",
            "Road",
            {{"tileId", std::bit_cast<int64_t>(layer->partitionId().objectId())},
             {"localId", int64_t{1}}});
        feature->addRelation("external", "Road", {{"tileId", int64_t{2}}, {"localId", int64_t{1}}});
    }
    /** Source-data conversion uses the same destination-owned partition identity. */
    void fill(TileSourceDataLayer::Ptr const&) override {}
    /** Any object-filter call to locate would indicate forbidden cross-object expansion. */
    std::vector<LocateCandidate> locate(LocateRequest const&) override
    {
        ++locates;
        return {};
    }
    /** Two discovery tiles deliberately reference the same object. */
    ObjectDiscoveryResult discoverObjects(ObjectDiscoveryRequest const&) override
    {
        ++discoveries;
        ObjectDiscoveryResult result;
        result.objects_ = {{UINT64_MAX, std::array<double, 4>{10, 47, 12, 49}}};
        result.ttl_ = std::chrono::seconds{5};
        return result;
    }
};
}  // namespace

TEST_CASE("Partition identities retain kind and full unsigned object values", "[objects]")
{
    auto const value = GENERATE(
        uint64_t{0},
        uint64_t{131073},
        uint64_t{9007199254740993ULL},
        uint64_t{9223372036854775808ULL},
        UINT64_MAX);
    auto id = PartitionId::object(value);
    CHECK(id.objectId() == value);
    CHECK_THROWS(id.tileId());
    CHECK_THROWS(id.value());
    CHECK(PartitionId::fromJson(id.toJson()) == id);
    MapPartitionKey key(LayerType::Features, "Map/A", "Road", id);
    CHECK(MapPartitionKey(key.toString()) == key);
    CHECK(key != MapPartitionKey(LayerType::Features, "Map/A", "Road", TileId::fromValue(131073)));
    CHECK_THROWS(PartitionId::fromJson({{"kind", "object"}, {"id", value}}));
    CHECK_THROWS(PartitionId::fromJson({{"kind", "object"}, {"id", "18446744073709551616"}}));
    CHECK_THROWS(PartitionId::fromJson({{"kind", "object"}, {"id", "-1"}}));
}

TEST_CASE("Object models roundtrip unsigned feature IDs and explicit geometry anchors", "[objects]")
{
    ObjectSource source;
    auto id = PartitionId::object(UINT64_MAX);
    auto info = source.metadata.layers_.at("Road");
    auto pool = std::make_shared<StringPool>("ObjectPool");
    auto layer = std::make_shared<PartitionFeatureLayer>(id, "ObjectPool", "Objects", info, pool);
    source.fill(layer);
    auto const canonical = std::string("Road.18446744073709551615.1");
    auto feature = layer->find(canonical);
    REQUIRE(feature);
    CHECK(feature->id()->toString() == canonical);
    CHECK(std::get<int64_t>(feature->id()->keyValuePairs().front().second) == -1);
    CHECK_THROWS(layer->tileId());
    CHECK(layer->toJson().at("partition") == id.toJson());
    CHECK_FALSE(layer->toJson().contains("mapgetTileId"));
    auto imported =
        std::make_shared<PartitionFeatureLayer>(id, "ObjectPool", "Objects", info, pool);
    imported->fromJson(layer->toJson());
    CHECK(imported->find(canonical));
    std::ostringstream bytes;
    REQUIRE(layer->write(bytes));
    auto data = bytes.str();
    auto decoded = std::make_shared<PartitionFeatureLayer>(
        std::vector<uint8_t>(data.begin(), data.end()),
        [&](auto const&, auto const&) { return info; },
        [&](auto const&) { return pool; });
    CHECK(decoded->partitionId() == id);
    CHECK(decoded->geometryAnchor().x == 11);
    CHECK(decoded->find(canonical));
    CHECK_THROWS(
        PartitionFeatureLayer(TileId::fromValue(131073), "ObjectPool", "Objects", info, pool));

    auto rawInfo = std::make_shared<LayerInfo>(*info);
    rawInfo->type_ = LayerType::SourceData;
    auto raw =
        std::make_shared<PartitionSourceDataLayer>(id, "ObjectPool", "Objects", rawInfo, pool);
    raw->addRoot(raw->newCompound(0));
    std::ostringstream rawBytes;
    REQUIRE(raw->write(rawBytes));
    auto rawData = rawBytes.str();
    auto rawDecoded = std::make_shared<PartitionSourceDataLayer>(
        std::vector<uint8_t>(rawData.begin(), rawData.end()),
        [&](auto const&, auto const&) { return rawInfo; },
        [&](auto const&) { return pool; });
    CHECK(rawDecoded->partitionId() == id);
    CHECK(rawDecoded->toJson() == raw->toJson());
}

TEST_CASE(
    "Object discovery and loading share service scheduling but independent identities",
    "[objects]")
{
    auto source = std::make_shared<ObjectSource>();
    Service service(std::make_shared<MemCache>(), false, std::chrono::milliseconds{0}, 2);
    service.add(source);
    for (auto tile : {TileId::fromTileXY(1, 1, 13), TileId::fromTileXY(2, 1, 13)}) {
        std::promise<ObjectDiscoveryResult> promise;
        service.discoverObjects(
            {"Objects", "Road", tile},
            [&](auto result) { promise.set_value(std::move(result)); });
        auto future = promise.get_future();
        REQUIRE(future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
        auto result = future.get();
        CHECK(result.status_ == ObjectDiscoveryResult::Status::Success);
        REQUIRE(result.objects_.size() == 1);
        CHECK(result.objects_.front().objectId_ == UINT64_MAX);
        CHECK(ObjectDiscoveryResult::fromJson(result.toJson()).toJson() == result.toJson());
    }
    CHECK(source->fills == 0);
    for (int repeat = 0; repeat < 2; ++repeat) {
        auto request = std::make_shared<
            LayerTilesRequest>("Objects", "Road", std::vector{PartitionId::object(UINT64_MAX)});
        PartitionFeatureLayer::Ptr received;
        request->onFeatureLayer([&](auto layer) { received = std::move(layer); });
        REQUIRE(service.request({request}));
        request->wait();
        REQUIRE(received);
        CHECK(received->partitionId() == PartitionId::object(UINT64_MAX));
    }
    CHECK(source->fills == 1);
    FeatureLayerFilterRequest filter;
    filter.filterId_ = "objects";
    filter.channels_ = {
        FeatureLayerFilterChannel{.channelId_ = "features", .entryFilter_ = "true"}};
    auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
        "Objects",
        "Road",
        std::vector{PartitionId::object(UINT64_MAX)},
        filter);
    PartitionSubsetLayer::Ptr subset;
    request->onFilterResult([&](auto layer) { subset = std::move(layer); });
    REQUIRE(service.request(request));
    request->wait();
    REQUIRE(subset);
    CHECK(subset->partitionId() == PartitionId::object(UINT64_MAX));
    CHECK(source->fills == 1);
    std::ostringstream bytes;
    REQUIRE(subset->write(bytes));
    auto data = bytes.str();
    auto decoded = std::make_shared<PartitionSubsetLayer>(
        std::vector<uint8_t>(data.begin(), data.end()),
        [&](auto const&, auto const&) { return subset->layerInfo(); },
        [&](auto const&) { return subset->strings(); });
    CHECK(decoded->partitionId() == subset->partitionId());
    CHECK(decoded->dependencies() == subset->dependencies());
    CHECK(decoded->toJson() == subset->toJson());
}

TEST_CASE("Object requests use the common HTTP and interactive parser", "[objects]")
{
    auto parsed = detail::parseLayerTilesRequestJson(
        {{"mapId", "Objects"},
         {"layerId", "Road"},
         {"partitions", nlohmann::json::array({PartitionId::object(UINT64_MAX).toJson()})}});
    REQUIRE(parsed.tileIds.size() == 1);
    CHECK(parsed.tileIds.front().objectId() == UINT64_MAX);
    CHECK_THROWS(detail::parseLayerTilesRequestJson(
        {{"mapId", "Objects"},
         {"layerId", "Road"},
         {"tileIds", nlohmann::json::array({131073})},
         {"partitions", nlohmann::json::array()}}));
}

TEST_CASE("Object filters keep point groups and relations within one partition", "[objects]")
{
    auto source = std::make_shared<ObjectSource>();
    Service service(std::make_shared<MemCache>(), false, std::chrono::milliseconds{0}, 2);
    service.add(source);
    FeatureLayerFilterRequest filter;
    filter.filterId_ = "local-only";
    filter.channels_ = {
        FeatureLayerFilterChannel{
            .channelId_ = "groups",
            .geometryTypes_ = uint32_t{1} << static_cast<uint8_t>(GeomType::Points),
            .group_ = FeatureLayerPointGridGroup{.origin_ = {0, 0, 0}, .cellSize_ = {1, 1, 1}}},
        FeatureLayerFilterChannel{
            .channelId_ = "relations",
            .scope_ = FeatureLayerFilterScope::Relation,
            .relation_ = FeatureLayerStoredRelationOptions{
                .relationNamePattern_ = ".*",
                .mergeTwoway_ = true}}};
    auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
        "Objects",
        "Road",
        std::vector{PartitionId::object(UINT64_MAX)},
        filter);
    PartitionSubsetLayer::Ptr subset;
    request->onFilterResult([&](auto result) { subset = std::move(result); });
    REQUIRE(service.request(request));
    request->wait();
    REQUIRE(subset);
    INFO(subset->toJson().dump());
    CHECK(source->fills == 1);
    CHECK(source->locates == 0);
    CHECK(subset->geometryAnchor().x == 11);
    size_t groups = 0, relations = 0;
    subset->forEachChannel(
        [&](auto const& channel)
        {
            groups += channel->groupEntryCount();
            relations += channel->relationEntryCount();
            return true;
        });
    CHECK(groups == 1);
    CHECK(relations == 1);
    REQUIRE(subset->dependencies().size() == 1);
    CHECK(
        subset->dependencies().front().sourceTileKey_.partitionId_ ==
        PartitionId::object(UINT64_MAX));
}

TEST_CASE(
    "Object discovery validates metadata, authorization and missing versus empty",
    "[objects]")
{
    auto source = std::make_shared<ObjectSource>();
    source->requireAuthHeaderRegexMatchOption("Authorization", std::regex("allowed"));
    Service service(std::make_shared<MemCache>(), false, std::chrono::milliseconds{0}, 1);
    service.add(source);
    auto query = [&](TileId tile, AuthHeaders headers)
    {
        std::promise<ObjectDiscoveryResult> promise;
        auto future = promise.get_future();
        service.discoverObjects(
            {"Objects", "Road", tile},
            [&](auto result) { promise.set_value(std::move(result)); },
            headers);
        REQUIRE(future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
        return future.get();
    };
    CHECK(query(TileId::fromTileXY(1, 1, 13), {}).status_ == ObjectDiscoveryResult::Status::Failed);
    CHECK(
        query(TileId::fromTileXY(1, 1, 12), {{"Authorization", "allowed"}}).status_ ==
        ObjectDiscoveryResult::Status::Failed);
    CHECK(source->discoveries == 0);
    CHECK(
        query(TileId::fromTileXY(1, 1, 13), {{"Authorization", "allowed"}}).status_ ==
        ObjectDiscoveryResult::Status::Success);
    ObjectDiscoveryResult missing;
    missing.status_ = ObjectDiscoveryResult::Status::Unavailable;
    CHECK(
        ObjectDiscoveryResult::fromJson(missing.toJson()).status_ ==
        ObjectDiscoveryResult::Status::Unavailable);
    missing.status_ = ObjectDiscoveryResult::Status::Success;
    CHECK(ObjectDiscoveryResult::fromJson(missing.toJson()).objects_.empty());
    CHECK_THROWS(PartitionId::fromJson({{"kind", "tile"}, {"id", UINT64_MAX}}));
    auto invalid = *source->metadata.layers_.at("Road");
    invalid.tileAssociationLevel_ = 16;
    CHECK_THROWS(invalid.validateIdentifiers());
}

TEST_CASE(
    "HTTP discovery and object loading preserve full unsigned identities",
    "[objects][HttpDataSource]")
{
    auto source = std::make_shared<ObjectSource>();
    auto& service = test::httpService();
    service.add(source);
    HttpClient client("127.0.0.1", service.port());
    auto result = client.discoverObjects({"Objects", "Road", TileId::fromTileXY(1, 1, 13)});
    REQUIRE(result.status_ == ObjectDiscoveryResult::Status::Success);
    REQUIRE(result.objects_.size() == 1);
    CHECK(result.objects_.front().objectId_ == UINT64_MAX);
    CHECK(source->fills == 0);
    auto request = std::make_shared<
        LayerTilesRequest>("Objects", "Road", std::vector{PartitionId::object(UINT64_MAX)});
    PartitionFeatureLayer::Ptr received;
    request->onFeatureLayer([&](auto layer) { received = std::move(layer); });
    client.request(request)->wait();
    REQUIRE(received);
    CHECK(received->partitionId().objectId() == UINT64_MAX);
    service.remove(source);
}
