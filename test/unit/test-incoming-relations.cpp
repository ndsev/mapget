#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <future>
#include <sstream>
#include "../../libs/http-service/src/tiles-request-json.h"
#include "mapget/http-service/http-client.h"
#include "mapget/model/attrpoint.h"
#include "mapget/model/featureid.h"
#include "mapget/model/incoming-relations.h"
#include "mapget/model/layerschema.h"
#include "mapget/model/mapschema.h"
#include "mapget/model/stream.h"
#include "mapget/service/memcache.h"
#include "mapget/service/service.h"
#include "test-http-service-fixture.h"

using namespace mapget;
using namespace std::chrono_literals;
namespace {
/** One-worker provider with ordinary forward, foreign and secondary references. */
class IncomingSource : public DataSource {
public:
    DataSourceInfo metadata;
    std::atomic_size_t tileLoads = 0;
    std::atomic_size_t providerLoads = 0;
    std::atomic_size_t unrelatedLoads = 0;
    bool failProvider = false;
    bool emptyProvider = false;
    bool ambiguous = false;
    bool throwLocate = false;
    std::promise<void> providerStarted;
    std::shared_future<void> unblock;
    explicit IncomingSource(
        bool object = false,
        bool withRelations = true,
        bool knownRelations = true)
    {
        auto type = [](std::string name) { return nlohmann::json{{"name",name},
            {"uniqueIdCompositions", {{{{"partId","id"},{"datatype","U64"}}},
                                       {{{"partId","alias"},{"datatype","U64"}}}}}}; };
        auto layer = nlohmann::json{{"type","Features"}, {"partitionKind", object ? "object" : "tile"},
            {"featureTypes", {type("Surface"), type("Lane")}}};
        layer["featureModelSchema"] = {
            {"type", "object"},
            {"x-mapget",
             {{"metaType", "Feature"},
              {"featureType", "Surface"},
              {"relationDeclarations", nlohmann::json::array()}}}};
        if (object) layer["tileAssociationLevel"] = 13;
        auto lane = layer;
        auto declarations = nlohmann::json::array();
        if (withRelations) {
            for (auto name : {"visible", "ordinary"}) {
                declarations.push_back(
                    {{"ownerType", "Lane"},
                     {"name", name},
                     {"targetTypes", {"Surface"}},
                     {"complete", true}});
            }
        }
        lane["featureModelSchema"] = {
            {"type", "object"},
            {"x-mapget",
             {{"metaType", "Feature"},
              {"featureType", "Lane"},
              {"relationDeclarations", std::move(declarations)}}}};
        if (!knownRelations)
            lane["featureModelSchema"]["x-mapget"].erase("relationDeclarations");
        metadata = DataSourceInfo::fromJson(
            {{"mapId", "IncomingMap"},
             {"stringPoolId", "IncomingPool"},
             {"maxParallelJobs", 1},
             {"layers", {{"Surface", layer}, {"Lane", lane}, {"Unrelated", layer}}}});
    }
    DataSourceInfo info() override { return metadata; }
    void fill(PartitionSourceDataLayer::Ptr const&) override {}
    void fill(PartitionFeatureLayer::Ptr const& layer) override {
        ++tileLoads;
        if (layer->layerInfo()->layerId_ == "Unrelated") { ++unrelatedLoads; return; }
        if (layer->layerInfo()->layerId_ == "Surface") {
            layer->newFeature("Surface", {{"id", int64_t{1}}});
            layer->newFeature("Surface", {{"id", int64_t{2}}});
            return;
        }
        ++providerLoads;
        if (unblock.valid()) { providerStarted.set_value(); unblock.wait(); }
        layer->setTtl(5s);
        if (failProvider) { layer->setError("Provider conversion failed"); return; }
        if (emptyProvider) return;
        auto source = layer->newFeature("Lane", {{"id", int64_t{7}}});
        source->addRelation("ordinary", "Surface", {{"id", int64_t{1}}});
        source->addRelation("visible", "Surface", {{"id", int64_t{1}}});
        source->addRelation("visible", "Surface", {{"alias", int64_t{9}}});
        auto foreign = layer->newFeatureId("Surface", {{"id", int64_t{1}}}, "OtherMap");
        source->addRelation("visible", foreign);
        source->addRelation("visible", "Surface", {{"id", int64_t{2}}});
    }
    std::vector<LocateCandidate> locate(LocateRequest const& request) override {
        if (throwLocate) throw std::runtime_error("Locator failed");
        if (!request.partition_) return {};
        auto key = MapPartitionKey(LayerType::Features,"IncomingMap","Surface",*request.partition_);
        if (ambiguous) return {LocateCandidate(key,"Surface", "true")};
        return {LocateCandidate(key, "Surface.1")};
    }
};
/** Build a bounded service-only incoming request with an exact selected target. */
FeatureLayerFilterTilesRequest::Ptr incomingRequest(PartitionId partition) {
    FeatureLayerFilterRequest definition;
    FeatureLayerFilterChannel channel;
    channel.channelId_ = "incoming";
    channel.scope_ = FeatureLayerFilterScope::Relation;
    channel.relation_ = FeatureLayerStoredRelationOptions{};
    channel.relation_->incoming_ = true;
    definition.channels_.push_back(channel);
    auto request = std::make_shared<FeatureLayerFilterTilesRequest>("IncomingMap","Surface",
        std::vector<PartitionId>{partition},definition);
    FeatureLayerFilterRoot root;
    root.partitionId_ = partition;
    root.canonicalFeatureId_ = "Surface.1";
    request->exactRoots_.push_back(root);
    return request;
}
/** Assert asynchronous work terminates within the test's bounded wait. */
void waitDone(FeatureLayerFilterTilesRequest::Ptr const& request) {
    for (int i = 0; i < 500 && !request->isDone(); ++i) std::this_thread::sleep_for(10ms);
    REQUIRE(request->isDone());
}
}

