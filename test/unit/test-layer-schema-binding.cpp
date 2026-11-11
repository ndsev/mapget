#include "mapget/model/featurelayer-filter.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/mapschema.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/subsetlayer.h"

#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json-schema.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>

using namespace mapget;

namespace
{

/** One shared graph with distinct layer views and two meanings for the same property value. */
class LayerSchemaBindingFixture
{
public:
    std::shared_ptr<MapSchema> graph;
    DataSourceInfo info;
    std::shared_ptr<StringPool> strings = std::make_shared<StringPool>("LayerBindings");
    std::array<simfil::TypeId, 2> properties{}, values{};
    simfil::TypeId concreteFeature = simfil::NoTypeId;
    simfil::TypeId bitmask = simfil::NoTypeId;

    explicit LayerSchemaBindingFixture(std::string version = "1", bool unionRoot = false)
        : graph(std::make_shared<MapSchema>(MapFormat{"layer-bindings", std::move(version), {}}))
    {
        auto road = graph->newLayer("Road", LayerType::Features);
        auto root = road->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey("Road"));
        concreteFeature = unionRoot ? road->addSchema(LayerSchema::FeatureKind) : root;
        if (unionRoot) {
            graph->schemaType(root).setComposition(simfil::SchemaType::Composition::OneOf);
            graph->schemaType(root).asComposition().addAlternative(
                graph->schemaType(concreteFeature));
        }
        auto choice = road->addSchema(
            simfil::SchemaType::Kind::Object,
            LayerSchema::featurePropertiesKey("Road"));
        graph->schemaType(choice).setComposition(simfil::SchemaType::Composition::OneOf);
        graph->schemaType(concreteFeature)
            .asObject()
            .addField("properties", graph->schemaType(choice));
        for (size_t i = 0; i != properties.size(); ++i) {
            properties[i] = road->addSchema(simfil::SchemaType::Kind::Object);
            values[i] = road->addSchema(simfil::SchemaType::Kind::Int);
            graph->schemaType(properties[i])
                .asObject()
                .addField("value", graph->schemaType(values[i]));
            graph->schemaType(choice).asComposition().addAlternative(
                graph->schemaType(properties[i]));
            graph->schemaType(values[i]).asScalar().addNamedValue(
                {.name = i == 0 ? "UNLIMITED" : "UNKNOWN",
                 .value = int64_t{255},
                 .role = simfil::SchemaType::NamedValue::Role::Special,
                 .meaning = i == 0 ?
                     simfil::SchemaType::NamedValue::Meaning::Unbounded :
                     simfil::SchemaType::NamedValue::Meaning::Unknown});
        }
        bitmask = road->addSchema(simfil::SchemaType::Kind::Bitmask);
        graph->schemaType(bitmask).asBitmask().setBitWidth(64);
        graph->schemaType(properties[0]).asObject().addField("flags", graph->schemaType(bitmask));
        auto identity = FeatureTypeInfo::fromJson(nlohmann::json::parse(R"({
            "name":"Road","uniqueIdCompositions":[[{"partId":"id","datatype":"U32"}]]
        })"));
        road->addFeatureType(identity, root);
        road->setRootSchema(root);
        graph->newLayer("OtherRoad", LayerType::Features)->addFeatureType(identity);
        (void)graph->newLayer("Raw", LayerType::SourceData);
        (void)graph->newLayer("OtherRaw", LayerType::SourceData);
        graph->finalize();

        info.mapId_ = "BindingMap";
        info.stringPoolId_ = "LayerBindings";
        for (auto name : {"Road", "OtherRoad", "Raw", "OtherRaw"}) {
            auto layer = std::make_shared<LayerInfo>();
            layer->layerId_ = name;
            layer->type_ = graph->layer(name)->layerType();
            info.layers_.emplace(name, std::move(layer));
        }
        info.setMapSchema(graph);
    }

    /** Give the destination a different dictionary to exercise ordinary clone remapping too. */
    PartitionFeatureLayer::Ptr features(bool destination = false) const
    {
        auto pool = destination ? std::make_shared<StringPool>("LayerBindingDestination") : strings;
        if (destination)
            REQUIRE(pool->emplace("padding"));
        return std::make_shared<PartitionFeatureLayer>(
            TileId::fromValue(131073), destination ? "LayerBindingDestination" : info.stringPoolId_,
            info.mapId_, info.getLayer("Road"), pool);
    }

    /** Exercise the same outer-header invariant on both native payload kinds. */
    PartitionLayer::Ptr native(bool feature) const
    {
        if (feature)
            return features();
        return std::make_shared<PartitionSourceDataLayer>(
            TileId::fromValue(131073), info.stringPoolId_, info.mapId_, info.getLayer("Raw"), strings);
    }

