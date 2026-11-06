#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "mapget/model/mapschema.h"
#include "mapget/model/sourcedata.h"
#include "mapget/service/memcache.h"
#include "mapget/service/service.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <sstream>
#include <stdexcept>

using namespace mapget;
using namespace std::chrono_literals;

namespace
{
/** Independent producer contracts with complementary fields and an add-on-only layer. */
class CompositionSource : public DataSource
{
public:
    DataSourceInfo metadata;
    MapSchema::Ptr graph;
    simfil::TypeId valueDomain = 0, flagsDomain = 0, rawDomain = 0;
    std::atomic_size_t featureLoads = 0, rawLoads = 0;

    explicit CompositionSource(
        bool addon = false,
        std::vector<std::string> const& additionalLayers = {})
        : graph(std::make_shared<MapSchema>(MapFormat{"composition", "1", {}}))
    {
        auto identity = FeatureTypeInfo::fromJson(nlohmann::json::parse(R"({
            "name":"Thing","uniqueIdCompositions":[[{"partId":"thingId","datatype":"U32"}]]
        })"));
        auto layer = graph->newLayer("Things", LayerType::Features);
        auto root = layer->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey("Thing"));
        auto properties = layer->addSchema(
            simfil::SchemaType::Kind::Object,
            LayerSchema::featurePropertiesKey("Thing"));
        graph->schemaType(root).asObject().addField("properties", graph->schemaType(properties));
        valueDomain = layer->addSchema(
            addon ? simfil::SchemaType::Kind::String : simfil::SchemaType::Kind::Int);
        graph->schemaType(properties)
            .asObject()
            .addField(addon ? "label" : "count", graph->schemaType(valueDomain));
        if (!addon) {
            flagsDomain = layer->addSchema(simfil::SchemaType::Kind::Bitmask);
            graph->schemaType(flagsDomain).asBitmask().setBitWidth(64);
            graph->schemaType(properties)
                .asObject()
                .addField("flags", graph->schemaType(flagsDomain));
        }
        layer->addFeatureType(identity, root);
        layer->setRootSchema(root);
        if (addon) {
            auto extra = graph->newLayer("Extra", LayerType::Features);
            extra->addFeatureType(identity, root);
            extra->setRootSchema(root);
        }
        else {
            auto raw = graph->newLayer("Raw", LayerType::SourceData);
            rawDomain = raw->addSchema(simfil::SchemaType::Kind::Object);
            graph->schemaType(rawDomain)
                .asObject()
                .addField("count", graph->schemaType(valueDomain));
            raw->setRootSchema(rawDomain);
        }
        for (auto const& name : additionalLayers) {
            auto additional = graph->newLayer(name, LayerType::Features);
            additional->addFeatureType(identity, root);
            additional->setRootSchema(root);
        }
        graph->finalize();
        metadata.mapId_ = "CompositionMap";
        metadata.stringPoolId_ = addon ? "CompositionAddon" : "CompositionBase";
        metadata.maxParallelJobs_ = 1;
        metadata.isAddOn_ = addon;
        for (auto const& name : graph->layerNames()) {
            auto info = std::make_shared<LayerInfo>();
            info->layerId_ = name;
            info->type_ = graph->layer(name)->layerType();
            metadata.layers_[name] = std::move(info);
        }
        metadata.setMapSchema(graph);
    }

    DataSourceInfo info() override { return metadata; }

    void fill(PartitionFeatureLayer::Ptr const& tile) override
    {
        ++featureLoads;
        if (tile->schemaId() != graph->schemaId())
            throw std::logic_error("Provider received composed schema IDs during conversion.");
        auto feature = tile->newFeature("Thing", {{"thingId", 1}});
        auto properties = feature->attributes();
        if (metadata.isAddOn_) {
            if (auto added = properties->addField("label", "overlay"); !added)
                throw std::runtime_error(added.error().message);
        }
        else {
            if (auto added = properties->addField(
                    "count",
                    tile->newQualifiedValue(int64_t{255}, graph->schemaType(valueDomain)));
                !added)
                throw std::runtime_error(added.error().message);
            if (auto added = properties->addField(
                    "flags",
                    tile->newBitmask(UINT64_C(0x8000000000000001), graph->schemaType(flagsDomain)));
                !added)
                throw std::runtime_error(added.error().message);
            feature->addLine({{11, 48, 0}, {11.1, 48.1, 0}});
        }
    }

