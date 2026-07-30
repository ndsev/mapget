#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "mapget/model/featurelayer-filter.h"
#include "mapget/model/stream.h"

using namespace mapget;

namespace
{

std::shared_ptr<LayerInfo> filterLayerInfo()
{
    return LayerInfo::fromJson(R"({
        "layerId": "FilterLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Road",
                "uniqueIdCompositions": [[
                    {"partId": "tileId", "description": "Tile.", "datatype": "U32"},
                    {"partId": "roadId", "description": "Road.", "datatype": "U64"}
                ]]
            }
        ]
    })"_json);
}

TileFeatureLayer::Ptr makeFilterSource()
{
    auto strings = std::make_shared<StringPool>("FilterPool");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId::fromTileXY(1, 0, 1),
        strings->stringPoolId_,
        "FilterMap",
        filterLayerInfo(),
        strings);
    source->setInfo("Producer/custom", 7);

    auto road = source->newFeature(
        "Road",
        {{"tileId", int64_t(1)}, {"roadId", int64_t(42)}});
    auto line = source->newGeometry(GeomType::Line, 2);
    line->setName("centerline");
    line->append(Point(11.0, 48.0, 0.0));
    line->append(Point(11.1, 48.1, 0.0));
    road->addGeometry(line);
    auto speedLimit =
        road->attributeLayers()
            ->newLayer("rules")
            ->newAttribute("speedLimit");
    speedLimit->addField("limit", source->newValue(int64_t(80)));
    return source;
}

TileFeatureLayer::Ptr makePointGroupSource(
    TileId tileId,
    int64_t roadId,
    Point point)
{
    auto strings =
        std::make_shared<StringPool>("FilterPool");
    auto source = std::make_shared<TileFeatureLayer>(
        tileId,
        strings->stringPoolId_,
        "FilterMap",
        filterLayerInfo(),
        strings);
    auto road = source->newFeature(
        "Road",
        {
            {
                "tileId",
                static_cast<int64_t>(
                    tileId.value())},
            {"roadId", roadId},
        });
    auto geometry =
        source->newGeometry(GeomType::Points, 1);
    geometry->setName("merge");
    geometry->append(point);
    road->addGeometry(geometry);
    return source;
}

TileFeatureLayer::Ptr makeRelationSource()
{
    auto strings =
        std::make_shared<StringPool>("FilterPool");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId::fromTileXY(1, 0, 1),
        strings->stringPoolId_,
        "FilterMap",
        filterLayerInfo(),
        strings);
    auto first = source->newFeature(
        "Road",
        {{"tileId", int64_t{1}},
         {"roadId", int64_t{1}}});
    auto second = source->newFeature(
        "Road",
        {{"tileId", int64_t{1}},
         {"roadId", int64_t{2}}});
    for (auto& [feature, longitude] :
         std::array<std::pair<
             model_ptr<Feature>,
             double>, 2>{
             std::pair{first, 11.0},
             std::pair{second, 11.1}})
    {
        auto geometry =
            source->newGeometry(
                GeomType::Points,
                1);
        geometry->setName("relation");
        geometry->append(
            Point{longitude, 48.0, 0.0});
        feature->addGeometry(geometry);
    }
    first->addRelation(
        "connected",
        second->id());
    second->addRelation(
        "connected",
        first->id());
    return source;
}

} // namespace