    PartitionLayer::Ptr decode(
        bool feature, std::vector<uint8_t> const& bytes, MapSchema::ConstPtr foreign = {}) const
    {
        auto layers = [metadata = info](auto const&, auto const& name) { return metadata.getLayer(std::string(name)); };
        auto pools = [pool = strings](auto const&) { return pool; };
        auto schemas = [owner = graph, foreign](std::string_view id) -> MapSchema::ConstPtr {
            if (id == owner->schemaId())
                return owner;
            return foreign && id == foreign->schemaId() ? foreign : nullptr;
        };
        if (feature)
            return std::make_shared<PartitionFeatureLayer>(bytes, layers, pools, schemas);
        return std::make_shared<PartitionSourceDataLayer>(bytes, layers, pools, schemas);
    }

    /** Replace only the trailing ModelPool artifact, leaving the valid native header/columns intact. */
    static std::vector<uint8_t> encode(PartitionLayer& layer, simfil::ModelPool* replacement = nullptr)
    {
        std::ostringstream encoded;
        REQUIRE(layer.write(encoded));
        auto wire = encoded.str();
        if (replacement) {
            std::ostringstream originalPool, replacementPool;
            REQUIRE(dynamic_cast<simfil::ModelPool&>(layer).simfil::ModelPool::write(originalPool));
            REQUIRE(replacement->write(replacementPool));
            REQUIRE(wire.ends_with(originalPool.str()));
            wire.resize(wire.size() - originalPool.str().size());
            wire += replacementPool.str();
        }
        return {wire.begin(), wire.end()};
    }
};

TEST_CASE(
    "Local cross-layer references need only the map identity registry",
    "[layer-schema-binding][feature-id-index]")
{
    auto graph = std::make_shared<MapSchema>();
    graph->addFeatureType(
        {"Road", {{{"region", {}, IdPartDataType::STR}, {"roadId", {}, IdPartDataType::U32}}}});
    graph->addFeatureType({"Lane", {{{"laneId", {}, IdPartDataType::U64}}}});
    graph->addFeatureType({"Unavailable", {{{"key", {}, IdPartDataType::STR}}}});
    auto road = graph->newLayer("Road", LayerType::Features);
    (void)road->newFeature("Road").id();
    (void)graph->newLayer("Lane", LayerType::Features)->newFeature("Lane");
    graph->finalize();
    DataSourceInfo info;
    info.mapId_ = "CrossLayer";
    info.stringPoolId_ = "CrossLayer";
    for (auto const& name : graph->layerNames()) {
        auto layer = std::make_shared<LayerInfo>();
        layer->layerId_ = name;
        info.layers_[name] = layer;
    }
    info.setMapSchema(graph);
    CHECK(info.getLayer("Road")->validFeatureId("Lane", {{"laneId", 1}}, false));
    CHECK_FALSE(info.getLayer("Road")->validFeatureId("Lane", {{"laneId", 1}}, true));
    auto strings = std::make_shared<StringPool>(info.stringPoolId_);
    auto tile = std::make_shared<PartitionFeatureLayer>(
        TileId::fromValue(131073),
        info.stringPoolId_,
        info.mapId_,
        info.getLayer("Road"),
        strings);
    // Unrelated reference identities need not share the source tile's prefix layout.
    REQUIRE_NOTHROW(tile->setIdPrefix({{"region", "DE"}}));
    auto feature = tile->newFeature("Road", {{"roadId", 7}});
    auto lane = tile->newFeatureId("Lane", {{"laneId", int64_t{-1}}});
    auto unavailable = tile->newFeatureId("Unavailable", {{"key", "target"}});
    CHECK(lane->toString() == "Lane.18446744073709551615");
    CHECK(unavailable->toString() == "Unavailable.target");
    feature->addRelation(tile->newRelation("lane", lane));
    feature->addRelation(tile->newRelation("missing-layer", unavailable));
    CHECK_THROWS(tile->newFeature("Lane", {{"laneId", 1}}));
    CHECK_THROWS(tile->newFeature("Unavailable", {{"key", "target"}}));
    CHECK_THROWS(tile->newFeatureId("Lane", {{"roadId", 1}}));
    CHECK_THROWS(tile->newFeatureId("Unknown", {{"id", 1}}));
    CHECK(tile->size() == 1);
    REQUIRE(tile->validate());
    FeatureLayerFilterRequest request;
    request.channels_.push_back({.channelId_ = "feature", .featureTypes_ = {"Road"}});
    REQUIRE(request.filterSource(*tile, false));
    request.channels_.front().featureTypes_ = {"Lane"};
    CHECK_FALSE(request.filterSource(*tile, false));

    // Decode both owners independently: no builder or layer-local identity copies survive.
    std::ostringstream schemaStream(std::ios::binary);
    REQUIRE(graph->write(schemaStream));
    auto schemaBytes = schemaStream.str();
    auto restoredGraph = MapSchema::read(
        std::span(reinterpret_cast<uint8_t const*>(schemaBytes.data()), schemaBytes.size()));
    auto restoredInfo =
        DataSourceInfo::fromJson(info.toJson(), [restoredGraph](auto) { return restoredGraph; });
    auto decoded = std::make_shared<PartitionFeatureLayer>(
        LayerSchemaBindingFixture::encode(*tile),
        [restoredInfo](auto, auto name) { return restoredInfo.getLayer(std::string(name)); },
        [strings](auto) { return strings; });
    REQUIRE(decoded->validate());
    CHECK(decoded->toJson() == tile->toJson());
    CHECK(decoded->at(0)->getRelation(0)->target()->keyValuePairs() == lane->keyValuePairs());
    CHECK(decoded->at(0)->getRelation(1)->target()->toString() == "Unavailable.target");
    CHECK(restoredGraph->featureIdentities().size() == 3);
    CHECK_FALSE(restoredGraph->layer("Unavailable"));
}

