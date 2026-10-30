#include "mapget/model/mapschema.h"
#include "mapget/model/simfilutil.h"
#include "mapget/model/stream.h"
#include "mapget/model/subsetlayer.h"
#include "simfil/simfil.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <sstream>

using namespace mapget;

namespace
{
/** Own only copied outputs and wire artifacts; producer/evaluator owners die inside populate(). */
class SubsetSchemaFixture
{
public:
    PartitionSubsetLayer::Ptr subset;
    std::shared_ptr<StringPool> strings = std::make_shared<StringPool>("subset-schema2");
    std::array<std::shared_ptr<simfil::StringPool>, 2> sourceStrings;
    std::array<std::weak_ptr<simfil::ModelPool>, 2> sourceModels;
    std::array<std::weak_ptr<MapSchema const>, 2> originalOwners;
    std::array<std::string, 2> identities;
    std::array<simfil::SchemaId, 2> scalars{}, masks{}, compounds{};
    std::map<std::string, std::vector<uint8_t>, std::less<>> artifacts;
    nlohmann::json metadata;
    std::array<std::string, 2> sourceDictionaryBytes;
    std::string destinationDictionaryBytes;

    /** Snapshot exact dictionary bytes rather than checking only its number of entries. */
    static std::string dictionary(simfil::StringPool const& pool)
    {
        std::ostringstream output(std::ios::out | std::ios::binary);
        REQUIRE(pool.write(output, 0));
        return output.str();
    }