TEST_CASE(
    "Feature-layer filter emits ordered feature and attribute channels",
    "[feature-layer-filter]")
{
    auto source = makeFilterSource();
    auto const sourceStringHighWatermark = source->strings()->highest();
    auto result = filterFeatureLayer(
        *source,
        FeatureLayerFilterRequest{
            .filterId_ = "style:roads",
            .generation_ = 9,
            .channels_ = {
                FeatureLayerFilterChannel{
                    .channelId_ = "roads",
                    .featureFilter_ = "enabled",
                    .entryFilter_ = "0",
                    .scope_ = FeatureLayerFilterScope::Feature,
                    .featureTypes_ = {"Road"},
                    .featureFields_ = {"typeId", "''"},
                    .geometryTypes_ =
                        uint32_t{1}
                        << static_cast<uint8_t>(GeomType::Line),
                    .geometryName_ = "centerline",
                },
                FeatureLayerFilterChannel{
                    .channelId_ = "speed-limits",
                    .featureFilter_ = "enabled",
                    .entryFilter_ =
                        "$hasValidity == false and limit > threshold",
                    .scope_ = FeatureLayerFilterScope::Attribute,
                    .featureTypes_ = {"Road"},
                    .featureFields_ = {"typeId"},
                    .entryFields_ = {
                        "limit",
                        "$hasValidity",
                        "$validityIndex",
                        "$validityCount",
                    },
                },
            },
            .bindings_ = {
                {"enabled", true},
                {"threshold", int64_t(40)},
            },
        });

    REQUIRE(result.has_value());
    REQUIRE(result->layer_);
    auto const& subset = result->layer_;
    REQUIRE(subset->filterId() == "style:roads");
    REQUIRE(subset->generation() == 9);
    REQUIRE(subset->size() == 2);
    REQUIRE(subset->dependencies().size() == 1);
    REQUIRE(subset->localSourceFeatureCount() == 1);
    REQUIRE(subset->info()["Producer/custom"] == 7);
    REQUIRE(subset->strings() == source->strings());
    REQUIRE(source->strings()->highest() == sourceStringHighWatermark);

    auto featureChannel = subset->at(0);
    REQUIRE(featureChannel->channelId() == "roads");
    REQUIRE(featureChannel->scope() == Scope::Feature);
    REQUIRE(featureChannel->featureFields() ==
            std::vector<std::string>{"typeId", "''"});
    REQUIRE(featureChannel->featureEntryCount() == 1);
    auto featureEntry = model_ptr<FeatureEntry>{};
    REQUIRE(featureChannel->forEachFeatureEntry([&](auto const& entry) {
        featureEntry = entry;
        return true;
    }));
    REQUIRE(featureEntry);
    REQUIRE(featureEntry->featureId()->toString() == "Road.1.42");
    REQUIRE(featureEntry->values()->toJson() ==
            nlohmann::json::array({"Road", ""}));
    REQUIRE(featureEntry->geometry()->numGeometries() == 1);
    std::optional<std::string_view> copiedGeometryName;
    featureEntry->geometry()->forEachGeometry([&](auto const& geometry) {
        copiedGeometryName = geometry->name();
        return false;
    });
    REQUIRE(copiedGeometryName ==
            std::optional<std::string_view>{"centerline"});

    auto attributeChannel = subset->at(1);
    REQUIRE(attributeChannel->channelId() == "speed-limits");
    REQUIRE(attributeChannel->scope() == Scope::Attribute);
    REQUIRE(attributeChannel->attributeValidityEntryCount() == 1);
    auto attributeEntry = model_ptr<AttributeValidityEntry>{};
    REQUIRE(attributeChannel->forEachAttributeValidityEntry(
        [&](auto const& entry) {
            attributeEntry = entry;
            return true;
        }));
    REQUIRE(attributeEntry);
    REQUIRE_FALSE(attributeEntry->hasValidity());
    REQUIRE(attributeEntry->validityIndex() == 0);
    REQUIRE(attributeEntry->validityCount() == 1);
    REQUIRE(attributeEntry->hostValues()->toJson() ==
            nlohmann::json::array({"Road"}));
    REQUIRE(attributeEntry->values()->toJson() ==
            nlohmann::json::array({80, false, 0, 1}));
    REQUIRE(subset->issues().empty());
}