/**
 * Build the native common contracts once, then exercise real model allocations.
 * Each raw layer is only a validation root for a shared built-in type; the
 * runtime feature always belongs to Road. Decoding the graph first also checks
 * that binary publication preserves the factories' runtime-facing contracts.
 */
class NativeCommonShapeFixture
{
public:
    MapSchema::ConstPtr graph;
    PartitionFeatureLayer::Ptr tile;
    std::shared_ptr<StringPool> strings = std::make_shared<StringPool>("CommonShapes");

    explicit NativeCommonShapeFixture(bool decoded)
    {
        using Builtin = MapSchema::Builtin;
        auto owner = std::make_shared<MapSchema>();
        auto road = owner->newLayer("Road", LayerType::Features);
        auto root =
            road
                ->newFeature(FeatureTypeInfo::fromJson(
                    {{"name", "Road"},
                     {"uniqueIdCompositions", {{{{"partId", "roadId"}, {"datatype", "U32"}}}}}}))
                .id();
        REQUIRE(root != simfil::NoTypeId);
        auto attributes = road->addSchema(simfil::SchemaType::Kind::Object);
        owner->schemaType(road->attributeLayerMapSchema("Road"))
            .asObject()
            .addField("rules", owner->schemaType(attributes))
            .setMultimap(true);
        auto attribute = owner->newAttribute("access", "AccessAttribute").id();
        owner->schemaType(attribute)
            .asObject()
            .addField("value", owner->schemaType(owner->primitive(simfil::SchemaType::Kind::Int)));
        owner->schemaType(attribute)
            .asObject()
            .addField("link", owner->schemaType(owner->builtin(Builtin::RelationReference)));
        owner->schemaType(attributes)
            .asObject()
            .addField("access", owner->schemaType(attribute))
            .setMultimap(true);
        road->bindAttribute(owner->schemaType(attribute), "Road", "rules", "access");
        owner->newLayer("Attribute", LayerType::SourceData)->setRootSchema(attribute);
        for (auto const& [name, builtin] : std::array{
                 std::pair{"SourceDataReference", Builtin::SourceDataReference},
                 std::pair{"SourceDataReferences", Builtin::SourceDataReferences},
                 std::pair{"Geometry", Builtin::Geometry},
                 std::pair{"FeatureId", Builtin::FeatureId},
                 std::pair{"Validity", Builtin::Validity},
                 std::pair{"Validities", Builtin::Validities},
                 std::pair{"Relation", Builtin::Relation},
                 std::pair{"RelationReference", Builtin::RelationReference},
                 std::pair{"AttrPoint", Builtin::AttrPoint},
                 std::pair{"AttrPointSequence", Builtin::AttrPointSequence},
                 std::pair{"AttrPointIndex", Builtin::AttrPointIndex},
                 std::pair{"AttrPointIndexRange", Builtin::AttrPointIndexRange}})
            owner->newLayer(name, LayerType::SourceData)->setRootSchema(owner->builtin(builtin));
        owner->finalize();
        graph = owner;
        if (decoded) {
            std::ostringstream output;
            REQUIRE(owner->write(output));
            auto bytes = output.str();
            graph = MapSchema::read(std::vector<uint8_t>(bytes.begin(), bytes.end()));
        }
        DataSourceInfo info;
        info.mapId_ = "CommonMap";
        info.stringPoolId_ = "CommonShapes";
        auto layer = std::make_shared<LayerInfo>();
        layer->layerId_ = "Road";
        layer->type_ = LayerType::Features;
        info.layers_.emplace("Road", layer);
        info.setMapSchema(graph);
        tile = std::make_shared<PartitionFeatureLayer>(
            TileId::fromValue(131073),
            info.stringPoolId_,
            info.mapId_,
            info.getLayer("Road"),
            strings);
    }

    /** Real native provenance, shared by features, attributes, geometry and relations. */
    model_ptr<SourceDataReferenceCollection> sourceLinks() const
    {
        QualifiedSourceDataReference reference{
            .address_ = SourceDataAddress::fromBitPosition(8, 16),
            .layerId_ = strings->emplace("Raw").value(),
            .qualifier_ = strings->emplace("payload").value()};
        return tile->newSourceDataReferenceCollection({&reference, 1});
    }