TEST_CASE("Incoming discovery includes every declared forward family without opt-in", "[incoming]")
{
    auto object = GENERATE(false,true);
    PartitionId partition = object ? PartitionId::object(UINT64_MAX) : PartitionId(TileId::fromTileXY(1,1,2));
    Service service(std::make_shared<MemCache>(),false,0ms,1);
    auto source = std::make_shared<IncomingSource>(object);
    service.add(source);
    auto request = incomingRequest(partition);
    std::vector<PartitionFeatureLayer::Ptr> snapshots;
    std::vector<IncomingRelationCollection> collections;
    request->onFeatureSnapshot([&](auto const& layer) { snapshots.push_back(layer); });
    request->onIncomingRelations([&](auto const& collection) { collections.push_back(collection); });
    REQUIRE(service.request(std::vector{request}));
    waitDone(request);
    REQUIRE(request->getStatus() == RequestStatus::Success);
    REQUIRE(collections.size() == 1);
    REQUIRE(collections[0].complete_);
    CHECK(collections[0].status_ == IncomingRelationCollection::Status::Complete);
    CHECK(
        collections[0].scope_ ==
        std::vector<MapPartitionKey>{
            MapPartitionKey(LayerType::Features, "IncomingMap", "Lane", partition)});
    REQUIRE(collections[0].relations_.size() == 3);
    CHECK(collections[0].relations_[0].relationIndex_ == 0);
    CHECK(collections[0].relations_[1].relationIndex_ == 1);
    CHECK(collections[0].relations_[2].relationIndex_ == 2);
    CHECK(collections[0].expiresAtMs_.has_value());
    CHECK(source->providerLoads == 1);
    CHECK(source->unrelatedLoads == 0);
    FeatureInspectionView view(collections[0], snapshots);
    REQUIRE(view.incoming().size() == 3);
    CHECK(view.target()->numRelations() == 0);
    CHECK(view.incoming()[0].source_->id()->toString() == "Lane.7");
    CHECK(view.incoming()[0].source_->numRelations() == 5);
    CHECK(view.incoming()[0].relation_->name() == "ordinary");
    CHECK(IncomingRelationCollection::fromJson(collections[0].toJson()).toJson() == collections[0].toJson());
    auto missing = collections[0]; missing.snapshots_.clear();
    CHECK_THROWS(FeatureInspectionView(missing, snapshots));
    auto wrong = collections[0];
    wrong.relations_[0].relationIndex_ = 3;
    CHECK_THROWS(FeatureInspectionView(wrong, snapshots));
    auto schema = source->metadata.layers_.at("Lane")->layerSchema();
    CHECK_FALSE(source->metadata.layers_.at("Lane")->toJson().contains("featureModelSchema"));
    std::ostringstream encoded;
    REQUIRE(source->metadata.mapSchema_->write(encoded));
    auto bytes = encoded.str();
    auto binary = MapSchema::read(
        std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()),
        source->metadata.schemaId_);
    auto imported = std::make_shared<MapSchema>();
    auto jsonLayer = imported->importLayer(
        "Lane",
        LayerType::Features,
        schema->toJsonSchema(),
        schema->featureTypeInfos());
    imported->finalize();
    for (auto roundtrip : std::vector<LayerSchema::Ptr>{binary->layer("Lane"), jsonLayer}) {
        auto declarations = roundtrip->relationDeclarations();
        REQUIRE(declarations.size() == 2);
        CHECK(declarations[0].permitsIncoming("Lane","visible","Surface"));
        CHECK(declarations[1].permitsIncoming("Lane", "ordinary", "Surface"));
    }
}