    void fill(PartitionSourceDataLayer::Ptr const& tile) override
    {
        ++rawLoads;
        if (tile->schemaId() != graph->schemaId())
            throw std::logic_error("Raw provider received composed schema IDs during conversion.");
        auto root = tile->newCompound(1);
        root->setSchema(rawDomain);
        root->setSchemaName("NativeRaw");
        root->setSourceDataAddress({32, 64});
        root->setSourceDataAddressScope();
        if (auto added = root->object()->addField(
                "count",
                tile->newQualifiedValue(int64_t{255}, graph->schemaType(valueDomain)));
            !added)
            throw std::runtime_error(added.error().message);
        tile->addRoot(root);
    }

    /** Each call creates an independent native owner, using the ordinary conversion path. */
    PartitionLayer::Ptr native(std::string const& name, Cache::Ptr& cache)
    {
        return get(
            MapPartitionKey(
                metadata.getLayer(name)->type_,
                metadata.mapId_,
                name,
                TileId::fromTileXY(1, 1, 2)),
            cache,
            metadata);
    }
};

/** Fail one add-on read, then use the unchanged native conversion on its retry. */
class TransientCompositionSource : public CompositionSource
{
public:
    bool returnNull = false;
    std::optional<std::chrono::milliseconds> failureTtl;
    std::atomic_size_t attempts = 0;

    /** Reuse the additive contract and extra layer of the ordinary composition fixture. */
    TransientCompositionSource() : CompositionSource(true) {}

    /** Model both error tiles and providers which fail to return a tile at all. */
    PartitionLayer::Ptr get(
        MapPartitionKey const& key,
        Cache::Ptr& cache,
        DataSourceInfo const& info,
        PartitionLayer::LoadStateCallback callback = {},
        TileCancellationCheck const& cancelled = {}) override
    {
        auto result = DataSource::get(key, cache, info, std::move(callback), cancelled);
        if (++attempts == 1) {
            if (returnNull)
                return {};
            result->setError("Transient add-on failure");
            result->setTtl(failureTtl);
        }
        return result;
    }
};

/** Preserve the exact binary path rather than testing only a JSON projection. */
std::vector<uint8_t> encode(PartitionLayer& layer)
{
    std::ostringstream output;
    auto result = layer.write(output);
    if (!result)
        throw std::runtime_error(result.error().message);
    auto bytes = output.str();
    return {bytes.begin(), bytes.end()};
}
}  // namespace

TEST_CASE(
    "Native subtree clones share exact artifacts without retaining an unrelated graph",
    "[schema2][composition][layer-binding]")
{
    CompositionSource destination;
    Cache::Ptr cache = std::make_shared<MemCache>(8);
    auto target =
        std::static_pointer_cast<PartitionFeatureLayer>(destination.native("Things", cache));
    simfil::ModelNode::Ptr copied;
    nlohmann::json expected;
    std::weak_ptr<MapSchema const> oldOwner;
    {
        CompositionSource source;
        REQUIRE(source.graph.get() != destination.graph.get());
        REQUIRE(source.graph->schemaId() == destination.graph->schemaId());
        oldOwner = source.graph;
        auto tile = std::static_pointer_cast<PartitionFeatureLayer>(source.native("Things", cache));
        auto properties = tile->at(0)->attributesOrNull();
        expected = properties->toJson();
        PartitionFeatureLayer::CloneCache clones;
        copied = target->clone(clones, tile, properties);
        REQUIRE(copied);
        CHECK(copied->owningModel().get() == target.get());
        CHECK(copied->schemaType().pool()->identity() == destination.graph->schemaId());
    }
    CHECK(oldOwner.expired());
    CHECK(copied->toJson() == expected);

    // Native partitions have one exact graph. Only a composed destination may
    // translate its known contributors; generic ModelPool copying is separate.
    CompositionSource unrelated(true);
    auto tile = std::static_pointer_cast<PartitionFeatureLayer>(unrelated.native("Things", cache));
    PartitionFeatureLayer::CloneCache clones;
    CHECK_THROWS(target->clone(clones, tile, tile->at(0)->attributesOrNull()));
    CHECK(target->schemaId() == destination.graph->schemaId());
    CHECK(copied->toJson() == expected);
}

