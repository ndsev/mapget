#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#include "mapget/model/featurelayer-filter.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/incoming-relations.h"
#include "mapget/model/pointnode.h"
#include "mapget/model/stream.h"
#include "mapget/model/subsetlayer.h"

using namespace mapget;
using namespace std::chrono_literals;

namespace
{
template<typename T>
simfil::ModelNode::Ptr projectedScalar(PartitionSubsetLayer& layer, T const& value)
{
    auto sequence = layer.newArray(1, true);
    sequence->append(layer.newValue(value));
    return sequence;
}

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
    PartitionSubsetLayer& layer,
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

model_ptr<GeometryCollection> transitionGeometry(PartitionSubsetLayer& layer)
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
    "PartitionSubsetLayer shares immutable empty projected-value arrays",
    "[test.subsetlayer]")
{
    auto info = subsetLayerInfo();
    auto strings = std::make_shared<StringPool>("EmptySubsetValues");
    auto subset = std::make_shared<PartitionSubsetLayer>(
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

TEST_CASE("All subset scopes preserve row-local projection errors on the wire", "[test.subsetlayer][projection]")
{
    auto info = subsetLayerInfo();
    auto strings = std::make_shared<StringPool>("ProjectionErrors");
    auto subset = std::make_shared<PartitionSubsetLayer>(
        TileId::fromWgs84(11, 48, 13), "ProjectionErrors", "TestMap", info, strings);
    auto id = subset->newFeatureId("Road", {{"roadId", int64_t{1}}});
    auto geometry = pointGeometry(*subset, 1);
    std::vector<std::string> fields{"broken"};
    std::vector<simfil::ModelNode::Ptr> values{subset->newArray(1, true)};
    std::vector<ProjectedValueError> errors{{0, "materialization", "Missing destination field"}};
    auto feature = subset->newChannel("feature", Scope::Feature, 0, {}, fields);
    CHECK(feature->newFeatureEntry(id, geometry, values, errors)->valueErrors() == errors);
    auto attribute = subset->newChannel("attribute", Scope::Attribute, 0, {}, fields, fields);
    auto attributeEntry = attribute->newAttributeValidityEntry(id, geometry, 0, false, 0, 1,
        values, values, {}, {}, ValidityData::NoGeometry, {}, ValidityData::Start,
        {}, ValidityData::Start, AttributeValidityEntry::InvalidTransitionPivotIndex, errors, errors);
    CHECK(attributeEntry->hostValueErrors() == errors);
    CHECK(attributeEntry->valueErrors() == errors);
    auto relation = subset->newChannel("relation", Scope::Relation, 0, {}, {}, fields);
    auto endpoint = relation->newFeatureEntry(id, geometry);
    CHECK(relation->newRelationEntry("r", "r", "", RelationDirection::Forward, false,
        endpoint, endpoint, geometry, geometry, values, errors)->valueErrors() == errors);
    auto group = subset->newChannel("group", Scope::Group, 0, {}, {}, fields);
    std::vector<model_ptr<FeatureId>> members{id};
    CHECK(group->newGroupEntry(subset->newValue(int64_t{1}), id, geometry, values, members, errors)
        ->valueErrors() == errors);

    std::vector<simfil::ModelNode::Ptr> invalid{subset->newValue(int64_t{1})};
    CHECK_THROWS(feature->newFeatureEntry(id, geometry, invalid));
    const auto dictionarySize = strings->size();
    std::stringstream stream;
    REQUIRE(subset->write(stream));
    auto encoded = stream.str();
    auto decoded = std::make_shared<PartitionSubsetLayer>(
        std::vector<uint8_t>(encoded.begin(), encoded.end()),
        [&](auto const&, auto const&) { return info; }, [&](auto const&) { return strings; });
    CHECK(decoded->toJson() == subset->toJson());
    CHECK(strings->size() == dictionarySize);
    decoded->at(1)->forEachAttributeValidityEntry([&](auto const& entry) {
        CHECK(entry->hostValueErrors() == errors);
        CHECK(entry->valueErrors() == errors);
        return true;
    });
    decoded->at(2)->forEachRelationEntry([&](auto const& entry) {
        CHECK(entry->valueErrors() == errors);
        return true;
    });
    decoded->at(3)->forEachGroupEntry([&](auto const& entry) {
        CHECK(entry->valueErrors() == errors);
        return true;
    });
}

TEST_CASE("A revision follows the complete feature snapshot across reload and restricted inspection",
          "[test.subsetlayer][source-revision]")
{
    auto info = subsetLayerInfo();
    auto tileId = TileId::fromWgs84(11, 48, 13);
    auto makeSource = [&](int rawValue, uint32_t recordOffset, bool coldReload) {
        auto strings = std::make_shared<StringPool>("RoadDictionary");
        if (coldReload)
            REQUIRE(strings->emplace("unrelated field from a different partition"));
        auto tile = std::make_shared<PartitionFeatureLayer>(tileId, "RoadDictionary", "Map", info, strings);
        auto road = tile->newFeature("Road", {{"roadId", int64_t{1}}});
        tile->newFeature("Road", {{"roadId", int64_t{2}}});
        for (auto name : {"centerline", "ADAS"}) {
            auto geometry = tile->newGeometry(GeomType::Line, 2);
            geometry->setName(name);
            geometry->append({11.0, 48.0, 0});
            geometry->append({11.001, 48.001, 0});
            road->addGeometry(geometry);
            if (std::string_view(name) != "ADAS")
                continue;

            // The same buffer occupies two original slots. The first occurrence
            // is the axis of a validity that used to pull it ahead of centerline.
            road->addGeometry(geometry);
            auto sequence = tile->newAttrPointSequence(road, geometry);
            auto slope = road->attributeLayers()->newLayer("Adas")->newAttribute("SLOPE");
            slope->addField("raw", tile->newValue(int64_t{rawValue}));
            QualifiedSourceDataReference ref{
                .address_ = SourceDataAddress::fromBitPosition(recordOffset, 8),
                .layerId_ = strings->emplace("raw").value(),
                .qualifier_ = strings->emplace("sample").value()};
            slope->setSourceDataReferences(tile->newSourceDataReferenceCollection({&ref, 1}));
            auto validities = slope->validity();
            validities->newAttrPointIndexRange(sequence, 0, 1);
            auto bounded = validities->newRange(Validity::RelativeLengthOffset, 0.25, 0.75, "ADAS");
            bounded->setFeatureId(road->id());
        }
        if (coldReload) {
            tile->setTimestamp(std::chrono::system_clock::time_point{123s});
            tile->setTtl(19ms);
            tile->setInfo("Load/Time#us", 1);
        }
        return tile;
    };

    auto source = makeSource(-13, 48, false);
    auto cold = makeSource(-13, 48, true);
    auto const producerDictionary = source->strings()->highest();
    auto revision = source->sourceRevision();
    CHECK(source->strings()->highest() == producerDictionary);
    CHECK(cold->sourceRevision() == revision);
    CHECK(makeSource(-12, 48, false)->sourceRevision() != revision);
    CHECK(makeSource(-13, 56, false)->sourceRevision() != revision);
    auto reference = makeSource(-13, 48, false);
    auto spelling = makeSource(-13, 48, false);
    reference->at(0)->attributes()->addField("target", reference->at(1)->id());
    spelling->at(0)->attributes()->addField("target", reference->at(1)->id()->toString());
    CHECK(reference->sourceRevision() != spelling->sourceRevision());
    source->publishSourceSnapshot("sample");
    CHECK(source->sourceRevision() == revision);
    std::vector<std::string> selected{"Road.1"};
    auto restricted = cloneInspectionFeatures(source, selected);
    REQUIRE(restricted->size() == 1);
    CHECK(restricted->sourceId() == "sample");
    CHECK(restricted->sourceRevision() == revision);
    REQUIRE(restricted->numAttrPointSequences() == 1);
    CHECK(restricted->attrPointSequenceAt(0)->geometryIndex() == 1);
    auto clonedRoad = restricted->find("Road.1");
    REQUIRE(clonedRoad);
    REQUIRE(clonedRoad->geomOrNull()->numGeometries() == 3);
    CHECK(clonedRoad->toJson() == source->find("Road.1")->toJson());

    // The cheap header route used by erdblick must see the complete identity
    // before it decides whether to construct the expensive feature model.
    std::stringstream wire;
    REQUIRE(restricted->write(wire));
    auto encoded = wire.str();
    std::vector<uint8_t> bytes(encoded.begin(), encoded.end());
    auto metadata = [&](auto const&, auto const&) { return info; };
    PartitionLayer header(bytes, metadata);
    CHECK(header.sourceId() == "sample");
    CHECK(header.sourceRevision() == revision);
    auto decoded = std::make_shared<PartitionFeatureLayer>(
        bytes, metadata, [&](auto const&) { return source->strings(); });
    CHECK(decoded->sourceRevision() == revision);
    CHECK(decoded->toServiceJson()["sourceId"] == "sample");
    CHECK(decoded->find("Road.1")->toJson() == clonedRoad->toJson());

    source->publishSourceSnapshot("sample", "supplier-snapshot-19");
    CHECK(cloneInspectionFeatures(source, selected)->sourceRevision() == "supplier-snapshot-19");
}

TEST_CASE("Filtering captures the source revision once across all channels and rows",
          "[test.subsetlayer][source-revision]")
{
    struct CountedSource : PartitionFeatureLayer
    {
        using PartitionFeatureLayer::PartitionFeatureLayer;
        mutable size_t revisionReads = 0;

        std::string sourceRevision() const override
        {
            ++revisionReads;
            return PartitionFeatureLayer::sourceRevision();
        }
    };

    auto strings = std::make_shared<StringPool>("CountedSource");
    auto source = std::make_shared<CountedSource>(TileId::fromWgs84(11, 48, 13),
        "CountedSource", "Map", subsetLayerInfo(), strings);
    for (int64_t id = 0; id < 96; ++id)
        source->newFeature("Road", {{"roadId", id}})->addPoint({11, 48, 0});
    FeatureLayerFilterRequest request;
    request.channels_ = {{.channelId_ = "first"}, {.channelId_ = "second"}};
    auto result = request.filterSource(*source, true);
    REQUIRE(result);
    REQUIRE(result->layer_);
    CHECK(result->layer_->at(0)->featureEntryCount() == 96);
    CHECK(result->layer_->at(1)->featureEntryCount() == 96);
    CHECK(source->revisionReads == 1);
}

TEST_CASE(
    "Shape-point wire rows preserve original identity, position and independent slots",
    "[test.subsetlayer][shapepoint]")
{
    auto info = subsetLayerInfo();
    auto strings = std::make_shared<StringPool>("PointWirePool");
    auto subset = std::make_shared<PartitionSubsetLayer>(
        TileId::fromWgs84(11, 48, 13),
        strings->stringPoolId_,
        "PointMap",
        info,
        strings);
    subset->setGeometryAnchor({11.0, 48.0, 0.0});
    auto id = subset->newFeatureId("Road", {{"roadId", int64_t{7}}});
    auto const canonicalId = id->toString();
    std::vector<std::string> hostFields{"roadId"}, pointFields{"curvature", "slope"};
    auto channel =
        subset->newChannel("vertex", Scope::ShapePoint, 2, "ADAS", hostFields, pointFields);
    std::array hostSlots{projectedScalar(*subset, int64_t{7})};
    std::array pointSlots{
        projectedScalar(*subset, -0.15296),
        simfil::ModelNode::Ptr(subset->newArray(0))};
    std::array errors{ProjectedValueError{1, "evaluation", "Ambiguous slope"}};
    auto host = channel->newShapePointHost(id, hostSlots);
    auto geometry = channel->newShapePointGeometry(host, 1, GeomType::Line, "ADAS");
    Point location{11.0625, 48.25, 543.5};
    channel->newShapePointEntry(geometry, 1, location, pointSlots, errors);
    channel->newShapePointEntry(geometry, 3, location, pointSlots, errors);

    // The projection schema and geometry handle belong to exactly one channel.
    auto other = subset->newChannel("other", Scope::ShapePoint, 2, {});
    CHECK_THROWS(other->newShapePointEntry(geometry, 1, location));
    CHECK_THROWS(channel->newShapePointEntry(geometry, 1, location));
    CHECK_THROWS(channel->newShapePointGeometry(host, 0, GeomType::Line, "centerline"));
    CHECK_FALSE(channel->shapePointEntryAt(2));
    CHECK(subset->numVertices() == 2);
    CHECK(subset->info().at("Filter/Entries/ShapePoints#count") == 2);

    std::stringstream stream;
    REQUIRE(subset->write(stream));
    auto const encoded = stream.str();
    auto decoded = std::make_shared<PartitionSubsetLayer>(
        std::vector<uint8_t>(encoded.begin(), encoded.end()),
        [&](auto const&, auto const&) { return info; },
        [&](auto const&) { return strings; });
    auto rows = decoded->at(0);
    REQUIRE(rows->shapePointEntryCount() == 2);
    CHECK(rows->entryCount() == 2);
    auto first = rows->shapePointEntryAt(0);
    auto second = rows->shapePointEntryAt(1);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->featureId()->toString() == canonicalId);
    CHECK(first->geometryOrdinal() == 1);
    CHECK(first->sourceGeometryType() == GeomType::Line);
    CHECK(first->geometryName() == "ADAS");
    CHECK(first->pointIndex() == 1);
    CHECK(second->pointIndex() == 3);
    CHECK(first->position() == location);
    CHECK(first->hostValues()->addr() == second->hostValues()->addr());
    CHECK(first->valueErrors() == std::vector<ProjectedValueError>(errors.begin(), errors.end()));
    auto json = first->toJson();
    CHECK(json["values"] == nlohmann::json::array({{-0.15296}, nlohmann::json::array()}));
    CHECK(json["sourceGeometryType"] == "line");
    CHECK(json["position"] == nlohmann::json(location));
    auto generic = static_cast<simfil::ModelNode const&>(*first).get(StringPool::PositionStr);
    REQUIRE(generic);
    CHECK(generic->toJson() == json["position"]);
    CHECK(decoded->resolve<PointNode>(*generic)->toJson() == json["position"]);
    CHECK(rows->toJson()["shapePointEntries"][0] == json);
    std::vector<uint32_t> seen;
    CHECK_FALSE(rows->forEachShapePointEntry(
        [&](auto const& entry)
        {
            seen.push_back(entry->pointIndex());
            return false;
        }));
    CHECK(seen == std::vector<uint32_t>{1});
}

TEST_CASE("PartitionSubsetLayer owns channel schemas and typed entries", "[test.subsetlayer]")
{
    auto info = subsetLayerInfo();
    auto strings = std::make_shared<StringPool>("SubsetNode");
    auto tileId = TileId::fromWgs84(11.0, 42.0, 13);

    auto source =
        std::make_shared<PartitionFeatureLayer>(tileId, "SubsetNode", "TestMap", info, strings);
    source->setInfo("Load/Backend#us", 42);
    source->setTimestamp(
        std::chrono::system_clock::time_point{1'725'000'000s});
    source->setTtl(4500ms);
    source->addWarning("Duplicate Road.1; retained first record");
    source->setLegalInfo("Source copyright and terms: https://example.org/terms");

    auto subset = std::make_shared<
        PartitionSubsetLayer>(tileId, "SubsetNode", "TestMap", info, strings, "styled-roads", 7);
    subset->adoptSourceInfo(*source);
    subset->setGlbAttachmentName("road-mesh");

    auto haloTile = tileId.neighbour(1, 0);
    subset->setDependencies({
        {
            MapPartitionKey(LayerType::Features, "TestMap", "Road", haloTile),
            3,
            "roads",
            "halo-snapshot",
        },
        {
            MapPartitionKey(LayerType::Features, "TestMap", "Road", tileId),
            11,
            "roads",
            "host-snapshot",
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
        projectedScalar(*subset, "#ff8800"),
        projectedScalar(*subset, 3.5),
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
        projectedScalar(*subset, "primary"),
    };
    std::vector<simfil::ModelNode::Ptr> attributeValues{
        projectedScalar(*subset, "#ffff00"),
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
        projectedScalar(*subset, "from"),
    };
    std::vector<simfil::ModelNode::Ptr> targetValues{
        projectedScalar(*subset, "to"),
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
        projectedScalar(*subset, "#00ffff"),
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
        projectedScalar(*subset, int64_t{2}),
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
    REQUIRE(subset->timestamp() == source->timestamp());
    REQUIRE(subset->ttl() == source->ttl());
    REQUIRE(subset->legalInfo() == source->legalInfo());
    REQUIRE(subset->size() == 4);
    REQUIRE(subset->info()["Load/Backend#us"] == 42);
    REQUIRE(subset->localSourceFeatureCount() == 11);
    REQUIRE(subset->localSourceDependency()->sourceRevision_ == "host-snapshot");
    auto conflicting = *subset->localSourceDependency();
    conflicting.sourceRevision_ = "changed";
    REQUIRE_THROWS(subset->addDependency(conflicting));
    REQUIRE(subset->localSourceDependency()->sourceRevision_ == "host-snapshot");
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
    auto identity = PartitionSubsetLayer::readFilterIdentity(
        bytes,
        [&](auto const&, auto const&) { return info; },
        &identityBytes);
    REQUIRE(identity.filterId_ == "styled-roads");
    REQUIRE(identity.generation_ == 7);
    REQUIRE(identityBytes > 0);
    REQUIRE(identityBytes < bytes.size());

    size_t metadataBytes = 0;
    auto metadata = PartitionSubsetLayer::readMetadata(
        bytes,
        [&](auto const&, auto const&) { return info; },
        &metadataBytes);
    REQUIRE(metadata.identity_ == identity);
    REQUIRE(metadata.dependencies_ == subset->dependencies());
    REQUIRE(metadata.issues_ == subset->issues());
    REQUIRE(metadata.glbAttachmentName_ == "road-mesh");
    REQUIRE(metadataBytes > identityBytes);
    REQUIRE(metadataBytes < bytes.size());

    auto parsed = std::make_shared<PartitionSubsetLayer>(
        bytes,
        [&](auto const&, auto const&) { return info; },
        [&](auto const&) { return strings; });
    REQUIRE(parsed->filterId() == subset->filterId());
    REQUIRE(parsed->generation() == subset->generation());
    REQUIRE(parsed->timestamp() == source->timestamp());
    REQUIRE(parsed->ttl() == source->ttl());
    REQUIRE(parsed->warnings() == source->warnings());
    REQUIRE(parsed->legalInfo() == source->legalInfo());
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

    PartitionSubsetLayer::Ptr streamed;
    TileLayerStream::Reader reader(
        [&](auto const&, auto const&) { return info; },
        [&](PartitionLayer::Ptr layer)
        { streamed = std::dynamic_pointer_cast<PartitionSubsetLayer>(std::move(layer)); });
    reader.read(framedBytes);
    REQUIRE(streamed);
    REQUIRE(streamed->toJson() == subset->toJson());
}

TEST_CASE(
    "PartitionSubsetLayer round-trips semantic transition metadata",
    "[test.subsetlayer][transition]")
{
    auto info = subsetLayerInfo();
    auto strings = std::make_shared<StringPool>("TransitionSubset");
    auto subset = std::make_shared<PartitionSubsetLayer>(
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
    auto parsed = std::make_shared<PartitionSubsetLayer>(
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