TEST_CASE("Incoming partial and empty results retain dependency expiry", "[incoming]") {
    auto mode = GENERATE(0,1,2);
    Service service(std::make_shared<MemCache>(),false,0ms,1);
    auto source = std::make_shared<IncomingSource>();
    source->emptyProvider = mode == 0;
    source->failProvider = mode == 1;
    source->ambiguous = mode == 2;
    service.add(source);
    auto request = incomingRequest(TileId::fromTileXY(1,1,2));
    std::vector<IncomingRelationCollection> result;
    request->onIncomingRelations([&](auto const& value) { result.push_back(value); });
    REQUIRE(service.request(std::vector{request})); waitDone(request);
    REQUIRE(result.size() == 1);
    CHECK(result[0].expiresAtMs_.has_value());
    CHECK(result[0].complete_ == (mode == 0));
    CHECK(
        result[0].status_ ==
        (mode == 0 ? IncomingRelationCollection::Status::Complete :
                     IncomingRelationCollection::Status::Partial));
    CHECK(result[0].scope_.size() == 1);
    CHECK(result[0].relations_.size() == (mode == 2 ? 2 : 0));
}

TEST_CASE(
    "Incoming discovery distinguishes empty contracts from unknown and unsupported scope",
    "[incoming]")
{
    auto mode = GENERATE(0, 1, 2);
    Service service(std::make_shared<MemCache>(), false, 0ms, 1);
    auto source = std::make_shared<IncomingSource>(false, mode == 2, mode != 1);
    if (mode == 2) {
        auto provider = source->metadata.getLayer("Lane");
        provider->partitionKind_ = PartitionKind::Object;
        provider->tileAssociationLevel_ = 13;
    }
    service.add(source);
    auto request = incomingRequest(TileId::fromTileXY(1, 1, 2));
    std::vector<IncomingRelationCollection> result;
    request->onIncomingRelations([&](auto const& value) { result.push_back(value); });
    REQUIRE(service.request(request));
    waitDone(request);
    REQUIRE(request->getStatus() == RequestStatus::Success);
    REQUIRE(result.size() == 1);
    using Status = IncomingRelationCollection::Status;
    CHECK(
        result[0].status_ ==
        (mode == 0     ? Status::NoPossibleFamily :
             mode == 1 ? Status::Partial :
                         Status::UnsupportedScope));
    CHECK(result[0].complete_ == (mode == 0));
    CHECK(result[0].scope_.empty());
    CHECK(result[0].relations_.empty());
    CHECK(source->providerLoads == 0);
    CHECK(source->unrelatedLoads == 0);
}