TEST_CASE(
    "Fresh feature and source-data models translate without copying payload addresses",
    "[schema2][composition]")
{
    CompositionSource base, addon(true);
    std::vector<MapSchema::ConstPtr> owners{base.graph, addon.graph};
    auto effective = MapSchema::compose(base.graph->format(), owners);
    auto metadata = base.metadata;
    metadata.schemaId_.clear();
    metadata.mapFormat_ = {};
    metadata.setMapSchema(effective);
    Cache::Ptr cache = std::make_shared<MemCache>();

    SECTION("Feature columns, qualified scalars, masks and independent owners")
    {
        auto native = std::static_pointer_cast<PartitionFeatureLayer>(base.native("Things", cache));
        auto independent =
            std::static_pointer_cast<PartitionFeatureLayer>(base.native("Things", cache));
        auto before = native->toJson();
        REQUIRE(before["features"][0]["id"] == "Thing.1");
        REQUIRE(before["features"][0]["thingId"] == 1);
        auto& maskJson = before["features"][0]["properties"]["flags"];
        REQUIRE(
            maskJson ==
            nlohmann::json{
                {"_bitmask", true},
                {"bits", "0x8000000000000001"},
                {"schema", base.graph->schemaId()},
                {"typeId", base.flagsDomain}});
        auto feature = native->at(0);
        auto featureAddress = feature->addr();
        auto const beforeGeometryErrors = native->checkForErrors();
        auto geometryAddress = feature->geom()->addr();
        auto const afterGeometryErrors = native->checkForErrors();
        CAPTURE(beforeGeometryErrors, afterGeometryErrors);
        CHECK(beforeGeometryErrors.empty());
        CHECK(afterGeometryErrors.empty());
        auto scalarAddress = feature->attributes()->at(0)->addr();
        auto maskAddress = feature->attributes()->at(1)->addr();
        native->rebindSchema(metadata.getLayer("Things"));
        maskJson["schema"] = effective->schemaId();
        maskJson["typeId"] = effective->remapSchema(base.graph->schemaId(), base.flagsDomain);
        CHECK(native->schemaId() == effective->schemaId());
        CHECK(native->toJson() == before);
        CHECK(native->at(0)->addr() == featureAddress);
        CHECK(native->at(0)->geom()->addr() == geometryAddress);
        CHECK(
            native->resolve(scalarAddress)->schemaType().pool()->identity() ==
            effective->schemaId());
        CHECK(
            native->resolve(maskAddress)->schemaType().pool()->identity() == effective->schemaId());
        CHECK(independent->schemaId() == base.graph->schemaId());
        CHECK(
            independent->at(0)->attributes()->at(0)->schemaType().pool()->identity() ==
            base.graph->schemaId());

        auto restored = std::make_shared<PartitionFeatureLayer>(
            encode(*native),
            [&](auto const&, auto const& name) { return metadata.getLayer(std::string(name)); },
            [&](auto const&) { return native->strings(); });
        CHECK(restored->schemaId() == effective->schemaId());
        CHECK(restored->toJson() == before);
        auto const errors = restored->checkForErrors();
        CAPTURE(errors);
        CHECK(errors.empty());
    }

    SECTION("Source-data compound identity and source address scopes")
    {
        auto native = std::static_pointer_cast<PartitionSourceDataLayer>(base.native("Raw", cache));
        auto root = native->resolve<SourceDataCompoundNode>(native->root(0).value());
        auto address = root->addr();
        auto json = native->toJson();
        native->rebindSchema(metadata.getLayer("Raw"));
        auto rebound = native->resolve<SourceDataCompoundNode>(address);
        CHECK(rebound->schemaType().pool()->identity() == effective->schemaId());
        CHECK(rebound->sourceDataAddress().u64() == SourceDataAddress(32, 64).u64());
        CHECK(rebound->isSourceDataAddressScope());
        CHECK(rebound->schemaName() == "NativeRaw");
        CHECK(native->toJson() == json);
        auto restored = std::make_shared<PartitionSourceDataLayer>(
            encode(*native),
            [&](auto const&, auto const& name) { return metadata.getLayer(std::string(name)); },
            [&](auto const&) { return native->strings(); });
        CHECK(restored->schemaId() == effective->schemaId());
        CHECK(restored->toJson() == json);
        CHECK(restored->resolve<SourceDataCompoundNode>(address)->isSourceDataAddressScope());
    }

    SECTION("An unrelated graph cannot change a populated model")
    {
        auto native = std::static_pointer_cast<PartitionFeatureLayer>(base.native("Things", cache));
        auto before = encode(*native);
        CHECK_THROWS(native->rebindSchema(addon.metadata.getLayer("Things")));
        CHECK(encode(*native) == before);
        CHECK(native->schemaId() == base.graph->schemaId());
    }
}

