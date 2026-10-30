#include "mapget/model/featurelayer.h"
#include "mapget/model/mapschema.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/subsetlayer.h"

#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
    std::array<simfil::SchemaId, 2> properties{}, values{};
    simfil::SchemaId concreteFeature = simfil::NoSchemaId;
    simfil::SchemaId bitmask = simfil::NoSchemaId;

    explicit LayerSchemaBindingFixture(std::string version = "1", bool unionRoot = false)
        : graph(std::make_shared<MapSchema>(MapFormat{"layer-bindings", std::move(version), {}}))
    {
        auto road = graph->newLayer("Road", LayerType::Features);
        auto root = road->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey("Road"));
        concreteFeature = unionRoot ? road->addSchema(LayerSchema::FeatureKind) : root;
        if (unionRoot) {
            road->setComposition(root, simfil::Schema::Composition::OneOf);
            road->addAlternative(root, concreteFeature);
        }
        auto choice = road->addSchema(simfil::Schema::Kind::Object, LayerSchema::featurePropertiesKey("Road"));
        road->setComposition(choice, simfil::Schema::Composition::OneOf);
        road->addFieldSchema(concreteFeature, "properties", choice);
        for (size_t i = 0; i != properties.size(); ++i) {
            properties[i] = road->addSchema(simfil::Schema::Kind::Object);
            values[i] = road->addSchema(simfil::Schema::Kind::Int);
            road->addFieldSchema(properties[i], "value", values[i]);
            road->addAlternative(choice, properties[i]);
            road->addNamedValue(values[i], {
                .name = i == 0 ? "UNLIMITED" : "UNKNOWN",
                .value = int64_t{255},
                .role = simfil::Schema::NamedValue::Role::Special,
                .meaning = i == 0 ? simfil::Schema::NamedValue::Meaning::Unbounded :
                                   simfil::Schema::NamedValue::Meaning::Unknown});
        }
        bitmask = road->addSchema(simfil::Schema::Kind::Bitmask);
        road->setBitWidth(bitmask, 64);
        road->addFieldSchema(properties[0], "flags", bitmask);
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

}  // namespace

TEST_CASE("Partition memory reports include qualified scalars, bitmasks and schema bindings", "[schema2][layer-binding][memory]")
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

    auto scalar = pool.newQualifiedValue(int64_t{255}, {fixture.graph->binding(), fixture.values[0]});
    auto mask = pool.newBitmask(UINT64_C(0x8000000000000001), {fixture.graph->binding(), fixture.bitmask});
    CHECK(scalar->addr().column() == simfil::ModelPool::QualifiedScalar);
    CHECK(mask->addr().column() == simfil::ModelPool::Bitmask);
    // Native partitions keep one graph; only subset results import a foreign graph namespace.
    if (std::string_view(kind) == "subset") {
        auto projected = pool.newQualifiedValue(int64_t{255}, {foreign.graph->binding(), foreign.values[0]});
        CHECK(projected->schemaReference().binding->identity() == foreign.graph->schemaId());
    }
    else
        CHECK(pool.hasDirectSchemaBinding(fixture.graph->schemaId()));

    auto stats = pool.memoryUsageStats();
    auto report = layer->memoryUsage();
    for (auto const& [name, expected] : std::array{
        std::pair{"model-pool.qualified-scalars", stats.qualifiedScalars},
        std::pair{"model-pool.bitmasks", stats.bitmasks},
        std::pair{"model-pool.schema-bindings", stats.schemaBindings}}) {
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
        CHECK(properties->schemaReference() == attributes->schemaReference());
        auto value = properties->at(0);
        REQUIRE(value);
        CHECK(value->addr().column() != simfil::ModelPool::QualifiedScalar);
        auto domain = simfil::Value::at(*properties, *value, 0).schemaReference();
        CHECK(domain == simfil::SchemaReference{fixture.graph->binding(), fixture.values[i]});
        REQUIRE(domain.definition());
        CHECK(domain.definition()->namedValue(i == 0 ? "UNLIMITED" : "UNKNOWN"));
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
        replacement->setSchemaBinding(mode == "exact" ? fixture.graph->binding() : foreign.graph->binding());
    else if (mode == "imported")
        REQUIRE(replacement->importSchema({fixture.graph->binding(), fixture.properties[0]}));

    // Every replacement is a valid generic pool; even a resolvable foreign graph is not native.
    std::ostringstream encodedPool;
    REQUIRE(replacement->write(encodedPool));
    auto poolWire = encodedPool.str();
    auto probe = std::make_shared<simfil::ModelPool>(fixture.strings);
    probe->setSchemaResolver([a = fixture.graph, b = foreign.graph](std::string_view id) {
        return id == a->schemaId() ? a->binding() : id == b->schemaId() ? b->binding() : nullptr;
    });
    REQUIRE(probe->read(std::vector<uint8_t>(poolWire.begin(), poolWire.end())));

    auto bytes = LayerSchemaBindingFixture::encode(*tile, replacement.get());
    if (mode == "exact")
        REQUIRE_NOTHROW(fixture.decode(feature, bytes, foreign.graph));
    else
        REQUIRE_THROWS(fixture.decode(feature, bytes, foreign.graph));
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
    REQUIRE(domainOffset + sizeof(simfil::SchemaId) <= bytes.size());
    auto declared = fixture.graph->layer("Road")->featureSchema("Road");
    simfil::SchemaId replacement = simfil::NoSchemaId;
    if (std::string_view(mode) == "declared") replacement = declared;
    else if (std::string_view(mode) == "concrete") replacement = fixture.concreteFeature;
    else if (std::string_view(mode) == "missing") replacement = simfil::MaxSchemaId;
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
        CHECK(restored->find("Road.1")->schemaReference().id == (replacement ? replacement : declared));
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
        auto imported = subset->importSchema({foreign.graph->binding(), foreign.properties[0]});
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
    CHECK_FALSE(model.schemaBinding("uninstalled-artifact"));
    if (std::string_view(kind) == "subset") {
        auto id = foreign.graph->schemaId();
        auto valueId = foreign.values[0];
        foreign.info = {};
        foreign.graph.reset();
        auto binding = model.schemaBinding(id);
        REQUIRE(binding);
        REQUIRE(binding->lookup(valueId));
        CHECK(binding->lookup(valueId)->namedValue("UNLIMITED"));
    }
    else
        CHECK(model.hasDirectSchemaBinding(fixture.graph->schemaId()));
}