TEST_CASE(
    "Incoming manifest v2 roundtrips scoped status and rejects inconsistent completeness",
    "[incoming]")
{
    using Status = IncomingRelationCollection::Status;
    auto status = GENERATE(
        Status::NoPossibleFamily,
        Status::Complete,
        Status::Partial,
        Status::UnsupportedScope);
    auto object = GENERATE(false, true);
    PartitionId partition = object ?
        PartitionId::object(UINT64_MAX) :
        PartitionId(TileId::fromTileXY(1, 1, 2));
    IncomingRelationCollection collection;
    collection.target_ = MapPartitionKey(LayerType::Features, "Incoming/Map", "Surface", partition);
    collection.featureId_ = "Surface.1";
    collection.status_ = status;
    collection.complete_ = status == Status::Complete || status == Status::NoPossibleFamily;
    if (status == Status::Complete || status == Status::Partial)
        collection.scope_.emplace_back(LayerType::Features, "Incoming/Map", "Lane", partition);
    auto json = collection.toJson();
    CHECK(json["version"] == 2);
    CHECK(
        json["status"] ==
        (status == Status::NoPossibleFamily ? "no-possible-family" :
             status == Status::Complete     ? "complete" :
             status == Status::Partial      ? "partial" :
                                              "unsupported-scope"));
    CHECK(json["scope"].is_array());
    CHECK(json["scope"].size() == collection.scope_.size());
    for (size_t i = 0; i < collection.scope_.size(); ++i)
        CHECK(json["scope"][i] == collection.scope_[i].toString());
    CHECK(IncomingRelationCollection::fromJson(json).toJson() == json);
    auto invalid = json;
    invalid["complete"] = !collection.complete_;
    CHECK_THROWS(IncomingRelationCollection::fromJson(invalid));
    invalid = json;
    invalid["status"] = "unknown";
    CHECK_THROWS(IncomingRelationCollection::fromJson(invalid));
    invalid = json;
    invalid["version"] = 1;
    CHECK_THROWS(IncomingRelationCollection::fromJson(invalid));
    invalid = json;
    invalid.erase("scope");
    CHECK_THROWS(IncomingRelationCollection::fromJson(invalid));
    invalid = json;
    invalid["scope"] = nlohmann::json::object();
    CHECK_THROWS(IncomingRelationCollection::fromJson(invalid));
    invalid = json;
    invalid["scope"] = nlohmann::json::array({42});
    CHECK_THROWS(IncomingRelationCollection::fromJson(invalid));
    invalid = json;
    invalid["scope"] = std::vector<std::string>(
        IncomingRelationCollection::MaxSnapshots + 1,
        collection.target_.toString());
    CHECK_THROWS(IncomingRelationCollection::fromJson(invalid));
}

TEST_CASE("Incoming cancellation and missing roots cannot publish completion", "[incoming]") {
    Service service(std::make_shared<MemCache>(),false,0ms,1);
    auto source = std::make_shared<IncomingSource>();
    auto request = incomingRequest(TileId::fromTileXY(1,1,2));
    std::atomic_size_t manifests = 0;
    request->onIncomingRelations([&](auto const&) { ++manifests; });
    SECTION("Missing target invalidates the entire bundle") {
        auto absent = request->exactRoots_.front(); absent.canonicalFeatureId_ = "Surface.99";
        request->exactRoots_.push_back(absent);
        service.add(source);
        REQUIRE(service.request(std::vector{request})); waitDone(request);
        CHECK(request->getStatus() == RequestStatus::Aborted);
        CHECK(manifests == 0);
    }
    SECTION("Cancelling during provider conversion suppresses late callbacks") {
        std::promise<void> release;
        source->unblock = release.get_future().share();
        auto started = source->providerStarted.get_future();
        service.add(source);
        REQUIRE(service.request(std::vector{request}));
        auto ready = started.wait_for(5s);
        service.abort(request);
        release.set_value();
        REQUIRE(ready == std::future_status::ready);
        CHECK(request->getStatus() == RequestStatus::Aborted);
        CHECK(manifests == 0);
    }
}