TEST_CASE(
    "Service composes independently converted tiles before cache publication",
    "[schema2][composition][Service]")
{
    Service service(std::make_shared<MemCache>(8), false);
    auto base = std::make_shared<CompositionSource>();
    auto addon = std::make_shared<CompositionSource>(true);
    service.add(base);
    service.add(addon);
    auto infos = service.info();
    auto primary = std::ranges::find_if(infos, [](auto const& info) { return !info.isAddOn_; });
    REQUIRE(primary != infos.end());
    REQUIRE(primary->schemaId_ != base->metadata.schemaId_);

    auto loadFeature = [&](std::string const& name)
    {
        PartitionFeatureLayer::Ptr result;
        auto request = std::make_shared<LayerTilesRequest>(
            "CompositionMap",
            name,
            std::vector<PartitionId>{TileId::fromTileXY(1, 1, 2)});
        request->onFeatureLayer([&](auto layer) { result = std::move(layer); });
        REQUIRE(service.request(std::vector{request}));
        request->wait();
        REQUIRE(request->getStatus() == RequestStatus::Success);
        REQUIRE(result);
        return result;
    };
    auto result = loadFeature("Things");
    CHECK(result->schemaId() == primary->schemaId_);
    REQUIRE(result->size() == 1);
    auto properties = result->at(0)->attributesOrNull()->toJson();
    CHECK(properties["count"] == 255);
    CHECK(properties["label"] == "overlay");
    CHECK(properties.contains("flags"));
    CHECK(result->checkForErrors().empty());
    auto cached = loadFeature("Things");
    CHECK(cached->toJson() == result->toJson());
    CHECK(base->featureLoads == 1);
    CHECK(addon->featureLoads == 1);

    auto extra = loadFeature("Extra");
    CHECK(extra->schemaId() == primary->schemaId_);
    REQUIRE(extra->size() == 1);
    CHECK(extra->at(0)->attributesOrNull()->toJson()["label"] == "overlay");
    CHECK(base->featureLoads == 1);
    CHECK(addon->featureLoads == 2);

    PartitionSourceDataLayer::Ptr raw;
    auto request = std::make_shared<LayerTilesRequest>(
        "CompositionMap",
        "Raw",
        std::vector<PartitionId>{TileId::fromTileXY(1, 1, 2)});
    request->onSourceDataLayer([&](auto layer) { raw = std::move(layer); });
    REQUIRE(service.request(std::vector{request}));
    request->wait();
    REQUIRE(raw);
    CHECK(raw->schemaId() == primary->schemaId_);
    CHECK(base->rawLoads == 1);
    CHECK(addon->rawLoads == 0);
}