TEST_CASE(
    "Filtered subsets preserve the datasource string-pool namespace",
    "[feature-layer-filter][stream]")
{
    auto strings = std::make_shared<StringPool>("FilterPool");
    auto info = filterLayerInfo();
    auto emptySource = std::make_shared<TileFeatureLayer>(
        TileId::fromTileXY(0, 0, 1),
        strings->stringPoolId_,
        "FilterMap",
        info,
        strings);
    FeatureLayerFilterRequest request{
        .filterId_ = "string-pool-regression",
        .generation_ = 1,
        .channels_ = {
            FeatureLayerFilterChannel{
                .channelId_ = "roads",
                .featureFilter_ = "enabled",
                .scope_ = FeatureLayerFilterScope::Feature,
                .featureTypes_ = {"Road"},
            },
        },
        .bindings_ = {{"enabled", true}},
    };

    auto emptyResult =
        filterFeatureLayer(*emptySource, request);
    REQUIRE(emptyResult.has_value());
    REQUIRE(emptyResult->layer_);

    std::string framedBytes;
    TileLayerStream::StringPoolOffsetMap offsets;
    TileLayerStream::Writer writer(
        [&](std::string bytes, auto) {
            framedBytes.append(bytes);
        },
        offsets);
    writer.write(emptyResult->layer_);

    auto populatedSource = std::make_shared<TileFeatureLayer>(
        TileId::fromTileXY(1, 0, 1),
        strings->stringPoolId_,
        "FilterMap",
        info,
        strings);
    populatedSource->newFeature(
        "Road",
        {{"tileId", int64_t{1}}, {"roadId", int64_t{42}}});
    auto populatedResult =
        filterFeatureLayer(*populatedSource, request);
    REQUIRE(populatedResult.has_value());
    REQUIRE(populatedResult->layer_);
    writer.write(populatedResult->layer_);

    std::vector<TileSubsetLayer::Ptr> streamed;
    TileLayerStream::Reader reader(
        [&](auto const&, auto const&) {
            return info;
        },
        [&](TileLayer::Ptr layer) {
            streamed.push_back(
                std::dynamic_pointer_cast<TileSubsetLayer>(
                    std::move(layer)));
        });
    reader.read(framedBytes);

    REQUIRE(streamed.size() == 2);
    REQUIRE(streamed[0]);
    REQUIRE(streamed[1]);
    auto populatedChannel = streamed[1]->at(0);
    REQUIRE(populatedChannel->featureEntryCount() == 1);
    model_ptr<FeatureEntry> populatedEntry;
    REQUIRE(populatedChannel->forEachFeatureEntry(
        [&](auto const& entry) {
            populatedEntry = entry;
            return true;
        }));
    REQUIRE(populatedEntry);
    REQUIRE(populatedEntry->featureId()->typeId() == "Road");
    REQUIRE(populatedEntry->featureId()->toString() == "Road.1.42");
}

TEST_CASE(
    "Filter compilation failure is channel-local and structured values become null",
    "[feature-layer-filter]")
{
    auto source = makeFilterSource();
    auto result = filterFeatureLayer(
        *source,
        FeatureLayerFilterRequest{
            .filterId_ = "fault-isolation",
            .channels_ = {
                FeatureLayerFilterChannel{
                    .channelId_ = "broken-filter",
                    .entryFilter_ = "1 +",
                    .scope_ = FeatureLayerFilterScope::Feature,
                    .featureFields_ = {"typeId"},
                },
                FeatureLayerFilterChannel{
                    .channelId_ = "surviving-channel",
                    .entryFilter_ = "''",
                    .scope_ = FeatureLayerFilterScope::Feature,
                    .featureFields_ = {"geometry", "typeId"},
                },
            },
        });

    REQUIRE(result.has_value());
    REQUIRE(result->layer_->at(0)->featureEntryCount() == 0);
    REQUIRE(result->layer_->at(1)->featureEntryCount() == 1);
    auto json = result->layer_->at(1)->toJson();
    REQUIRE(json["featureEntries"][0]["values"] ==
            nlohmann::json::array({nullptr, "Road"}));
    REQUIRE(result->layer_->issues().size() == 2);
    REQUIRE(result->layer_->issues()[0].occurrenceCount_ == 1);
    REQUIRE(result->layer_->issues()[1].occurrenceCount_ == 1);
}