TEST_CASE("Incoming HTTP contract preserves typed bundles and rejects interactive traversal", "[incoming]") {
    auto source = std::make_shared<IncomingSource>();
    auto& service = test::httpService();
    service.add(source);
    HttpClient client("127.0.0.1",service.port());
    auto request = incomingRequest(TileId::fromTileXY(1,1,2));
    size_t snapshots = 0, manifests = 0;
    request->onFeatureSnapshot([&](auto const&) { ++snapshots; });
    request->onIncomingRelations(
        [&](auto const& value)
        {
            ++manifests;
            CHECK(value.relations_.size() == 3);
        });
    client.filter(request);
    CHECK(request->getStatus() == RequestStatus::Success);
    CHECK(snapshots == 2);
    CHECK(manifests == 1);
    auto json = detail::filterRequestToJson(request->filter_,false);
    CHECK(json["channels"][0]["relation"]["traversal"] == "incoming");
    auto interactive = json;
    interactive["mapId"] = "IncomingMap"; interactive["layerId"] = "Surface";
    interactive["filterId"] = "incoming"; interactive["generation"] = 1;
    interactive["partitions"] = nlohmann::json::array({request->tileIds_[0].toJson()});
    CHECK_THROWS(detail::parseLayerTilesRequestJson(interactive));
    service.remove(source);
}

TEST_CASE("Restricted incoming snapshots retain unselected attribute-point hosts", "[incoming]") {
    auto metadata = IncomingSource().metadata.layers_.at("Lane");
    auto source = std::make_shared<PartitionFeatureLayer>(TileId::fromTileXY(1,1,2),
        "HostPool", "IncomingMap", metadata, std::make_shared<StringPool>("HostPool"));
    auto host = source->newFeature("Lane", {{"id", int64_t{99}}});
    auto geometry = source->newGeometry(GeomType::Line, 2);
    geometry->append({11,48,0}); geometry->append({12,49,0}); host->addGeometry(geometry);
    auto sequence = source->newAttrPointSequence(host, geometry);
    sequence->appendAttrPoint(1, {11.5,48.5,0});
    auto selected = source->newFeature("Lane", {{"id", int64_t{1}}});
    auto relation = selected->addRelation("visible","Surface",{{"id",int64_t{1}}});
    auto validity = source->newValidityCollection();
    validity->newAttrPointIndexRange(sequence,0,2);
    relation->setSourceValidity(validity);
    std::vector<std::string> ids = {"Lane.1"};
    auto cloned = cloneInspectionFeatures(source, ids);
    CHECK(cloned->find("Lane.1")->toJson() == selected->toJson());
    REQUIRE(cloned->find("Lane.99"));
    CHECK(cloned->numAttrPointSequences() == 1);
    CHECK(cloned->find("Lane.99")->numRelations() == 0);
    CHECK(cloneInspectionFeatures(source, ids, 1)->size() == 0);
    CHECK_THROWS(cloneInspectionFeatures(source, ids, SIZE_MAX, [] { return true; }));
    CHECK_FALSE(incomingRequest(source->partitionId())->filter_.filterSource(*source, true));
}