    /** Check actual enumerated fields and the emitted JSON against the same native graph. */
    void accepts(std::string const& contract, simfil::ModelNode::Ptr const& node) const
    {
        CAPTURE(contract);
        REQUIRE(node);
        auto view = graph->layer(contract);
        REQUIRE(view);
        if (node->type() == simfil::ValueType::Object) {
            for (uint32_t i = 0; i < node->size(); ++i) {
                auto name = node->owningModel()->lookupStringId(node->keyAt(i));
                REQUIRE(name);
                CAPTURE(*name);
                CHECK(view->childSchema(view->rootSchema(), *name) != simfil::NoTypeId);
            }
        }
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(view->toJsonSchema());
        CHECK_NOTHROW(validator.validate(node->toJson()));
    }
};

}  // namespace

TEST_CASE(
    "Native common Feature and Attribute contracts accept runtime fields and canonical aliases",
    "[schema2][layer-binding][common-shapes]")
{
    NativeCommonShapeFixture fixture(GENERATE(false, true));
    auto feature = fixture.tile->newFeature("Road", {{"roadId", int64_t{1}}});
    fixture.accepts("Road", feature);
    CHECK_FALSE(feature->toJson().contains("geometry"));
    auto links = fixture.sourceLinks();
    feature->setSourceDataReferences(links);
    fixture.accepts("SourceDataReferences", links);
    fixture.accepts("SourceDataReference", links->at(0));

    auto rules = feature->attributeLayers()->newLayer("rules");
    auto access = rules->newAttribute("access");
    REQUIRE(access->addField("value", int64_t{30}));
    access->setSourceDataReferences(links);
    access->validity()->newDirection(Validity::Positive);
    fixture.accepts("Attribute", fixture.tile->resolve(access->addr()));
    fixture.accepts("Road", feature);
    // Repeating one attribute name must remain valid after its JSON value becomes an array.
    REQUIRE(rules->newAttribute("access")->addField("value", int64_t{50}));
    fixture.accepts("Road", feature);

    // Consumers enumerate through ModelNode; Feature keeps these overrides protected.
    simfil::ModelNode const& node = *feature;
    auto properties = node.get(StringPool::PropertiesStr);
    auto alias = node.get(StringPool::AttributesStr);
    REQUIRE(properties);
    REQUIRE(alias);
    CHECK(properties->addr() == alias->addr());
    CHECK(properties->toJson() == alias->toJson());
    auto view = fixture.graph->layer("Road");
    CHECK(
        view->childSchema(node.schema(), "attributes") ==
        view->childSchema(node.schema(), "properties"));
    size_t propertyFields = 0;
    for (uint32_t i = 0; i < node.size(); ++i) {
        CHECK(node.keyAt(i) != StringPool::AttributesStr);
        propertyFields += node.keyAt(i) == StringPool::PropertiesStr;
    }
    CHECK(propertyFields == 1);
    CHECK_FALSE(feature->toJson().contains("attributes"));
    auto fields = view->mapSchema()->schemaType(node.schema()).directFields();
    CHECK(std::ranges::count(fields, StringPool::PropertiesStr) == 1);
    CHECK(std::ranges::count(fields, StringPool::AttributesStr) == 0);
    CHECK(fixture.tile->checkForErrors().empty());
}

TEST_CASE(
    "Native common contracts accept relation validity cardinality and compact references",
    "[schema2][layer-binding][common-shapes]")
{
    NativeCommonShapeFixture fixture(GENERATE(false, true));
    auto multiple = GENERATE(false, true);
    auto feature = fixture.tile->newFeature("Road", {{"roadId", int64_t{1}}});
    auto foreign = fixture.tile->newFeatureId("OtherRoad", {{"key", "external"}}, "OtherMap");
    fixture.accepts("FeatureId", feature->id());
    fixture.accepts("FeatureId", foreign);
    auto relation = feature->addRelation(fixture.tile->newRelation("connectedTo", foreign));
    relation->sourceValidity()->newRange(Validity::RelativeLengthOffset, 0.25, 0.75);
    relation->targetValidity()->newFeatureId(foreign, Validity::Negative);
    if (multiple)
        relation->sourceValidity()->newDirection(Validity::Positive);
    relation->setSourceDataReferences(fixture.sourceLinks());
    auto current = fixture.tile->resolve(relation->addr());
    auto validity = current->get(StringPool::SourceValidityStr);
    REQUIRE(validity);
    CHECK(validity->type() == (multiple ? simfil::ValueType::Array : simfil::ValueType::Object));
    fixture.accepts("Validities", validity);
    fixture.accepts("Relation", current);

    auto reference = fixture.tile->newRelationReference(relation);
    auto attribute = feature->attributeLayers()->newLayer("rules")->newAttribute("access");
    REQUIRE(attribute->addField("link", reference));
    fixture.accepts("RelationReference", reference);
    fixture.accepts("Attribute", fixture.tile->resolve(attribute->addr()));
    fixture.accepts("Road", feature);
    auto json = reference->toJson();
    REQUIRE(json.size() == 1);
    CHECK(json.at("$mapgetRelation") == 0);
    REQUIRE(reference->get(StringPool::TargetStr));
    CHECK(reference->get(StringPool::TargetStr)->toJson() == foreign->toJson());
    CHECK(fixture.tile->checkForErrors().empty());
}