    /** Evaluate real expressions, then use inherited copySequence for every projection slot. */
    void populate()
    {
        for (auto name : {"Road", "id", "speed", "flags", "ordinary"})
            REQUIRE(strings->emplace(name));
        destinationDictionaryBytes = dictionary(*strings);
        std::array<simfil::ModelNode::Ptr, 2> roots;
        std::array<simfil::ModelNode::Ptr, 2> inlineCodes;
        std::array<simfil::Value, 2> selected{simfil::Value::undef(), simfil::Value::undef()};
        std::array<simfil::Value, 2> flags{simfil::Value::undef(), simfil::Value::undef()};
        simfil::Value computed = simfil::Value::undef();
        simfil::Value unknown = simfil::Value::undef();
        for (size_t i = 0; i < 2; ++i) {
            auto owner = std::make_shared<MapSchema>(MapFormat{"fixture", "2", {}});
            auto layer = owner->newLayer("Road", LayerType::Features);
            auto feature =
                layer->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey("Road"));
            compounds[i] = layer->addSchema(
                simfil::Schema::Kind::Object,
                LayerSchema::featurePropertiesKey("Road"));
            scalars[i] = layer->addSchema(simfil::Schema::Kind::Int);
            masks[i] = layer->addSchema(simfil::Schema::Kind::Bitmask);
            layer->setTypeName(scalars[i], i == 0 ? "fixture.Speed" : "fixture.Count");
            layer->setTypeName(masks[i], i == 0 ? "fixture.AccessA" : "fixture.AccessB");
            layer->addNamedValue(
                scalars[i],
                {.name = i == 0 ? "UNLIMITED" : "UNKNOWN_COUNT",
                 .value = int64_t{255},
                 .role = simfil::Schema::NamedValue::Role::Special,
                 .meaning = i == 0 ?
                     simfil::Schema::NamedValue::Meaning::Unbounded :
                     simfil::Schema::NamedValue::Meaning::Unknown});
            layer->setBitWidth(masks[i], 64);
            layer->addNamedValue(
                masks[i],
                {.name = "KNOWN",
                 .value = uint64_t{1},
                 .role = simfil::Schema::NamedValue::Role::Flag});
            layer->addFieldSchema(feature, "properties", compounds[i]);
            layer->addFieldSchema(compounds[i], "speed", scalars[i]);
            layer->addFieldSchema(compounds[i], "flags", masks[i]);
            layer->addFieldSchema(compounds[i], "ordinary");
            layer->addFeatureType(
                FeatureTypeInfo::fromJson(nlohmann::json::parse(R"({
                "name":"Road","uniqueIdCompositions":[[{"partId":"id","datatype":"U64"}]]
            })")),
                feature);
            layer->setRootSchema(feature);
            owner->finalize();
            identities[i] = owner->schemaId();
            originalOwners[i] = owner;
            std::ostringstream wire(std::ios::out | std::ios::binary);
            REQUIRE(owner->write(wire));
            auto encoded = wire.str();
            artifacts.emplace(identities[i], std::vector<uint8_t>(encoded.begin(), encoded.end()));

            if (i == 0) {
                DataSourceInfo info;
                info.mapId_ = "SubsetSchema2";
                info.stringPoolId_ = "subset-schema2";
                auto layerInfo = std::make_shared<LayerInfo>();
                layerInfo->layerId_ = "Road";
                info.layers_.emplace("Road", layerInfo);
                info.setMapSchema(owner);
                metadata = info.toJson();
                subset = std::make_shared<PartitionSubsetLayer>(
                    TileId::fromValue(131073),
                    info.stringPoolId_,
                    info.mapId_,
                    info.getLayer("Road"),
                    strings,
                    "typed",
                    3);
            }
            sourceStrings[i] = std::make_shared<simfil::StringPool>();
            // Deliberately different data IDs: graph IDs and data dictionary IDs are independent.
            if (i == 1)
                REQUIRE(sourceStrings[i]->emplace("padding"));
            for (auto name : {"speed", "flags", "ordinary"})
                REQUIRE(sourceStrings[i]->emplace(name));
            sourceDictionaryBytes[i] = dictionary(*sourceStrings[i]);
            auto source = std::make_shared<simfil::ModelPool>(sourceStrings[i]);
            sourceModels[i] = source;
            source->setSchemaBinding(owner->binding());
            auto root = source->newObject();
            REQUIRE(root->setSchema(compounds[i]));
            inlineCodes[i] = source->newSmallValue(int16_t{255});
            root->addField("speed", inlineCodes[i]);
            root->addField("ordinary", source->newSmallValue(int16_t{255}));
            root->addField(
                "flags",
                source->newBitmask(UINT64_C(0x8000000000000001), {owner->binding(), masks[i]}));
            roots[i] = root;
            auto env = makeEnvironment(sourceStrings[i]);
            installLayerSchema(*env, layer, sourceStrings[i]);
            auto evaluate = [&](std::string_view text)
            {
                auto expression = simfil::compile(
                    *env,
                    text,
                    {.any = false,
                     .rewriteMode = simfil::RewriteMode::Schema,
                     .rootSchema = compounds[i]});
                REQUIRE(expression);
                auto values = simfil::eval(*env, **expression, *root, nullptr);
                REQUIRE(values);
                REQUIRE(values->size() == 1);
                return values->front();
            };
            selected[i] = evaluate("speed");
            flags[i] = evaluate("flags");
            if (i == 0) {
                computed = evaluate("speed + 0");
                unknown = evaluate("ordinary");
            }
            CHECK(dictionary(*sourceStrings[i]) == sourceDictionaryBytes[i]);
        }
        REQUIRE(identities[0] != identities[1]);
        REQUIRE(scalars[0] == scalars[1]);
        REQUIRE(inlineCodes[0]->addr() == inlineCodes[1]->addr());
        REQUIRE(selected[0].schemaReference());
        REQUIRE(selected[1].schemaReference());
        REQUIRE(selected[0].schemaReference() != selected[1].schemaReference());
        REQUIRE_FALSE(computed.schemaReference());
        REQUIRE_FALSE(unknown.schemaReference());
        std::vector<simfil::Value> mixed{
            selected[0],
            selected[1],
            flags[0],
            flags[1],
            simfil::Value::field(roots[0]),
            simfil::Value::field(roots[1]),
            computed,
            unknown};
        auto slots = [&]
        {
            // This is the production ModelPool path, not manual wrapper/node construction.
            auto values = subset->copySequence(mixed);
            auto empty = subset->copySequence(std::span<simfil::Value const>{});
            REQUIRE(values);
            REQUIRE(empty);
            return std::vector<simfil::ModelNode::Ptr>{*values, *empty};
        };
        std::vector<std::string> fields{"mixed", "empty"};
        auto id = subset->newFeatureId("Road", {{"id", int64_t{1}}});
        (void)id->keyValuePairs();
        auto geometry = subset->newGeometryCollection(1, true);
        auto point = subset->newGeometry(GeomType::Points, 1, true);
        point->append(Point{0, 0, 0});
        geometry->addGeometry(point);
        auto feature = subset->newChannel("feature", Scope::Feature, 0, {}, fields);
        feature->newFeatureEntry(id, geometry, slots());
        auto attribute = subset->newChannel("attribute", Scope::Attribute, 0, {}, fields, fields);
        attribute->newAttributeValidityEntry(id, geometry, 0, false, 0, 1, slots(), slots());
        auto relation = subset->newChannel("relation", Scope::Relation, 0, {}, fields, fields);
        auto from = relation->newFeatureEntry(id, geometry, slots());
        auto to = relation->newFeatureEntry(id, geometry, slots());
        relation->newRelationEntry(
            "r",
            "r",
            "stored",
            RelationDirection::Forward,
            false,
            from,
            to,
            geometry,
            geometry,
            slots());
        auto group = subset->newChannel("group", Scope::Group, 0, {}, {}, fields);
        auto copiedKey = subset->copySequence(std::span(selected.data(), 1));
        REQUIRE(copiedKey);
        std::vector<model_ptr<FeatureId>> members{id};
        group->newGroupEntry((*copiedKey)->at(0), id, geometry, slots(), members);
        simfil::Trace trace{};
        trace.calls = 1;
        trace.totalus = std::chrono::microseconds{17};
        trace.values = mixed;
        subset->setTraces({{"mixed", std::move(trace)}});
        CHECK(subset->issues().empty());
        CHECK(dictionary(*strings) == destinationDictionaryBytes);
    }