TEST_CASE(
    "Partial add-ons do not participate in unadvertised base layers",
    "[schema2][composition][Service][MAPV-1152]")
{
    auto addonFirst = GENERATE(false, true);
    Service service(std::make_shared<MemCache>(16), false);
    auto base = std::make_shared<CompositionSource>(
        false,
        std::vector<std::string>{"Road", "Lane", "Display2D", "POI", "Landmark"});
    auto addon =
        std::make_shared<CompositionSource>(true, std::vector<std::string>{"Road", "Lane"});
    service.add(addonFirst ? addon : base);
    service.add(addonFirst ? base : addon);

    auto load = [&](std::string const& name)
    {
        PartitionFeatureLayer::Ptr result;
        auto request = std::make_shared<LayerTilesRequest>(
            "CompositionMap",
            name,
            std::vector<PartitionId>{TileId::fromTileXY(1, 1, 2)});
        request->onFeatureLayer([&](auto tile) { result = std::move(tile); });
        REQUIRE(service.request(std::vector{request}));
        request->wait();
        REQUIRE(request->getStatus() == RequestStatus::Success);
        REQUIRE(result);
        REQUIRE_FALSE(result->error());
        REQUIRE(result->size() == 1);
        CHECK(result->checkForErrors().empty());
        return result->at(0)->attributesOrNull()->toJson();
    };
    for (auto const* name : {"Display2D", "POI", "Landmark"}) {
        CAPTURE(name);
        auto properties = load(name);
        CHECK(properties["count"] == 255);
        CHECK_FALSE(properties.contains("label"));
        CHECK(load(name) == properties);
        CHECK(addon->featureLoads == 0);
    }
    CHECK(base->featureLoads == 3);
    for (auto const* name : {"Road", "Lane"}) {
        CAPTURE(name);
        auto properties = load(name);
        CHECK(properties["count"] == 255);
        CHECK(properties["label"] == "overlay");
        CHECK(load(name) == properties);
    }
    CHECK(base->featureLoads == 5);
    CHECK(addon->featureLoads == 2);
}

TEST_CASE(
    "Add-on publication isolates failing invalidation callbacks",
    "[schema2][composition][Service][source-lifecycle]")
{
    auto const standardException = GENERATE(true, false);
    std::vector<RequestStatus> completions;
    std::vector<DataSourceCatalogChange> changes;
    Service service(std::make_shared<MemCache>(8), false, 0ms, 1);
    auto base = std::make_shared<CompositionSource>();
    auto addon = std::make_shared<CompositionSource>(true);
    service.add(base);
    auto subscription = service.subscribeToSourceCatalogChanges(
        [&](auto const& change) { changes.push_back(change); });

    std::vector<LayerTilesRequest::Ptr> requests;
    for (int i = 0; i < 2; ++i) {
        auto request = std::make_shared<LayerTilesRequest>(
            "CompositionMap", "Things", std::vector{TileId::fromTileXY(1, 1, 2)});
        request->setWorkAdmissionGate(std::make_shared<std::atomic_bool>(false));
        requests.push_back(std::move(request));
    }
    // Invalidation deduplicates by address; make its first callback the failing one.
    std::ranges::sort(requests, {}, [](auto const& request) { return request.get(); });
    requests.front()->onDone_ = [&](auto status)
    {
        completions.push_back(status);
        if (standardException)
            throw std::runtime_error("Disconnected invalidation consumer");
        throw 1;
    };
    requests.back()->onDone_ = [&](auto status) { completions.push_back(status); };
    REQUIRE(service.request(requests));

    REQUIRE_NOTHROW(service.add(addon));
    CHECK(completions == std::vector{RequestStatus::Aborted, RequestStatus::Aborted});
    CHECK(requests.front()->getStatus() == RequestStatus::Aborted);
    CHECK(requests.back()->getStatus() == RequestStatus::Aborted);
    CHECK(base->featureLoads == 0);
    REQUIRE(changes.size() == 1);
    CHECK(changes.front().reason == "added");
    auto catalog = service.sourceCatalog();
    CHECK(changes.front().revision == catalog.revision);
    REQUIRE(catalog.sources.size() == 2);
    for (auto const& source : catalog.sources)
        CHECK(source.status == DataSourceCatalogStatus::Ready);
}