TEST_CASE(
    "Native common contracts accept runtime validity and attribute point representations",
    "[schema2][layer-binding][common-shapes]")
{
    NativeCommonShapeFixture fixture(GENERATE(false, true));
    auto feature = fixture.tile->newFeature("Road", {{"roadId", int64_t{1}}});
    auto target = fixture.tile->newFeature("Road", {{"roadId", int64_t{2}}});
    auto geometry = feature->geom()->newGeometry(GeomType::Line, 2, true);
    geometry->append({11, 48, 0});
    geometry->append({11.1, 48.1, 0});
    geometry->setName("centerline");
    geometry->setSourceDataReferences(fixture.sourceLinks());
    auto sequence = fixture.tile->newAttrPointSequence(feature, geometry);
    auto point = sequence->appendAttrPoint(1, {11.05, 48.05, 0}, fixture.sourceLinks());
    sequence->setSourceDataReferences(fixture.sourceLinks());
    fixture.accepts("Geometry", geometry);
    fixture.accepts("AttrPoint", point);
    fixture.accepts("AttrPointSequence", sequence);

    auto validities = fixture.tile->newValidityCollection();
    validities->newComplete();
    validities->newDirection(Validity::None);
    validities->newGeometry(geometry);
    validities->newPoint(Point{11.05, 48.05, 0});
    validities->newRange(Point{11, 48, 0}, Point{11.1, 48.1, 0});
    validities->newPoint(Validity::BufferOffset, int32_t{1}, "centerline");
    validities->newRange(Validity::MetricLengthOffset, 1.0, 10.0);
    validities->newFeatureId(target->id());
    validities->newGeomName("centerline");
    validities->newFeatureTransition(feature, Validity::End, target, Validity::Start, 3);
    auto index = validities->newAttrPointIndex(sequence, 1);
    auto range = validities->newAttrPointIndexRange(sequence, 0, 2);
    fixture.accepts("AttrPointIndex", index->attrPointIndex());
    fixture.accepts("AttrPointIndexRange", range->attrPointIndexRange());
    for (uint32_t i = 0; i < validities->size(); ++i) {
        CAPTURE(i);
        fixture.accepts("Validity", validities->at(i));
    }
    fixture.accepts("Validities", validities);
    fixture.accepts("Road", feature);
    CHECK(fixture.tile->checkForErrors().empty());
}

TEST_CASE("Partition memory reports include typed scalars, bitmasks and schema bindings", "[schema2][layer-binding][memory]")
{
    auto kind = GENERATE("feature", "source", "subset");
    CAPTURE(kind);
    LayerSchemaBindingFixture fixture, foreign("memory-foreign");
    PartitionLayer::Ptr layer;
    if (std::string_view(kind) == "subset")
        layer = std::make_shared<PartitionSubsetLayer>(
            TileId::fromValue(131073), fixture.info.stringPoolId_, fixture.info.mapId_,
            fixture.info.getLayer("Road"), fixture.strings, "memory", 1);
    else
        layer = fixture.native(std::string_view(kind) == "feature");
    auto& pool = dynamic_cast<simfil::ModelPool&>(*layer);
    auto beforePool = pool.memoryUsageStats().total();
    auto beforeLayer = layer->memoryUsage().total();

    auto scalar =
        pool.newTypedValue(int64_t{255}, fixture.graph->schemaType(fixture.values[0]));
    auto mask =
        pool.newBitmask(UINT64_C(0x8000000000000001), fixture.graph->schemaType(fixture.bitmask));
    CHECK(scalar->addr().column() == simfil::ModelPool::TypedScalar);
    CHECK(mask->addr().column() == simfil::ModelPool::Bitmask);
    // Native partitions keep one graph; only subset results import a foreign graph namespace.
    if (std::string_view(kind) == "subset") {
        auto projected =
            pool.newTypedValue(int64_t{255}, foreign.graph->schemaType(foreign.values[0]));
        CHECK(projected->schemaType().pool()->identity() == foreign.graph->schemaId());
    }
    else
        CHECK(pool.hasDirectSchemaPool(fixture.graph->schemaId()));

    auto stats = pool.memoryUsageStats();
    auto report = layer->memoryUsage();
    for (auto const& [name, expected] : std::array{
             std::pair{"model-pool.typed-scalars", stats.typedScalars},
             std::pair{"model-pool.bitmasks", stats.bitmasks},
             std::pair{"model-pool.schema-owners", stats.schemaOwners}})
    {
        CAPTURE(name);
        REQUIRE(expected.logicalBytes > 0);
        REQUIRE(expected.allocatedBytes > 0);
        REQUIRE(report.components.contains(name));
        CHECK(report.components.at(name).logicalBytes == expected.logicalBytes);
        CHECK(report.components.at(name).allocatedBytes == expected.allocatedBytes);
    }
    simfil::MemoryUsage reportedPool;
    for (auto const& [name, usage] : report.components)
        if (name.starts_with("model-pool."))
            reportedPool += usage;
    CHECK(reportedPool.logicalBytes == stats.total().logicalBytes);
    CHECK(reportedPool.allocatedBytes == stats.total().allocatedBytes);
    CHECK(report.total().logicalBytes - beforeLayer.logicalBytes == stats.total().logicalBytes - beforePool.logicalBytes);
    CHECK(report.total().allocatedBytes - beforeLayer.allocatedBytes == stats.total().allocatedBytes - beforePool.allocatedBytes);
}

