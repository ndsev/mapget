#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#include "mapget/model/featurelayer.h"
#include "mapget/model/stream.h"
#include "mapget/model/subsetlayer.h"

using namespace mapget;
using namespace std::chrono_literals;

namespace
{
std::shared_ptr<LayerInfo> subsetLayerInfo()
{
    return LayerInfo::fromJson(R"({
        "layerId": "Road",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Road",
                "uniqueIdCompositions": [[
                    {"partId": "roadId", "datatype": "U64"}
                ]]
            }
        ]
    })"_json);
}

model_ptr<GeometryCollection> pointGeometry(
    TileSubsetLayer& layer,
    double x,
    std::optional<std::string_view> name = std::nullopt)
{
    auto collection = layer.newGeometryCollection(1, true);
    auto geometry = layer.newGeometry(GeomType::Points, 1, true);
    geometry->append(Point{x, x, 0.0});
    geometry->setName(name);
    collection->addGeometry(geometry);
    return collection;
}

model_ptr<GeometryCollection> transitionGeometry(TileSubsetLayer& layer)
{
    auto collection = layer.newGeometryCollection(1, true);
    auto geometry = layer.newGeometry(GeomType::Line, 5, true);
    for (auto const& point : {
             Point{0.0, 0.0, 0.0},
             Point{1.0, 0.0, 0.0},
             Point{1.0, 0.0, 0.0},
             Point{1.0, 0.0, 0.0},
             Point{1.0, 1.0, 0.0}})
    {
        geometry->append(point);
    }
    collection->addGeometry(geometry);
    return collection;
}

} // namespace

TEST_CASE(
    "TileSubsetLayer shares immutable empty projected-value arrays",
    "[test.subsetlayer]")
{
    auto info = subsetLayerInfo();
    auto strings = std::make_shared<StringPool>("EmptySubsetValues");
    auto subset = std::make_shared<TileSubsetLayer>(
        TileId::fromWgs84(11.0, 42.0, 13),
        "EmptySubsetValues",
        "TestMap",
        info,
        strings,
        "empty-values",
        1);
    auto channel = subset->newChannel(
        "feature-rule",
        Scope::Feature,
        1U << static_cast<uint8_t>(GeomType::Points),
        std::nullopt);
    auto geometry = pointGeometry(*subset, 1.0);
    auto first = channel->newFeatureEntry(
        subset->newFeatureId("Road", {{"roadId", int64_t{1}}}),
        geometry);
    auto second = channel->newFeatureEntry(
        subset->newFeatureId("Road", {{"roadId", int64_t{2}}}),
        geometry);

    REQUIRE(first->values()->size() == 0);
    REQUIRE(second->values()->addr() == first->values()->addr());
}