TEST_CASE(
    "Unsupported add-on-only object layers leave the published base untouched",
    "[schema2][composition][Service][objects]")
{
    Service service(std::make_shared<MemCache>(8), false, 0ms, 1);
    auto base = std::make_shared<CompositionSource>();
    auto addon = std::make_shared<CompositionSource>(true);
    addon->metadata.layers_.at("Extra")->partitionKind_ = PartitionKind::Object;
    addon->metadata.layers_.at("Extra")->tileAssociationLevel_ = 13;
    service.add(base);
    auto before = service.sourceCatalog();
    REQUIRE_THROWS_AS(service.add(addon), std::invalid_argument);
    auto after = service.sourceCatalog();
    REQUIRE(after.sources.size() == 1);
    CHECK(after.revision == before.revision);
    CHECK(after.sources.front().info == before.sources.front().info);
    CHECK(after.sources.front().info->schemaId_ == base->graph->schemaId());
    CHECK_FALSE(after.sources.front().info->layers_.contains("Extra"));

    PartitionFeatureLayer::Ptr result;
    auto request = std::make_shared<LayerTilesRequest>(
        "CompositionMap", "Things", std::vector{TileId::fromTileXY(1, 1, 2)});
    request->onFeatureLayer([&](auto layer) { result = std::move(layer); });
    REQUIRE(service.request(std::vector{request}));
    request->wait();
    REQUIRE(result);
    CHECK(result->schemaId() == base->graph->schemaId());
    CHECK(result->at(0)->attributesOrNull()->toJson()["count"] == 255);
    CHECK(base->featureLoads == 1);
    CHECK(addon->featureLoads == 0);
}

TEST_CASE(
    "Failed add-on contributions expire and retry instead of caching empty layers forever",
    "[schema2][composition][Service][ttl]")
{
    auto const layerName = GENERATE(std::string("Extra"), std::string("Things"));
    auto cache = std::make_shared<MemCache>(8);
    Service service(cache, false, 0ms, 1, 5min, 1h);
    auto base = std::make_shared<CompositionSource>();
    auto addon = std::make_shared<TransientCompositionSource>();
    auto expectedTtl = 5min;
    SECTION("Error with no producer lifetime") {}
    SECTION("Error with an infinite producer lifetime") { addon->failureTtl = 0ms; }
    SECTION("Error with a longer producer lifetime") { addon->failureTtl = 20min; }
    SECTION("Error with a shorter producer lifetime")
    {
        addon->failureTtl = 2min;
        expectedTtl = 2min;
    }
    SECTION("Null response") { addon->returnNull = true; }
    service.add(base);
    service.add(addon);
    auto load = [&]
    {
        PartitionFeatureLayer::Ptr result;
        auto request = std::make_shared<LayerTilesRequest>(
            "CompositionMap", layerName, std::vector{TileId::fromTileXY(1, 1, 2)});
        request->onFeatureLayer([&](auto layer) { result = std::move(layer); });
        REQUIRE(service.request(std::vector{request}));
        request->wait();
        REQUIRE(request->getStatus() == RequestStatus::Success);
        REQUIRE(result);
        return result;
    };
    auto failed = load();
    CHECK(failed->size() == (layerName == "Extra" ? 0 : 1));
    CHECK_FALSE(failed->error());
    CHECK_FALSE(failed->warnings().empty());
    REQUIRE(failed->ttl() == expectedTtl);
    CHECK(load()->warnings() == failed->warnings());
    REQUIRE(addon->attempts == 1);

    // Expire the serialized result explicitly: no wall-clock sleeps or cache bypass.
    failed->setTimestamp(std::chrono::system_clock::now() - expectedTtl - 1s);
    cache->putTileLayer(failed);
    auto recovered = load();
    REQUIRE(addon->attempts == 2);
    REQUIRE(recovered->size() == 1);
    CHECK(recovered->warnings().empty());
    CHECK(recovered->at(0)->attributesOrNull()->toJson()["label"] == "overlay");
    CHECK((!recovered->ttl() || recovered->ttl()->count() == 0));
    CHECK(base->featureLoads == (layerName == "Extra" ? 0 : 2));
}