TEST_CASE(
    "Single-channel filters preserve search-query SIMFIL diagnostics",
    "[feature-layer-filter][diagnostics]")
{
    auto source = makeFilterSource();
    auto result = filterFeatureLayer(
        *source,
        FeatureLayerFilterRequest{
            .filterId_ = "query-diagnostics",
            .channels_ = {
                FeatureLayerFilterChannel{
                    .channelId_ = "search-style:0",
                    .entryFilter_ = "typeId == 42",
                    .scope_ =
                        FeatureLayerFilterScope::Feature,
                },
            },
        });

    REQUIRE(result.has_value());
    REQUIRE(result->layer_);
    auto messages =
        simfil::diagnostics(
            result->layer_->diagnostics());
    REQUIRE(messages.has_value());
    REQUIRE_FALSE(messages->empty());
}

TEST_CASE(
    "Feature-layer source traversal honors cancellation at feature boundaries",
    "[feature-layer-filter][cancellation]")
{
    auto source = makeFilterSource();
    size_t cancellationChecks = 0;
    auto result = filterFeatureLayerSource(
        *source,
        FeatureLayerFilterRequest{
            .filterId_ = "cancelled-filter",
            .channels_ = {
                FeatureLayerFilterChannel{
                    .channelId_ = "roads",
                    .scope_ =
                        FeatureLayerFilterScope::Feature,
                },
            },
        },
        true,
        {},
        [&] {
            ++cancellationChecks;
            return true;
        });

    REQUIRE(result.has_value());
    REQUIRE(result->layer_);
    REQUIRE(cancellationChecks == 1);
    REQUIRE(
        result->layer_->at(0)
            ->featureEntryCount() == 0);
}

TEST_CASE(
    "Point groups evaluate completed cross-source feature arrays",
    "[feature-layer-filter][point-group]")
{
    auto first = makePointGroupSource(
        TileId::fromTileXY(1, 0, 1),
        41,
        Point{11.1, 48.1, 0.0});
    auto second = makePointGroupSource(
        TileId::fromTileXY(2, 0, 1),
        42,
        Point{11.2, 48.2, 0.0});
    FeatureLayerFilterRequest request{
        .filterId_ = "point-groups",
        .generation_ = 2,
        .channels_ = {
            FeatureLayerFilterChannel{
                .channelId_ = "merged-roads",
                .featureFilter_ =
                    "typeId == 'Road'",
                .scope_ =
                    FeatureLayerFilterScope::Feature,
                .featureTypes_ = {"Road"},
                .entryFields_ = {
                    "count($features.*)",
                    "typeId",
                },
                .geometryTypes_ =
                    uint32_t{1}
                    << static_cast<uint8_t>(
                           GeomType::Points),
                .geometryName_ = "merge",
                .group_ =
                    FeatureLayerPointGridGroup{
                        .origin_ = {0.0, 0.0, 0.0},
                        .cellSize_ = {1.0, 1.0, 1.0},
                    },
            },
        },
    };

    auto firstResult =
        filterFeatureLayerSource(
            *first,
            request,
            true);
    auto secondResult =
        filterFeatureLayerSource(
            *second,
            request,
            false);
    REQUIRE(firstResult.has_value());
    REQUIRE(secondResult.has_value());
    REQUIRE(firstResult->layer_);
    REQUIRE_FALSE(secondResult->layer_);
    REQUIRE(
        firstResult->pointGroupMembers_.size() == 1);
    REQUIRE(
        secondResult->pointGroupMembers_.size() == 1);

    auto members =
        firstResult->pointGroupMembers_;
    members.insert(
        members.end(),
        secondResult->pointGroupMembers_.begin(),
        secondResult->pointGroupMembers_.end());
    auto completion =
        completeFeatureLayerPointGroups(
            *firstResult->layer_,
            request,
            members);
    REQUIRE(completion.has_value());
    REQUIRE(completion->entriesAdded_ == 1);
    REQUIRE(completion->issues_.empty());

    auto channel = firstResult->layer_->at(0);
    REQUIRE(channel->scope() == Scope::Group);
    REQUIRE(channel->groupEntryCount() == 1);
    model_ptr<GroupEntry> group;
    REQUIRE(channel->forEachGroupEntry(
        [&](auto const& entry) {
            group = entry;
            return true;
        }));
    REQUIRE(group);
    REQUIRE(
        group->values()->toJson() ==
        nlohmann::json::array({2, "Road"}));
    REQUIRE(
        group->memberFeatureIds()->size() == 2);
    REQUIRE(
        firstResult->layer_
            ->resolve<FeatureId>(
                *group->memberFeatureIds()->at(0))
            ->addr() ==
        group->representativeFeatureId()->addr());
    REQUIRE(
        firstResult->layer_
            ->resolve<FeatureId>(
                *group->memberFeatureIds()->at(0))
            ->toString()
            .ends_with(".41"));
    REQUIRE(
        firstResult->layer_
            ->resolve<FeatureId>(
                *group->memberFeatureIds()->at(1))
            ->toString()
            .ends_with(".42"));
}