    /** Check exact graph/domain identity, native columns and plain JSON's intentional scalar shape.
     */
    void checkSequence(simfil::ModelNode::Ptr const& sequence) const
    {
        REQUIRE(sequence);
        REQUIRE(sequence->size() == 8);
        for (size_t i = 0; i < 2; ++i) {
            auto scalar = sequence->at(i);
            REQUIRE(scalar->type() == simfil::ValueType::Int);
            CHECK(scalar->addr().column() == simfil::ModelPool::QualifiedScalar);
            CHECK(std::get<int64_t>(scalar->value()) == 255);
            auto domain = scalar->schemaReference();
            REQUIRE(domain);
            CHECK(domain.binding->identity() == identities[i]);
            CHECK(domain.id == scalars[i]);
            CHECK(domain.definition()->namedValue(i == 0 ? "UNLIMITED" : "UNKNOWN_COUNT"));
            CHECK(scalar->toJson() == 255);
            auto mask = sequence->at(i + 2);
            REQUIRE(mask->type() == simfil::ValueType::Bitmask);
            CHECK(mask->addr().column() == simfil::ModelPool::Bitmask);
            auto bits = std::get<simfil::Bitmask>(mask->value());
            CHECK(bits.bits == UINT64_C(0x8000000000000001));
            CHECK(bits.domain.binding->identity() == identities[i]);
            CHECK(bits.domain.id == masks[i]);
            CHECK(mask->toJson() == bits.toJson());
            auto compound = sequence->at(i + 4);
            REQUIRE(compound->type() == simfil::ValueType::Object);
            auto root = compound->schemaReference();
            REQUIRE(root);
            CHECK(root.binding->identity() == identities[i]);
            CHECK(root.id == compounds[i]);
            // Copying a bound subtree must not qualify every ordinary scalar leaf.
            auto leaf = compound->at(0);
            REQUIRE(leaf);
            CHECK(leaf->addr().column() != simfil::ModelPool::QualifiedScalar);
            CHECK_FALSE(leaf->schemaReference());
            CHECK(simfil::Value::at(*compound, *leaf, 0).schemaReference() == domain);
        }
        for (size_t i : {6, 7}) {
            auto plain = sequence->at(i);
            CHECK(plain->type() == simfil::ValueType::Int);
            CHECK(plain->addr().column() != simfil::ModelPool::QualifiedScalar);
            CHECK_FALSE(plain->schemaReference());
            CHECK(plain->toJson() == 255);
        }
        CHECK(sequence->at(0)->schemaReference() != sequence->at(1)->schemaReference());
    }