TEST_CASE("Partition metadata cannot switch layer views inside the same graph", "[schema2][layer-binding]")
{
    auto feature = GENERATE(false, true);
    LayerSchemaBindingFixture fixture;
    auto tile = fixture.native(feature);
    auto original = tile->layerInfo();
    auto other = fixture.info.getLayer(feature ? "OtherRoad" : "OtherRaw");
    REQUIRE(other->schemaOwner() == original->schemaOwner());
    REQUIRE(other->type_ == original->type_);
    REQUIRE_THROWS_AS(tile->setLayerInfo(other), std::invalid_argument);
    CHECK(tile->layerInfo() == original);
    REQUIRE_THROWS_AS(tile->setLayerInfo(nullptr), std::invalid_argument);

    auto sameLayer = std::make_shared<LayerInfo>(*original);
    sameLayer->defaultVisible_ = false;
    REQUIRE_NOTHROW(tile->setLayerInfo(sameLayer));
    CHECK(tile->layerInfo() == sameLayer);
    CHECK(tile->schemaId() == fixture.graph->schemaId());
}

TEST_CASE("Native feature clones retain concrete property domains below a union", "[schema2][layer-binding][clone]")
{
    LayerSchemaBindingFixture fixture;
    auto source = fixture.features();
    auto target = fixture.features(true);
    PartitionFeatureLayer::CloneCache cache;
    for (size_t i = 0; i != fixture.properties.size(); ++i) {
        auto feature = source->newFeature("Road", {{"id", int64_t(i + 1)}});
        auto attributes = feature->attributes();
        REQUIRE(attributes->setSchema(fixture.properties[i]));
        REQUIRE(attributes->addField("value", source->newSmallValue(int16_t{255})));
        target->clone(cache, source, *feature, "Road", feature->id()->keyValuePairs());
        auto cloned = target->find("Road", feature->id()->keyValuePairs());
        REQUIRE(cloned);
        auto properties = cloned->attributesOrNull();
        REQUIRE(properties);
        CHECK(properties->schemaType() == attributes->schemaType());
        auto value = properties->at(0);
        REQUIRE(value);
        CHECK(value->addr().column() != simfil::ModelPool::TypedScalar);
        auto domain = simfil::Value::at(*properties, *value, 0).schemaType();
        CHECK(domain == fixture.graph->schemaType(fixture.values[i]));
        REQUIRE(domain);
        CHECK(domain.namedValue(i == 0 ? "UNLIMITED" : "UNKNOWN"));
    }

    // A second merge must not reinterpret an existing inline scalar using another alternative.
    auto incoming = source->find("Road.2");
    auto existing = target->find("Road.1");
    REQUIRE(incoming);
    REQUIRE(existing);
    REQUIRE_THROWS_AS(
        target->clone(cache, source, *incoming, "Road", existing->id()->keyValuePairs()),
        std::invalid_argument);
    CHECK(existing->attributesOrNull()->schema() == fixture.properties[0]);
    CHECK(existing->attributesOrNull()->size() == 1);

    auto restored = std::dynamic_pointer_cast<PartitionFeatureLayer>(fixture.decode(
        true, LayerSchemaBindingFixture::encode(*source)));
    REQUIRE(restored);
    CHECK(restored->find("Road.1")->attributesOrNull()->schema() == fixture.properties[0]);
    CHECK(restored->find("Road.2")->attributesOrNull()->schema() == fixture.properties[1]);
}

TEST_CASE("Native binary payload namespaces must match their outer schema header", "[schema2][layer-binding][binary]")
{
    auto feature = GENERATE(false, true);
    auto mode = GENERATE(std::string("exact"), std::string("foreign"), std::string("absent"), std::string("imported"));
    CAPTURE(feature, mode);
    LayerSchemaBindingFixture fixture, foreign("2");
    auto tile = fixture.native(feature);
    REQUIRE_NOTHROW(fixture.decode(feature, LayerSchemaBindingFixture::encode(*tile)));
    auto replacement = std::make_shared<simfil::ModelPool>(fixture.strings);
    if (mode == "exact" || mode == "foreign")
        replacement->setSchemaPool(mode == "exact" ? fixture.graph : foreign.graph);
    else if (mode == "imported")
        REQUIRE(replacement->importSchema(fixture.graph->schemaType(fixture.properties[0])));

    // Every replacement is a valid generic pool; even a resolvable foreign graph is not native.
    std::ostringstream encodedPool;
    REQUIRE(replacement->write(encodedPool));
    auto poolWire = encodedPool.str();
    auto probe = std::make_shared<simfil::ModelPool>(fixture.strings);
    probe->setSchemaResolver(
        [a = fixture.graph, b = foreign.graph](std::string_view id) {
            return id == a->schemaId() ? a : id == b->schemaId() ? b : nullptr;
        });
    REQUIRE(probe->read(std::vector<uint8_t>(poolWire.begin(), poolWire.end())));

    auto bytes = LayerSchemaBindingFixture::encode(*tile, replacement.get());
    if (mode == "exact")
        REQUIRE_NOTHROW(fixture.decode(feature, bytes, foreign.graph));
    else
        REQUIRE_THROWS(fixture.decode(feature, bytes, foreign.graph));
}