TEST_CASE("Incoming callback failures terminate without publishing a complete bundle", "[incoming]") {
    Service service(std::make_shared<MemCache>(),false,0ms,1);
    auto source = std::make_shared<IncomingSource>();
    auto request = incomingRequest(TileId::fromTileXY(1,1,2));
    size_t manifests = 0;
    request->onIncomingRelations([&](auto const&) { ++manifests; });
    SECTION("Throwing secondary locator") { source->throwLocate = true; }
    SECTION("Malformed forward declaration fails before publication")
    {
        auto owner = std::make_shared<MapSchema>();
        auto schema = owner->newLayer("Lane", LayerType::Features);
        auto root = schema->addSchema(simfil::Schema::Kind::Object, LayerSchema::featureKey("Lane"), "Lane");
        schema->setJsonSchemaAnnotations(
            root,
            {{"x-mapget",
              {{"relationDeclarations",
                {{{"ownerType", "Lane"}, {"name", "visible"}, {"targetTypes", "invalid"}}}}}}});
        for (auto const& type : source->metadata.getLayer("Lane")->featureTypes())
            schema->addFeatureType(
                type,
                type.name_ == "Lane" ? root : simfil::NoSchemaId,
                type.name_ == "Lane");
        REQUIRE_THROWS(owner->finalize());
        CHECK(manifests == 0);
        return;
    }
    SECTION("Consumer throws while accepting snapshots") {
        request->onFeatureSnapshot([](auto const&) { throw std::runtime_error("Consumer rejected snapshot"); });
    }
    SECTION("Consumer cancels while accepting snapshots") {
        request->onFeatureSnapshot([&](auto const&) { service.abort(request); });
    }
    service.add(source);
    REQUIRE(service.request(request)); waitDone(request);
    CHECK(request->getStatus() == RequestStatus::Aborted);
    CHECK(manifests == 0);
}

TEST_CASE("Incoming discovery reports incompatible add-on contracts without claiming completeness", "[incoming][add-on]") {
    Service service(std::make_shared<MemCache>(),false,0ms,1);
    auto primary = std::make_shared<IncomingSource>(false, false);
    primary->emptyProvider = true;
    auto addon = std::make_shared<IncomingSource>();
    addon->metadata.stringPoolId_ = "IncomingAddon";
    addon->metadata.isAddOn_ = true;
    REQUIRE(primary->metadata.schemaId_ != addon->metadata.schemaId_);
    service.add(primary); service.add(addon);
    auto request = incomingRequest(TileId::fromTileXY(1,1,2));
    std::vector<IncomingRelationCollection> result;
    request->onIncomingRelations([&](auto const& value) { result.push_back(value); });
    REQUIRE(service.request(request)); waitDone(request);
    REQUIRE(request->getStatus() == RequestStatus::Success);
    REQUIRE(result.size() == 1);
    CHECK_FALSE(result[0].complete_);
    CHECK(result[0].status_ == IncomingRelationCollection::Status::Partial);
    CHECK(result[0].relations_.empty());
    CHECK(addon->tileLoads == 0);
    CHECK(std::ranges::any_of(result[0].issues_, [&](auto const& issue) {
        return issue.find("Cross-schema add-on composition is not supported") != std::string::npos &&
            issue.find(primary->metadata.schemaId_) != std::string::npos &&
            issue.find(addon->metadata.schemaId_) != std::string::npos;
    }));
}

TEST_CASE("Same-schema add-ons preserve incoming relations and report contributor failures", "[incoming][add-on]") {
    auto failed = GENERATE(false, true);
    Service service(std::make_shared<MemCache>(),false,0ms,1);
    auto primary = std::make_shared<IncomingSource>();
    primary->emptyProvider = true;
    auto addon = std::make_shared<IncomingSource>();
    addon->metadata.stringPoolId_ = "IncomingAddon";
    addon->metadata.isAddOn_ = true;
    addon->failProvider = failed;
    REQUIRE(primary->metadata.schemaId_ == addon->metadata.schemaId_);
    service.add(primary); service.add(addon);
    auto request = incomingRequest(TileId::fromTileXY(1,1,2));
    std::vector<IncomingRelationCollection> result;
    request->onIncomingRelations([&](auto const& value) { result.push_back(value); });
    REQUIRE(service.request(request)); waitDone(request);
    REQUIRE(request->getStatus() == RequestStatus::Success);
    REQUIRE(result.size() == 1);
    CHECK(result[0].complete_ == !failed);
    CHECK(
        result[0].status_ ==
        (failed ? IncomingRelationCollection::Status::Partial :
                  IncomingRelationCollection::Status::Complete));
    CHECK(result[0].relations_.size() == (failed ? 0 : 3));
    CHECK(addon->providerLoads == 1);
}