TEST_CASE(
    "TileSubsetLayer owns channel schemas and typed entries",
    "[test.subsetlayer]")
{
    auto info = subsetLayerInfo();
    auto strings = std::make_shared<StringPool>("SubsetNode");
    auto tileId = TileId::fromWgs84(11.0, 42.0, 13);

    auto source = std::make_shared<TileFeatureLayer>(
        tileId,
        "SubsetNode",
        "TestMap",
        info,
        strings);
    source->setInfo("Load/Backend#us", 42);
    source->setTimestamp(
        std::chrono::system_clock::time_point{1'725'000'000s});
    source->setTtl(4500ms);

    auto subset = std::make_shared<TileSubsetLayer>(
        tileId,
        "SubsetNode",
        "TestMap",
        info,
        strings,
        "styled-roads",
        7,
        12);
    subset->adoptSourceInfo(*source);
    subset->setGlbAttachmentName("road-mesh");

    auto haloTile = tileId.neighbour(1, 0);
    subset->setDependencies({
        {
            MapTileKey(LayerType::Features, "TestMap", "Road", haloTile),
            3,
        },
        {
            MapTileKey(LayerType::Features, "TestMap", "Road", tileId),
            11,
        },
    });
    subset->addIssue({
        "attribute-rule",
        "speedLimit",
        Scope::Attribute,
        "Synthetic issue",
        2,
    });

    auto road1 = subset->newFeatureId("Road", {{"roadId", int64_t{1}}});
    auto road2 = subset->newFeatureId("Road", {{"roadId", int64_t{2}}});
    auto road1Geometry = pointGeometry(*subset, 1.0, "display");
    auto road2Geometry = pointGeometry(*subset, 2.0, "display");

    std::vector<std::string> featureFields{"color", "width"};
    auto featureChannel = subset->newChannel(
        "feature-rule",
        Scope::Feature,
        1U << static_cast<uint8_t>(GeomType::Points),
        "display",
        featureFields);
    std::vector<simfil::ModelNode::Ptr> featureValues{
        subset->newValue("#ff8800"),
        subset->newValue(3.5),
    };
    auto featureEntry = featureChannel->newFeatureEntry(
        road1,
        road1Geometry,
        featureValues);

    std::vector<std::string> hostFields{"hostClass"};
    std::vector<std::string> attributeFields{"attributeColor"};
    auto attributeChannel = subset->newChannel(
        "attribute-rule",
        Scope::Attribute,
        1U << static_cast<uint8_t>(GeomType::Points),
        std::nullopt,
        hostFields,
        attributeFields);
    std::vector<simfil::ModelNode::Ptr> hostValues{
        subset->newValue("primary"),
    };
    std::vector<simfil::ModelNode::Ptr> attributeValues{
        subset->newValue("#ffff00"),
    };
    auto attributeEntry = attributeChannel->newAttributeValidityEntry(
        road1,
        road1Geometry,
        4,
        false,
        0,
        1,
        hostValues,
        attributeValues,
        "rules",
        "warningSign");

    std::vector<std::string> endpointFields{"endpointLabel"};
    std::vector<std::string> relationFields{"linkColor"};
    auto relationChannel = subset->newChannel(
        "relation-rule",
        Scope::Relation,
        1U << static_cast<uint8_t>(GeomType::Points),
        "display",
        endpointFields,
        relationFields);
    std::vector<simfil::ModelNode::Ptr> sourceValues{
        subset->newValue("from"),
    };
    std::vector<simfil::ModelNode::Ptr> targetValues{
        subset->newValue("to"),
    };
    auto sourceEntry = relationChannel->newFeatureEntry(
        road1,
        road1Geometry,
        sourceValues);
    auto targetEntry = relationChannel->newFeatureEntry(
        road2,
        road2Geometry,
        targetValues);
    std::vector<simfil::ModelNode::Ptr> relationValues{
        subset->newValue("#00ffff"),
    };
    auto relationEntry = relationChannel->newRelationEntry(
        "Road.1/connectedTo/0",
        "connectedTo",
        "stored",
        RelationDirection::Forward,
        true,
        sourceEntry,
        targetEntry,
        sourceEntry->geometry(),
        targetEntry->geometry(),
        relationValues);

    std::vector<std::string> groupFields{"mergeCount"};
    auto groupChannel = subset->newChannel(
        "group-rule",
        Scope::Group,
        1U << static_cast<uint8_t>(GeomType::Points),
        "display",
        {},
        groupFields);
    std::vector<simfil::ModelNode::Ptr> groupValues{
        subset->newValue(int64_t{2}),
    };
    std::vector<model_ptr<FeatureId>> members{road2, road1};
    auto groupEntry = groupChannel->newGroupEntry(
        subset->newValue("cell-1"),
        road1,
        road1Geometry,
        groupValues,
        members);

    REQUIRE(subset->filterId() == "styled-roads");
    REQUIRE(subset->generation() == 7);
    REQUIRE(subset->deliveryEpoch() == 12);
    REQUIRE(subset->timestamp() == source->timestamp());
    REQUIRE(subset->ttl() == source->ttl());
    REQUIRE(subset->size() == 4);
    REQUIRE(subset->info()["Load/Backend#us"] == 42);
    REQUIRE(subset->localSourceFeatureCount() == 11);
    REQUIRE(subset->dependencies().size() == 2);
    REQUIRE(subset->issues().size() == 1);
    REQUIRE(subset->glbAttachmentName() == "road-mesh");
    REQUIRE(subset->numVertices() == 2);

    REQUIRE(featureChannel->scope() == Scope::Feature);
    REQUIRE(featureChannel->geometryName() == "display");
    REQUIRE(featureChannel->featureFields() == featureFields);
    REQUIRE(featureChannel->entryFields().empty());
    REQUIRE(featureChannel->entryCount() == 1);
    REQUIRE(featureEntry->values()->size() == 2);

    REQUIRE(attributeChannel->hasWildcardGeometryName());
    REQUIRE(attributeChannel->entryCount() == 1);
    REQUIRE_FALSE(attributeEntry->hasValidity());
    REQUIRE(attributeEntry->validityIndex() == 0);
    REQUIRE(attributeEntry->validityCount() == 1);
    REQUIRE(attributeEntry->attributeLayer() == "rules");
    REQUIRE(attributeEntry->attributeName() == "warningSign");

    REQUIRE(relationChannel->featureEntryCount() == 2);
    REQUIRE(relationChannel->entryCount() == 1);
    REQUIRE(relationEntry->source()->addr() == sourceEntry->addr());
    REQUIRE(relationEntry->target()->addr() == targetEntry->addr());
    REQUIRE(relationEntry->sourceGeometry()->addr() == sourceEntry->geometry()->addr());
    REQUIRE(relationEntry->twoway());

    REQUIRE(groupChannel->entryCount() == 1);
    REQUIRE(groupEntry->memberFeatureIds()->size() == 2);
    REQUIRE(
        subset->resolve<FeatureId>(*groupEntry->memberFeatureIds()->at(0))
            ->toString() == "Road.1");
    REQUIRE(
        subset->resolve<FeatureId>(*groupEntry->memberFeatureIds()->at(1))
            ->toString() == "Road.2");

    REQUIRE_THROWS(featureChannel->newGroupEntry(
        subset->newValue("invalid"),
        road1,
        road1Geometry,
        {},
        members));
    REQUIRE_THROWS(subset->newChannel(
        "feature-rule",
        Scope::Feature,
        0,
        std::nullopt));

    std::stringstream output;
    REQUIRE(subset->write(output).has_value());
    REQUIRE(subset->info()["Filter/Geometry/Vertices#count"] == 2);
    auto bytesString = output.str();
    std::vector<uint8_t> bytes(bytesString.begin(), bytesString.end());

    size_t identityBytes = 0;
    auto identity = TileSubsetLayer::readFilterIdentity(
        bytes,
        [&](auto const&, auto const&) { return info; },
        &identityBytes);
    REQUIRE(identity.filterId_ == "styled-roads");
    REQUIRE(identity.generation_ == 7);
    REQUIRE(identity.deliveryEpoch_ == 12);
    REQUIRE(identityBytes > 0);
    REQUIRE(identityBytes < bytes.size());

    size_t metadataBytes = 0;
    auto metadata = TileSubsetLayer::readMetadata(
        bytes,
        [&](auto const&, auto const&) { return info; },
        &metadataBytes);
    REQUIRE(metadata.identity_ == identity);
    REQUIRE(metadata.dependencies_ == subset->dependencies());
    REQUIRE(metadata.issues_ == subset->issues());
    REQUIRE(metadata.glbAttachmentName_ == "road-mesh");
    REQUIRE(metadataBytes > identityBytes);
    REQUIRE(metadataBytes < bytes.size());

    auto parsed = std::make_shared<TileSubsetLayer>(
        bytes,
        [&](auto const&, auto const&) { return info; },
        [&](auto const&) { return strings; });
    REQUIRE(parsed->filterId() == subset->filterId());
    REQUIRE(parsed->generation() == subset->generation());
    REQUIRE(parsed->deliveryEpoch() == subset->deliveryEpoch());
    REQUIRE(parsed->timestamp() == source->timestamp());
    REQUIRE(parsed->ttl() == source->ttl());
    REQUIRE(parsed->toJson() == subset->toJson());
    REQUIRE(parsed->at(2)->scope() == Scope::Relation);
    REQUIRE(parsed->at(2)->featureEntryCount() == 2);
    REQUIRE(parsed->at(2)->relationEntryCount() == 1);

    std::string framedBytes;
    TileLayerStream::StringPoolOffsetMap offsets;
    TileLayerStream::Writer writer(
        [&](std::string bytes, auto) {
            framedBytes.append(bytes);
        },
        offsets);
    writer.write(subset);

    TileSubsetLayer::Ptr streamed;
    TileLayerStream::Reader reader(
        [&](auto const&, auto const&) {
            return info;
        },
        [&](TileLayer::Ptr layer) {
            streamed =
                std::dynamic_pointer_cast<TileSubsetLayer>(
                    std::move(layer));
        });
    reader.read(framedBytes);
    REQUIRE(streamed);
    REQUIRE(streamed->toJson() == subset->toJson());
}

TEST_CASE(
    "TileSubsetLayer round-trips semantic transition metadata",
    "[test.subsetlayer][transition]")
{
    auto info = subsetLayerInfo();
    auto strings = std::make_shared<StringPool>("TransitionSubset");
    auto subset = std::make_shared<TileSubsetLayer>(
        TileId::fromWgs84(11.0, 42.0, 13),
        "TransitionSubset",
        "TestMap",
        info,
        strings);
    auto host = subset->newFeatureId("Road", {{"roadId", int64_t{3}}});
    auto from = subset->newFeatureId("Road", {{"roadId", int64_t{1}}});
    auto to = subset->newFeatureId("Road", {{"roadId", int64_t{2}}});
    auto channel = subset->newChannel(
        "transition-rule",
        Scope::Attribute,
        1U << static_cast<uint8_t>(GeomType::Line),
        std::nullopt);
    auto entry = channel->newAttributeValidityEntry(
        host,
        transitionGeometry(*subset),
        0,
        true,
        0,
        1,
        {},
        {},
        "rules",
        "turn",
        ValidityData::FeatureTransition,
        from,
        ValidityData::End,
        to,
        ValidityData::Start,
        2);
    REQUIRE(entry->isFeatureTransition());
    REQUIRE(entry->transitionFromFeatureId()->toString() == "Road.1");
    REQUIRE(entry->transitionToFeatureId()->toString() == "Road.2");
    REQUIRE(entry->transitionFromConnectedEnd() == ValidityData::End);
    REQUIRE(entry->transitionToConnectedEnd() == ValidityData::Start);
    REQUIRE(entry->transitionPivotIndex() == 2);

    std::stringstream output;
    REQUIRE(subset->write(output).has_value());
    auto const serialized = output.str();
    auto parsed = std::make_shared<TileSubsetLayer>(
        std::vector<uint8_t>(serialized.begin(), serialized.end()),
        [&](auto const&, auto const&) { return info; },
        [&](auto const&) { return strings; });
    auto parsedEntry = model_ptr<AttributeValidityEntry>{};
    REQUIRE_FALSE(parsed->at(0)->forEachAttributeValidityEntry(
        [&](auto const& candidate) {
            parsedEntry = candidate;
            return false;
        }));
    REQUIRE(parsedEntry);
    REQUIRE(parsedEntry->isFeatureTransition());
    REQUIRE(parsedEntry->transitionFromFeatureId()->toString() == "Road.1");
    REQUIRE(parsedEntry->transitionToFeatureId()->toString() == "Road.2");
    REQUIRE(parsedEntry->transitionPivotIndex() == 2);
}