TEST_CASE(
    "Broad feature declarations retain concrete bindings independent of nullability",
    "[schema2][layer-binding][binary]")
{
    using Kind = simfil::SchemaType::Kind;
    auto kind = GENERATE(Kind::Unknown, Kind::Any, Kind::Object);
    auto nullable =
        GENERATE(std::optional<bool>{}, std::optional<bool>{false}, std::optional<bool>{true});
    CAPTURE(kind, nullable.has_value(), nullable.value_or(false));
    auto graph = std::make_shared<MapSchema>();
    auto layer = graph->newLayer("Road", LayerType::Features);
    auto declared = layer->addSchema(kind, LayerSchema::featureKey("Road"));
    graph->schemaType(declared).setNullable(nullable);
    auto concrete = graph->newObjectType();
    layer->addFeatureType({"Road", {{{"roadId", {}, IdPartDataType::U32}}}}, declared);
    layer->setRootSchema(declared);
    graph->finalize();

    DataSourceInfo info;
    info.mapId_ = "BroadFeatureMap";
    info.stringPoolId_ = "BroadFeatureBindings";
    auto metadata = std::make_shared<LayerInfo>();
    metadata->layerId_ = "Road";
    info.layers_.emplace(metadata->layerId_, metadata);
    info.setMapSchema(graph);
    auto strings = std::make_shared<StringPool>(info.stringPoolId_);
    auto source = std::make_shared<PartitionFeatureLayer>(
        TileId::fromValue(131073),
        info.stringPoolId_,
        info.mapId_,
        info.getLayer("Road"),
        strings);
    auto feature = source->newFeature("Road", {{"roadId", int64_t{1}}});
    if (kind == Kind::Object) {
        // Matching structure alone is not a declared refinement of a concrete object.
        REQUIRE_THROWS_AS(feature->setSchema(concrete.id()), std::invalid_argument);
        CHECK(feature->schemaType().id() == declared);
    }
    else {
        REQUIRE_NOTHROW(feature->setSchema(concrete.id()));
        CHECK(feature->schemaType().id() == concrete.id());
    }
    CHECK(source->checkForErrors().empty());

    auto restored = std::make_shared<PartitionFeatureLayer>(
        LayerSchemaBindingFixture::encode(*source),
        [info](auto const&, auto const& name) { return info.getLayer(std::string(name)); },
        [strings](auto const&) { return strings; },
        [graph](std::string_view id) -> MapSchema::ConstPtr
        { return id == graph->schemaId() ? graph : nullptr; });
    REQUIRE(restored->find("Road.1"));
    CHECK(restored->find("Road.1")->schemaType() == feature->schemaType());
    CHECK(restored->checkForErrors().empty());
}