    /** Check every scope, attribute hosts, relation endpoints, group keys and trace copies. */
    void check(PartitionSubsetLayer const& layer) const
    {
        REQUIRE(layer.size() == 4);
        CHECK(layer.at(0)->scope() == Scope::Feature);
        CHECK(layer.at(0)->featureEntryCount() == 1);
        CHECK(layer.at(1)->scope() == Scope::Attribute);
        CHECK(layer.at(1)->attributeValidityEntryCount() == 1);
        CHECK(layer.at(2)->scope() == Scope::Relation);
        CHECK(layer.at(2)->featureEntryCount() == 2);
        CHECK(layer.at(2)->relationEntryCount() == 1);
        CHECK(layer.at(3)->scope() == Scope::Group);
        CHECK(layer.at(3)->groupEntryCount() == 1);
        auto checkSlots = [&](model_ptr<simfil::Array> const& values)
        {
            REQUIRE(values);
            REQUIRE(values->size() == 2);
            checkSequence(values->at(0));
            REQUIRE(values->at(1)->type() == simfil::ValueType::Array);
            CHECK(values->at(1)->size() == 0);
        };
        for (size_t i = 0; i < layer.size(); ++i) {
            auto channel = layer.at(i);
            channel->forEachFeatureEntry(
                [&](auto const& row)
                {
                    checkSlots(row->values());
                    return true;
                });
            channel->forEachAttributeValidityEntry(
                [&](auto const& row)
                {
                    checkSlots(row->hostValues());
                    checkSlots(row->values());
                    return true;
                });
            channel->forEachRelationEntry(
                [&](auto const& row)
                {
                    REQUIRE(row->sourceGeometry());
                    REQUIRE(row->targetGeometry());
                    CHECK(row->sourceGeometry()->numGeometries() == 1);
                    CHECK(row->targetGeometry()->numGeometries() == 1);
                    checkSlots(row->values());
                    return true;
                });
            channel->forEachGroupEntry(
                [&](auto const& row)
                {
                    checkSlots(row->values());
                    CHECK(row->groupKey()->schemaReference().binding->identity() == identities[0]);
                    return true;
                });
        }
        REQUIRE(layer.traceCount() == 1);
        CHECK(layer.traceAt(0)->calls() == 1);
        CHECK(layer.traceAt(0)->totalUs() == std::chrono::microseconds{17});
        checkSequence(layer.traceAt(0)->values());
        CHECK(dictionary(*strings) == destinationDictionaryBytes);
        for (size_t i = 0; i < 2; ++i)
            CHECK(dictionary(*sourceStrings[i]) == sourceDictionaryBytes[i]);
    }
};
}  // namespace

TEST_CASE(
    "Subset Schema2 values retain domains across scopes, traces and binary reload",
    "[test.subsetlayer][schema2]")
{
    SubsetSchemaFixture fixture;
    fixture.populate();
    for (size_t i = 0; i < 2; ++i) {
        CHECK(fixture.sourceModels[i].expired());
        CHECK_FALSE(fixture.originalOwners[i].expired());
    }
    fixture.check(*fixture.subset);
    std::ostringstream output(std::ios::out | std::ios::binary);
    REQUIRE(fixture.subset->write(output));
    // Serialization finalizes geometry metrics included in the persisted header.
    auto expectedJson = fixture.subset->toJson();
    CHECK(expectedJson["info"]["Filter/Geometry/Vertices#count"] == 1);
    auto encoded = output.str();
    std::vector<uint8_t> bytes(encoded.begin(), encoded.end());
    std::string framed;
    TileLayerStream::StringPoolOffsetMap offsets;
    TileLayerStream::Writer writer([&](std::string payload, auto) { framed += payload; }, offsets);
    writer.write(fixture.subset);
    fixture.subset.reset();
    for (auto const& owner : fixture.originalOwners)
        CHECK(owner.expired());

    std::map<std::string, MapSchema::ConstPtr, std::less<>> installed;
    SchemaResolveFun resolve = [&](std::string_view id) -> MapSchema::ConstPtr
    {
        auto bytes = fixture.artifacts.find(id);
        if (bytes == fixture.artifacts.end())
            return {};
        auto [it, inserted] = installed.try_emplace(std::string(id));
        if (inserted)
            it->second = MapSchema::read(bytes->second, id);
        return it->second;
    };
    auto info = DataSourceInfo::fromJson(fixture.metadata, resolve);
    auto layerInfo = [&](auto const&, auto const&)
    {
        return info.getLayer("Road");
    };
    auto restored = std::make_shared<PartitionSubsetLayer>(
        bytes,
        layerInfo,
        [&](auto const&) { return fixture.strings; },
        resolve);
    fixture.check(*restored);
    CHECK(restored->toJson() == expectedJson);
    CHECK(installed.size() == 2);

    PartitionSubsetLayer::Ptr streamed;
    TileLayerStream::Reader reader(
        layerInfo,
        [&](PartitionLayer::Ptr value)
        { streamed = std::dynamic_pointer_cast<PartitionSubsetLayer>(value); },
        nullptr,
        {},
        resolve);
    reader.read(framed);
    REQUIRE(reader.eos());
    REQUIRE(streamed);
    fixture.check(*streamed);
    CHECK(streamed->toJson() == expectedJson);
    auto streamedDictionary = SubsetSchemaFixture::dictionary(*streamed->strings());
    fixture.check(*streamed);
    CHECK(SubsetSchemaFixture::dictionary(*streamed->strings()) == streamedDictionary);

    SchemaResolveFun missingForeign = [&](std::string_view id) -> MapSchema::ConstPtr
    {
        return id == fixture.identities[0] ? installed.at(fixture.identities[0]) : nullptr;
    };
    CHECK_THROWS(PartitionSubsetLayer(
        bytes,
        layerInfo,
        [&](auto const&) { return fixture.strings; },
        missingForeign));
}