TEST_CASE(
    "Committed coalesced deliveries retain their snapshot across reentrant recomposition",
    "[schema2][composition][Service][source-lifecycle]")
{
    PartitionFeatureLayer::Ptr first, second, refreshed;
    std::promise<RequestStatus> firstDone, secondDone, refreshedDone;
    auto firstStatus = firstDone.get_future();
    auto secondStatus = secondDone.get_future();
    auto refreshedStatus = refreshedDone.get_future();
    auto cache = std::make_shared<MemCache>(8);
    Service service(cache, false, 0ms, 1);
    auto base = std::make_shared<CompositionSource>();
    auto addon = std::make_shared<CompositionSource>(true);
    service.add(base);
    auto gate = std::make_shared<std::atomic_bool>(false);
    std::vector<LayerTilesRequest::Ptr> requests;
    for (int i = 0; i < 2; ++i) {
        auto request = std::make_shared<LayerTilesRequest>(
            "CompositionMap", "Things", std::vector{TileId::fromTileXY(1, 1, 2)});
        request->setWorkAdmissionGate(gate);
        requests.push_back(std::move(request));
    }
    requests.front()->onFeatureLayer([&](auto layer)
    {
        first = std::move(layer);
        // The scheduler already committed both consumers to this completed snapshot.
        service.add(addon);
    });
    requests.front()->onDone_ = [&](auto status) { firstDone.set_value(status); };
    requests.back()->onFeatureLayer([&](auto layer) { second = std::move(layer); });
    requests.back()->onDone_ = [&](auto status) { secondDone.set_value(status); };
    REQUIRE(service.request(requests));
    gate->store(true);
    service.notifyWorkAvailable();
    REQUIRE(firstStatus.wait_for(5s) == std::future_status::ready);
    REQUIRE(secondStatus.wait_for(5s) == std::future_status::ready);
    CHECK(firstStatus.get() == RequestStatus::Success);
    CHECK(secondStatus.get() == RequestStatus::Success);
    REQUIRE(first);
    CHECK(second == first);
    CHECK(first->schemaId() == base->graph->schemaId());
    CHECK_FALSE(first->at(0)->attributesOrNull()->toJson().contains("label"));
    CHECK_FALSE(cache->getTileLayerBlob(first->id()));

    auto fresh = std::make_shared<LayerTilesRequest>(
        "CompositionMap", "Things", std::vector{TileId::fromTileXY(1, 1, 2)});
    fresh->onFeatureLayer([&](auto layer) { refreshed = std::move(layer); });
    fresh->onDone_ = [&](auto status) { refreshedDone.set_value(status); };
    REQUIRE(service.request(std::vector{fresh}));
    REQUIRE(refreshedStatus.wait_for(5s) == std::future_status::ready);
    CHECK(refreshedStatus.get() == RequestStatus::Success);
    REQUIRE(refreshed);
    CHECK(refreshed->schemaId() != first->schemaId());
    CHECK(refreshed->at(0)->attributesOrNull()->toJson()["label"] == "overlay");
    CHECK(base->featureLoads == 2);
    CHECK(addon->featureLoads == 1);
}