TEST_CASE(
    "Stored relations resolve local recursion and merge deterministic reverse pairs",
    "[feature-layer-filter][relation]")
{
    auto source = makeRelationSource();
    FeatureLayerFilterRequest request{
        .filterId_ = "relations",
        .generation_ = 4,
        .channels_ = {
            FeatureLayerFilterChannel{
                .channelId_ = "connected",
                .featureFilter_ =
                    "typeId == 'Road'",
                .entryFilter_ = "$twoway",
                .scope_ =
                    FeatureLayerFilterScope::Relation,
                .featureTypes_ = {"Road"},
                .featureFields_ = {"typeId"},
                .entryFields_ = {
                    "$source.typeId",
                    "$target.typeId",
                    "$twoway",
                },
                .geometryTypes_ =
                    uint32_t{1}
                    << static_cast<uint8_t>(
                           GeomType::Points),
                .geometryName_ = "relation",
                .relation_ =
                    FeatureLayerStoredRelationOptions{
                        .relationNamePattern_ =
                            "connected",
                        .recursive_ = true,
                        .mergeTwoway_ = true,
                    },
            },
        },
    };

    auto sourceResult =
        filterFeatureLayerSource(
            *source,
            request,
            true);
    REQUIRE(sourceResult.has_value());
    REQUIRE(sourceResult->layer_);
    REQUIRE(
        sourceResult
            ->relationDescriptors_.size() == 2);
    auto completion =
        completeFeatureLayerRelations(
            *sourceResult->layer_,
            request,
            sourceResult->relationDescriptors_,
            std::array<MapTileKey, 1>{
                MapTileKey(*source)});
    REQUIRE(completion.has_value());
    REQUIRE(completion->issues_.empty());
    REQUIRE(completion->entriesAdded_ == 1);
    REQUIRE(
        completion
            ->relationsSkippedOwnerOutsideCoverage_ ==
        0);

    auto channel = sourceResult->layer_->at(0);
    REQUIRE(channel->scope() == Scope::Relation);
    REQUIRE(channel->featureEntryCount() == 2);
    REQUIRE(channel->relationEntryCount() == 1);
    model_ptr<RelationEntry> relation;
    REQUIRE(channel->forEachRelationEntry(
        [&](auto const& entry) {
            relation = entry;
            return true;
        }));
    REQUIRE(relation);
    REQUIRE(relation->twoway());
    REQUIRE(
        relation->values()->toJson() ==
        nlohmann::json::array(
            {"Road", "Road", true}));
    REQUIRE(
        relation->sourceGeometry()
            ->numGeometries() == 1);
    REQUIRE(
        relation->targetGeometry()
            ->numGeometries() == 1);
}
