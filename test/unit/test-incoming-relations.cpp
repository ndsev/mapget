#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <future>
#include <sstream>
#include "mapget/model/incoming-relations.h"
#include "mapget/model/featureid.h"
#include "mapget/model/attrpoint.h"
#include "mapget/model/layerschema.h"
#include "mapget/model/stream.h"
#include "mapget/service/service.h"
#include "mapget/service/memcache.h"
#include "mapget/http-service/http-client.h"
#include "test-http-service-fixture.h"
#include "../../libs/http-service/src/tiles-request-json.h"

using namespace mapget;
using namespace std::chrono_literals;
namespace {
/** One-worker provider with eligible, conventional, foreign and secondary references. */
class IncomingSource : public DataSource {
public:
    DataSourceInfo metadata;
    std::atomic_size_t providerLoads = 0;
    std::atomic_size_t unrelatedLoads = 0;
    bool failProvider = false;
    bool emptyProvider = false;
    bool ambiguous = false;
    bool throwLocate = false;
    std::promise<void> providerStarted;
    std::shared_future<void> unblock;
    explicit IncomingSource(bool object = false) {
        auto type = [](std::string name) { return nlohmann::json{{"name",name},
            {"uniqueIdCompositions", {{{{"partId","id"},{"datatype","U64"}}},
                                       {{{"partId","alias"},{"datatype","U64"}}}}}}; };
        auto layer = nlohmann::json{{"type","Features"}, {"partitionKind", object ? "object" : "tile"},
            {"featureTypes", {type("Surface"), type("Lane")}}};
        if (object) layer["tileAssociationLevel"] = 13;
        metadata = DataSourceInfo::fromJson({{"mapId","IncomingMap"},{"stringPoolId","IncomingPool"},
            {"maxParallelJobs",1},{"layers",{{"Surface",layer},{"Lane",layer},{"Unrelated",layer}}}});
        auto schema = std::make_shared<LayerSchema>();
        auto root = schema->addSchema(simfil::Schema::Kind::Object, LayerSchema::featureKey("Lane"), "Lane");
        schema->setJsonSchemaAnnotations(root, {{"x-mapget", {{"relationDeclarations", {
            {{"ownerType","Lane"},{"name","visible"},{"targetTypes",{"Surface"}},{"incoming",true},{"coverage","complete"}},
            {{"ownerType","Lane"},{"name","ordinary"},{"targetTypes",{"Surface"}},{"incoming",false}}
        }}}}});
        schema->finalize();
        metadata.layers_.at("Lane")->featureModelSchema_ = schema;
    }
    DataSourceInfo info() override { return metadata; }
    void fill(PartitionSourceDataLayer::Ptr const&) override {}
    void fill(PartitionFeatureLayer::Ptr const& layer) override {
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

TEST_CASE("Incoming discovery keeps source ownership and conventional families separate", "[incoming]") {
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
    REQUIRE(collections[0].relations_.size() == 2);
    CHECK(collections[0].relations_[0].relationIndex_ == 1);
    CHECK(collections[0].relations_[1].relationIndex_ == 2);
    CHECK(collections[0].expiresAtMs_.has_value());
    CHECK(source->providerLoads == 1);
    CHECK(source->unrelatedLoads == 0);
    FeatureInspectionView view(collections[0], snapshots);
    REQUIRE(view.incoming().size() == 2);
    CHECK(view.target()->numRelations() == 0);
    CHECK(view.incoming()[0].source_->id()->toString() == "Lane.7");
    CHECK(view.incoming()[0].source_->numRelations() == 5);
    CHECK(view.incoming()[0].relation_->name() == "visible");
    CHECK(IncomingRelationCollection::fromJson(collections[0].toJson()).toJson() == collections[0].toJson());
    auto missing = collections[0]; missing.snapshots_.clear();
    CHECK_THROWS(FeatureInspectionView(missing, snapshots));
    auto wrong = collections[0]; wrong.relations_[0].relationIndex_ = 0;
    CHECK_THROWS(FeatureInspectionView(wrong, snapshots));
    auto schema = source->metadata.layers_.at("Lane")->layerSchema();
    CHECK(source->metadata.layers_.at("Lane")->toJson()["incomingRelationTargetTypes"] == nlohmann::json::array({"Surface"}));
    for (auto roundtrip : std::vector<std::shared_ptr<LayerSchema const>>{schema->detachedCopy(), LayerSchema::fromJsonSchema(schema->toJsonSchema())}) {
        auto declarations = roundtrip->relationDeclarations();
        REQUIRE(declarations.size() == 2);
        CHECK(declarations[0].permitsIncoming("Lane","visible","Surface"));
        CHECK_FALSE(declarations[1].permitsIncoming("Lane","ordinary","Surface"));
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
    CHECK(result[0].relations_.size() == (mode == 2 ? 1 : 0));
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
    request->onIncomingRelations([&](auto const& value) { ++manifests; CHECK(value.relations_.size() == 2); });
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
    SECTION("Malformed declaration") {
        auto schema = std::make_shared<LayerSchema>();
        auto root = schema->addSchema(simfil::Schema::Kind::Object, LayerSchema::featureKey("Lane"), "Lane");
        schema->setJsonSchemaAnnotations(root, {{"x-mapget", {{"relationDeclarations", {
            {{"ownerType","Lane"},{"name","visible"},{"targetTypes",{"Surface"}},{"incoming","invalid"}}
        }}}}});
        schema->finalize();
        source->metadata.layers_.at("Lane")->featureModelSchema_ = schema;
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

TEST_CASE("Incoming discovery composes add-on declarations and reports contributor failures", "[incoming]") {
    auto failed = GENERATE(false, true);
    Service service(std::make_shared<MemCache>(),false,0ms,1);
    auto primary = std::make_shared<IncomingSource>();
    primary->emptyProvider = true;
    primary->metadata.layers_.at("Lane")->featureModelSchema_.reset();
    auto addon = std::make_shared<IncomingSource>();
    addon->metadata.stringPoolId_ = "IncomingAddon";
    addon->metadata.isAddOn_ = true;
    addon->failProvider = failed;
    service.add(primary); service.add(addon);
    auto request = incomingRequest(TileId::fromTileXY(1,1,2));
    std::vector<IncomingRelationCollection> result;
    request->onIncomingRelations([&](auto const& value) { result.push_back(value); });
    REQUIRE(service.request(request)); waitDone(request);
    REQUIRE(request->getStatus() == RequestStatus::Success);
    REQUIRE(result.size() == 1);
    CHECK(result[0].complete_ == !failed);
    CHECK(result[0].relations_.size() == (failed ? 0 : 2));
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