TEST_CASE("Feature binary readers validate explicit domains in native records", "[schema2][layer-binding][binary]")
{
    auto mode = GENERATE("default", "declared", "concrete", "missing", "scalar", "object", "other-feature");
    CAPTURE(mode);
    LayerSchemaBindingFixture fixture("1", true);
    auto source = fixture.features();
    auto feature = source->newFeature("Road", {{"id", int64_t{1}}});
    feature->setSchema(fixture.concreteFeature);
    CHECK(source->checkForErrors().empty());
    auto bytes = LayerSchemaBindingFixture::encode(*source);

    // Decode the common header and fixed feature prefix, not a guessed byte pattern.
    size_t offset = 0;
    PartitionLayer header(bytes,
        [metadata = fixture.info](auto const&, auto const& name) {
            return metadata.getLayer(std::string(name));
        }, &offset);
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> input(Adapter(bytes.begin() + offset, bytes.end()));
    double coordinate = 0;
    input.value8b(coordinate);
    input.value8b(coordinate);
    input.value8b(coordinate);
    bool attachment = false;
    input.boolValue(attachment);
    REQUIRE_FALSE(attachment);
    uint64_t columnBytes = 0;
    input.value8b(columnBytes);
    // Wire record: two 16-bit type/domain IDs, then two 32-bit storage addresses.
    REQUIRE(columnBytes == 12);
    REQUIRE(input.adapter().error() == bitsery::ReaderError::NoError);
    auto record = offset + input.adapter().currentReadPos();
    auto domainOffset = record + sizeof(simfil::StringId);
    REQUIRE(domainOffset + sizeof(simfil::TypeId) <= bytes.size());
    auto declared = fixture.graph->layer("Road")->featureSchema("Road");
    simfil::TypeId replacement = simfil::NoTypeId;
    if (std::string_view(mode) == "declared") replacement = declared;
    else if (std::string_view(mode) == "concrete") replacement = fixture.concreteFeature;
    else if (std::string_view(mode) == "missing")
        replacement = simfil::MaxTypeId;
    else if (std::string_view(mode) == "scalar") replacement = fixture.values[0];
    else if (std::string_view(mode) == "object") replacement = fixture.properties[0];
    else if (std::string_view(mode) == "other-feature")
        replacement = fixture.graph->layer("OtherRoad")->featureSchema("Road");
    // ModelColumn records are raw native-layout bytes, including the explicit domain slot.
    std::memcpy(bytes.data() + domainOffset, &replacement, sizeof(replacement));
    if (std::string_view(mode) == "default" || std::string_view(mode) == "declared" ||
        std::string_view(mode) == "concrete") {
        auto restored = std::dynamic_pointer_cast<PartitionFeatureLayer>(fixture.decode(true, bytes));
        REQUIRE(restored);
        CHECK(
            restored->find("Road.1")->schemaType().id() == (replacement ? replacement : declared));
        CHECK(restored->checkForErrors().empty());
    }
    else
        REQUIRE_THROWS(fixture.decode(true, bytes));
}

TEST_CASE("Hydrated partitions release borrowed resolvers while retaining graph owners", "[schema2][layer-binding][binary]")
{
    auto kind = GENERATE("feature", "source", "subset");
    CAPTURE(kind);
    LayerSchemaBindingFixture fixture, foreign("2");
    PartitionLayer::Ptr layer;
    if (std::string_view(kind) == "subset") {
        auto subset = std::make_shared<PartitionSubsetLayer>(
            TileId::fromValue(131073), fixture.info.stringPoolId_, fixture.info.mapId_,
            fixture.info.getLayer("Road"), fixture.strings, "test", 1);
        auto imported = subset->importSchema(foreign.graph->schemaType(foreign.properties[0]));
        REQUIRE(imported);
        auto object = subset->newObject();
        REQUIRE(object->setSchema(*imported));
        REQUIRE(object->addField("value", subset->newSmallValue(int16_t{255})));
        auto slot = subset->newArray(1);
        slot->append(object);
        std::array<std::string, 1> fields{"properties"};
        auto channel = subset->newChannel("features", Scope::Feature, 0, {}, fields);
        std::array<simfil::ModelNode::Ptr, 1> slots{slot};
        auto geometry = subset->newGeometryCollection(1, true);
        auto point = subset->newGeometry(GeomType::Points, 1, true);
        point->append(Point{0, 0, 0});
        geometry->addGeometry(point);
        channel->newFeatureEntry(subset->newFeatureId("Road", {{"id", int64_t{1}}}), geometry, slots);
        layer = std::move(subset);
    }
    else
        layer = fixture.native(std::string_view(kind) == "feature");
    auto bytes = LayerSchemaBindingFixture::encode(*layer);
    auto layers = [metadata = fixture.info](auto const&, auto const& name) {
        return metadata.getLayer(std::string(name));
    };
    auto pools = [pool = fixture.strings](auto const&) { return pool; };
    PartitionLayer::Ptr restored;
    std::weak_ptr<int> resolverLifetime;
    {
        auto token = std::make_shared<int>(0);
        resolverLifetime = token;
        auto schemas = [token, owner = foreign.graph](std::string_view id) -> MapSchema::ConstPtr {
            ++*token;
            return id == owner->schemaId() ? owner : nullptr;
        };
        if (std::string_view(kind) == "subset") {
            restored = std::make_shared<PartitionSubsetLayer>(bytes, layers, pools, schemas);
            REQUIRE(*token == 1);
        }
        else if (std::string_view(kind) == "feature")
            restored = std::make_shared<PartitionFeatureLayer>(bytes, layers, pools, schemas);
        else
            restored = std::make_shared<PartitionSourceDataLayer>(bytes, layers, pools, schemas);
    }
    CHECK(resolverLifetime.expired());
    layer.reset();
    auto& model = dynamic_cast<simfil::ModelPool&>(*restored);
    CHECK_FALSE(model.schemaPool("uninstalled-artifact"));
    if (std::string_view(kind) == "subset") {
        auto id = foreign.graph->schemaId();
        auto valueId = foreign.values[0];
        foreign.info = {};
        foreign.graph.reset();
        auto owner = model.schemaPool(id);
        REQUIRE(owner);
        REQUIRE(owner->schemaType(valueId));
        CHECK(owner->schemaType(valueId).namedValue("UNLIMITED"));
    }
    else
        CHECK(model.hasDirectSchemaPool(fixture.graph->schemaId()));
}