TEST_CASE("Rejected add-on contracts leave primary tiles and dictionaries intact", "[incoming][add-on]") {
    auto cache = std::make_shared<MemCache>();
    Service service(cache, false, 0ms, 1);
    auto primary = std::make_shared<IncomingSource>(false, false);
    auto addon = std::make_shared<IncomingSource>();
    addon->metadata.stringPoolId_ = "IncompatibleAddon";
    addon->metadata.isAddOn_ = true;
    REQUIRE(primary->metadata.schemaId_ != addon->metadata.schemaId_);
    service.add(primary); service.add(addon);
    auto request = std::make_shared<LayerTilesRequest>("IncomingMap", "Surface",
        std::vector<PartitionId>{TileId::fromTileXY(1, 1, 2)});
    PartitionFeatureLayer::Ptr result;
    request->onFeatureLayer([&](auto tile) { result = std::move(tile); });
    REQUIRE(service.request(std::vector{request}));
    request->wait();
    REQUIRE(request->getStatus() == RequestStatus::Success);
    REQUIRE(result);
    CHECK_FALSE(result->error());
    CHECK(result->stringPoolId() == primary->metadata.stringPoolId_);
    CHECK(result->schemaId() == primary->metadata.schemaId_);
    CHECK(result->numRoots() == 2);
    for (auto const& feature : *result) {
        CHECK(feature->numRelations() == 0);
        CHECK_FALSE(feature->attributesOrNull());
        CHECK_FALSE(feature->geomOrNull());
    }
    CHECK(result->warnings().size() == 1);
    CHECK(addon->tileLoads == 0);
    // Neither an add-on source pool nor a combined output pool should have been allocated.
    CHECK(cache->getStatistics().at("loaded-string-pools") == 1);
}

TEST_CASE("Incoming batching deduplicates snapshots and enforces provider authorization", "[incoming]") {
    Service service(std::make_shared<MemCache>(),false,0ms,1);
    auto provider = std::make_shared<IncomingSource>();
    provider->requireAuthHeaderRegexMatchOption("authorization", std::regex("allowed"));
    service.add(provider);
    auto request = incomingRequest(TileId::fromTileXY(1,1,2));
    auto second = request->exactRoots_.front(); second.canonicalFeatureId_ = "Surface.2";
    request->exactRoots_.push_back(second);
    request->exactRoots_.push_back(request->exactRoots_.front());
    std::vector<IncomingRelationCollection> result;
    size_t snapshots = 0;
    request->onIncomingRelations([&](auto const& value) { result.push_back(value); });
    request->onFeatureSnapshot([&](auto const&) { ++snapshots; });
    SECTION("Authorized provider is scanned once for both targets") {
        REQUIRE(service.request(request, AuthHeaders{{"authorization", "allowed"}})); waitDone(request);
        CHECK(request->getStatus() == RequestStatus::Success);
        CHECK(result.size() == 2);
        CHECK(snapshots == 2);
        CHECK(provider->providerLoads == 1);
    }
    SECTION("Unauthorized provider is not loaded") {
        REQUIRE_FALSE(service.request(request, AuthHeaders{})); waitDone(request);
        CHECK(request->getStatus() == RequestStatus::Unauthorized);
        CHECK(result.empty());
        CHECK(provider->providerLoads == 0);
    }
}
