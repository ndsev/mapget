#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mapget/model/featurelayer-filter.h"
#include "mapget/model/simfilexpressioncache.h"
#include "mapget/model/simfilutil.h"
#include "mapget/model/stream.h"
#include "mapget/service/locate.h"

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

    auto road = source->newFeature("Road", {{"tileId", int64_t(1)}, {"roadId", int64_t(42)}});
    auto line = source->newGeometry(GeomType::Line, 2);
    line->setName("centerline");
    line->append(Point(11.0, 48.0, 0.0));
    line->append(Point(11.1, 48.1, 0.0));
    road->addGeometry(line);
    auto speedLimit = road->attributeLayers()->newLayer("rules")->newAttribute("speedLimit");
    speedLimit->addField("limit", source->newValue(int64_t(80)));
    return source;
}

TileFeatureLayer::Ptr makePointGroupSource(TileId tileId, int64_t roadId, Point point)
{
    auto strings = std::make_shared<StringPool>("FilterPool");
    auto source = std::make_shared<
        TileFeatureLayer>(tileId, strings->stringPoolId_, "FilterMap", filterLayerInfo(), strings);
    auto road = source->newFeature(
        "Road",
        {
            {"tileId", static_cast<int64_t>(tileId.value())},
            {"roadId", roadId},
        });
    auto geometry = source->newGeometry(GeomType::Points, 1);
    geometry->setName("merge");
    geometry->append(point);
    road->addGeometry(geometry);
    return source;
}

TileFeatureLayer::Ptr makeTransitionFilterSource()
{
    auto strings = std::make_shared<StringPool>("FilterPool");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId::fromTileXY(1, 0, 1),
        strings->stringPoolId_,
        "FilterMap",
        filterLayerInfo(),
        strings);
    auto newRoad = [&](int64_t id, std::vector<Point> const& points)
    {
        auto road = source->newFeature("Road", {{"tileId", int64_t{1}}, {"roadId", id}});
        auto geometry = source->newGeometry(GeomType::Line, points.size());
        for (auto const& point : points) {
            geometry->append(point);
        }
        road->addGeometry(geometry);
        return road;
    };
    auto from = newRoad(1, {{11.0, 48.0, 0.0}, {11.001, 48.0, 0.0}});
    auto to = newRoad(2, {{11.001, 48.0, 0.0}, {11.001, 48.001, 0.0}});
    auto host = source->newFeature("Road", {{"tileId", int64_t{1}}, {"roadId", int64_t{3}}});
    auto pivot = source->newGeometry(GeomType::Points, 1);
    pivot->append({11.001, 48.0, 0.0});
    host->addGeometry(pivot);
    host->attributeLayers()
        ->newLayer("rules")
        ->newAttribute("turn")
        ->validity()
        ->newFeatureTransition(from, ValidityData::End, to, ValidityData::Start, 7);
    return source;
}

TileFeatureLayer::Ptr makeAttrPointFilterSource()
{
    auto strings = std::make_shared<StringPool>("FilterPool");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId::fromTileXY(1, 0, 1),
        strings->stringPoolId_,
        "FilterMap",
        filterLayerInfo(),
        strings);
    auto road = source->newFeature("Road", {{"tileId", int64_t{1}}, {"roadId", int64_t{4}}});
    auto geometry = road->geom()->newGeometry(GeomType::Line, 3, true);
    geometry->append({11.0, 48.0, 0.0});
    geometry->append({11.5, 48.0, 0.0});
    geometry->append({12.0, 48.0, 0.0});
    auto sequence = source->newAttrPointSequence(road, geometry);
    sequence->appendAttrPoint(1, {11.25, 48.0, 0.0});
    sequence->appendAttrPoint(3, {11.75, 48.0, 0.0});
    road->attributeLayers()
        ->newLayer("rules")
        ->newAttribute("access")
        ->validity()
        ->newAttrPointIndexRange(sequence, 1, 3, Validity::Positive);
    return source;
}

TileFeatureLayer::Ptr makeRelationSource()
{
    auto strings = std::make_shared<StringPool>("FilterPool");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId::fromTileXY(1, 0, 1),
        strings->stringPoolId_,
        "FilterMap",
        filterLayerInfo(),
        strings);
    auto first = source->newFeature("Road", {{"tileId", int64_t{1}}, {"roadId", int64_t{1}}});
    auto second = source->newFeature("Road", {{"tileId", int64_t{1}}, {"roadId", int64_t{2}}});
    for (auto& [feature, longitude] : std::array<std::pair<model_ptr<Feature>, double>, 2>{
             std::pair{first, 11.0},
             std::pair{second, 11.1}})
    {
        auto geometry = source->newGeometry(GeomType::Points, 1);
        geometry->setName("relation");
        geometry->append(Point{longitude, 48.0, 0.0});
        feature->addGeometry(geometry);
    }
    first->addRelation("connected", second->id());
    second->addRelation("connected", first->id());
    return source;
}

}  // namespace

TEST_CASE(
    "Portable selector scans stop cooperatively when cancelled",
    "[feature-layer-filter][selector][cancellation]")
{
    auto source = makeFilterSource();
    size_t cancellationChecks = 0;
    auto selected = source->find(
        FeatureLayerSelector{
            .typeId_ = "Road",
            .featureFilter_ = "roadId == 42",
        },
        [&] { return ++cancellationChecks == 3; });

    REQUIRE(selected.has_value());
    REQUIRE(selected->empty());
    REQUIRE(cancellationChecks == 3);
}

TEST_CASE(
    "Portable selectors share tile setup and resolve computed feature IDs",
    "[feature-layer-filter][selector]")
{
    auto source = makeRelationSource();
    auto const firstId = source->at(0)->id()->toString();
    auto selectors = std::array{
        FeatureLayerSelector{.canonicalFeatureId_ = firstId},
        FeatureLayerSelector{
            .typeId_ = "Road",
            .featureFilter_ = "roadId == selectedRoadId",
            .bindings_ = {{"selectedRoadId", int64_t{2}}},
        },
        FeatureLayerSelector{
            .typeId_ = "Road",
            .featureIdExpression_ =
                "select(($features.*{typeId == selectedType}.id), selectedIndex)",
            .bindings_ =
                {
                    {"selectedType", std::string{"Road"}},
                    {"selectedIndex", int64_t{1}},
                },
        },
    };

    auto selected = source->find(selectors);

    REQUIRE(selected.has_value());
    REQUIRE(selected->size() == selectors.size());
    REQUIRE((*selected)[0].size() == 1);
    REQUIRE((*selected)[0].front()->id()->toString() == firstId);
    REQUIRE((*selected)[1].size() == 1);
    REQUIRE((*selected)[1].front()->id()->toString() == source->at(1)->id()->toString());
    REQUIRE((*selected)[2].size() == 1);
    REQUIRE((*selected)[2].front()->id()->toString() == source->at(1)->id()->toString());
}

TEST_CASE(
    "Portable selector compilation distinguishes typed bindings",
    "[feature-layer-filter][selector][expression-cache]")
{
    auto source = makeRelationSource();
    auto selectors = std::array{
        FeatureLayerSelector{
            .typeId_ = "Road",
            .featureFilter_ = "roadId == selectedRoadId",
            .bindings_ = {{"selectedRoadId", int64_t{1}}},
        },
        FeatureLayerSelector{
            .typeId_ = "Road",
            .featureFilter_ = "roadId == selectedRoadId",
            .bindings_ = {{"selectedRoadId", int64_t{2}}},
        },
    };
    SimfilExpressionCache cache;

    auto selected = source->find(selectors, {}, &cache);

    REQUIRE(selected);
    REQUIRE((*selected)[0].size() == 1);
    REQUIRE((*selected)[1].size() == 1);
    CHECK((*selected)[0].front()->id()->toString().ends_with(".1"));
    CHECK((*selected)[1].front()->id()->toString().ends_with(".2"));
    auto const statistics = cache.statistics();
    CHECK(statistics.entries == 2);
    CHECK(statistics.compiles == 2);
}

TEST_CASE(
    "SIMFIL expression cache retains deterministic compilation failures",
    "[feature-layer-filter][expression-cache]")
{
    auto strings = std::make_shared<StringPool>("FilterPool");
    auto environment = makeEnvironment(strings);
    SimfilExpressionCache cache;
    size_t compileCalls = 0;
    auto compile = [&]() -> tl::expected<simfil::ASTPtr, simfil::Error>
    {
        ++compileCalls;
        return simfil::compile(*environment, "(", false);
    };

    simfil::CompileOptions options{.any = false};
    auto first = cache.getOrCompile("(", options, nullptr, {}, compile);
    auto second = cache.getOrCompile("(", options, nullptr, {}, compile);

    REQUIRE_FALSE(first);
    REQUIRE_FALSE(second);
    CHECK(first.error() == second.error());
    CHECK(compileCalls == 1);
    auto const statistics = cache.statistics();
    CHECK(statistics.entries == 1);
    CHECK(statistics.hits == 1);
    CHECK(statistics.misses == 1);
    CHECK(statistics.compiles == 1);
    CHECK(statistics.failedCompiles == 1);
}

TEST_CASE(
    "SIMFIL expression cache compiles a concurrent first use once",
    "[feature-layer-filter][expression-cache]")
{
    auto strings = std::make_shared<StringPool>("FilterPool");
    auto environment = makeEnvironment(strings);
    SimfilExpressionCache cache;
    std::atomic_size_t compileCalls = 0;
    std::promise<void> start;
    auto ready = start.get_future().share();
    auto compile = [&]() -> tl::expected<simfil::ASTPtr, simfil::Error>
    {
        ++compileCalls;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return simfil::compile(*environment, "true", false);
    };

    std::array<std::future<tl::expected<simfil::SharedAST, simfil::Error>>, 8> workers;
    for (auto& worker : workers) {
        worker = std::async(
            std::launch::async,
            [&]
            {
                ready.wait();
                return cache.getOrCompile(
                    "true",
                    simfil::CompileOptions{.any = false},
                    nullptr,
                    {},
                    compile);
            });
    }
    start.set_value();

    simfil::SharedAST shared;
    for (auto& worker : workers) {
        auto result = worker.get();
        REQUIRE(result);
        if (!shared) {
            shared = *result;
        }
        else {
            CHECK(result->get() == shared.get());
        }
    }
    CHECK(compileCalls == 1);
    auto const statistics = cache.statistics();
    CHECK(statistics.entries == 1);
    CHECK(statistics.compiles == 1);
    CHECK(statistics.hits == workers.size() - 1);
}

TEST_CASE(
    "Feature-ID expression locate candidates roundtrip and resolve",
    "[feature-layer-filter][selector][locate]")
{
    auto source = makeRelationSource();
    auto candidate = LocateCandidate::fromFeatureIdExpression(
        source->id(),
        "Road",
        "select(($features.*{typeId == selectedType}.id), selectedIndex)",
        {
            {"selectedType", std::string{"Road"}},
            {"selectedIndex", int64_t{1}},
        });

    auto serialized = candidate.serialize();
    LocateCandidate roundtripped(serialized);
    auto selected = resolveLocateCandidate(roundtripped, *source);

    REQUIRE(
        serialized.at("selector").at("featureIdExpression") ==
        "select(($features.*{typeId == selectedType}.id), selectedIndex)");
    REQUIRE_FALSE(serialized.at("selector").contains("featureFilter"));
    REQUIRE(roundtripped.serialize() == serialized);
    REQUIRE(selected.has_value());
    REQUIRE(selected->size() == 1);
    REQUIRE(selected->front()->id()->toString() == source->at(1)->id()->toString());
}

TEST_CASE(
    "Feature-layer filter emits ordered feature and attribute channels",
    "[feature-layer-filter]")
{
    auto source = makeFilterSource();
    auto const sourceStringHighWatermark = source->strings()->highest();
    auto request = FeatureLayerFilterRequest{
        .filterId_ = "style:roads",
        .generation_ = 9,
        .channels_ =
            {
                FeatureLayerFilterChannel{
                    .channelId_ = "roads",
                    .featureFilter_ = "enabled",
                    .entryFilter_ = "0",
                    .scope_ = FeatureLayerFilterScope::Feature,
                    .featureTypes_ = {"Road"},
                    .featureFields_ = {"typeId", "''"},
                    .geometryTypes_ = uint32_t{1} << static_cast<uint8_t>(GeomType::Line),
                    .geometryName_ = "centerline",
                },
                FeatureLayerFilterChannel{
                    .channelId_ = "speed-limits",
                    .featureFilter_ = "enabled",
                    .entryFilter_ = "$hasValidity == false and limit > threshold",
                    .scope_ = FeatureLayerFilterScope::Attribute,
                    .featureTypes_ = {"Road"},
                    .featureFields_ = {"typeId"},
                    .entryFields_ =
                        {
                            "limit",
                            "$hasValidity",
                            "$validityIndex",
                            "$validityCount",
                        },
                },
            },
        .bindings_ =
            {
                {"enabled", true},
                {"threshold", int64_t(40)},
            },
    };
    auto result = request.filter(*source);

    REQUIRE(result.has_value());
    REQUIRE(*result);
    auto const& subset = *result;
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
    REQUIRE(featureChannel->featureFields() == std::vector<std::string>{"typeId", "''"});
    REQUIRE(featureChannel->featureEntryCount() == 1);
    auto featureEntry = model_ptr<FeatureEntry>{};
    REQUIRE(featureChannel->forEachFeatureEntry(
        [&](auto const& entry)
        {
            featureEntry = entry;
            return true;
        }));
    REQUIRE(featureEntry);
    REQUIRE(featureEntry->featureId()->toString() == "Road.1.42");
    REQUIRE(featureEntry->values()->toJson() == nlohmann::json::array({"Road", ""}));
    REQUIRE(featureEntry->geometry()->numGeometries() == 1);
    std::optional<std::string_view> copiedGeometryName;
    featureEntry->geometry()->forEachGeometry(
        [&](auto const& geometry)
        {
            copiedGeometryName = geometry->name();
            return false;
        });
    REQUIRE(copiedGeometryName == std::optional<std::string_view>{"centerline"});

    auto attributeChannel = subset->at(1);
    REQUIRE(attributeChannel->channelId() == "speed-limits");
    REQUIRE(attributeChannel->scope() == Scope::Attribute);
    REQUIRE(attributeChannel->attributeValidityEntryCount() == 1);
    auto attributeEntry = model_ptr<AttributeValidityEntry>{};
    REQUIRE(attributeChannel->forEachAttributeValidityEntry(
        [&](auto const& entry)
        {
            attributeEntry = entry;
            return true;
        }));
    REQUIRE(attributeEntry);
    REQUIRE(attributeEntry->featureId()->addr().value_ == featureEntry->featureId()->addr().value_);
    REQUIRE_FALSE(attributeEntry->hasValidity());
    REQUIRE(attributeEntry->validityIndex() == 0);
    REQUIRE(attributeEntry->validityCount() == 1);
    REQUIRE(attributeEntry->hostValues()->toJson() == nlohmann::json::array({"Road"}));
    REQUIRE(attributeEntry->values()->toJson() == nlohmann::json::array({80, false, 0, 1}));
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
        .channels_ =
            {
                FeatureLayerFilterChannel{
                    .channelId_ = "roads",
                    .featureFilter_ = "enabled",
                    .scope_ = FeatureLayerFilterScope::Feature,
                    .featureTypes_ = {"Road"},
                },
            },
        .bindings_ = {{"enabled", true}},
    };

    auto emptyResult = request.filter(*emptySource);
    REQUIRE(emptyResult.has_value());
    REQUIRE(*emptyResult);

    std::string framedBytes;
    TileLayerStream::StringPoolOffsetMap offsets;
    TileLayerStream::Writer
        writer([&](std::string bytes, auto) { framedBytes.append(bytes); }, offsets);
    writer.write(*emptyResult);

    auto populatedSource = std::make_shared<TileFeatureLayer>(
        TileId::fromTileXY(1, 0, 1),
        strings->stringPoolId_,
        "FilterMap",
        info,
        strings);
    populatedSource->newFeature("Road", {{"tileId", int64_t{1}}, {"roadId", int64_t{42}}});
    auto populatedResult = request.filter(*populatedSource);
    REQUIRE(populatedResult.has_value());
    REQUIRE(*populatedResult);
    writer.write(*populatedResult);

    std::vector<TileSubsetLayer::Ptr> streamed;
    TileLayerStream::Reader reader(
        [&](auto const&, auto const&) { return info; },
        [&](TileLayer::Ptr layer)
        { streamed.push_back(std::dynamic_pointer_cast<TileSubsetLayer>(std::move(layer))); });
    reader.read(framedBytes);

    REQUIRE(streamed.size() == 2);
    REQUIRE(streamed[0]);
    REQUIRE(streamed[1]);
    auto populatedChannel = streamed[1]->at(0);
    REQUIRE(populatedChannel->featureEntryCount() == 1);
    model_ptr<FeatureEntry> populatedEntry;
    REQUIRE(populatedChannel->forEachFeatureEntry(
        [&](auto const& entry)
        {
            populatedEntry = entry;
            return true;
        }));
    REQUIRE(populatedEntry);
    REQUIRE(populatedEntry->featureId()->typeId() == "Road");
    REQUIRE(populatedEntry->featureId()->toString() == "Road.1.42");
}

TEST_CASE(
    "Feature-layer filter preserves semantic transition metadata",
    "[feature-layer-filter][transition]")
{
    auto request = FeatureLayerFilterRequest{
        .filterId_ = "transition",
        .generation_ = 1,
        .channels_ =
            {
                FeatureLayerFilterChannel{
                    .channelId_ = "transition-rule",
                    .entryFilter_ = "true",
                    .scope_ = FeatureLayerFilterScope::Attribute,
                    .featureTypes_ = {"Road"},
                    .geometryTypes_ = uint32_t{1} << static_cast<uint8_t>(GeomType::Line),
                },
            },
    };
    auto result = request.filter(*makeTransitionFilterSource());
    REQUIRE(result);
    REQUIRE(*result);
    auto channel = (*result)->at(0);
    REQUIRE(channel->attributeValidityEntryCount() == 1);
    model_ptr<AttributeValidityEntry> entry;
    REQUIRE(channel->forEachAttributeValidityEntry(
        [&](auto const& value)
        {
            entry = value;
            return true;
        }));
    REQUIRE(entry);
    REQUIRE(entry->isFeatureTransition());
    REQUIRE(entry->transitionFromFeatureId()->toString() == "Road.1.1");
    REQUIRE(entry->transitionToFeatureId()->toString() == "Road.1.2");
    REQUIRE(entry->transitionFromConnectedEnd() == ValidityData::End);
    REQUIRE(entry->transitionToConnectedEnd() == ValidityData::Start);
    REQUIRE(entry->transitionPivotIndex().has_value());
    model_ptr<Geometry> line;
    entry->geometry()->forEachGeometry(
        [&](auto const& value)
        {
            line = value;
            return false;
        });
    REQUIRE(line);
    REQUIRE(line->numPoints() >= 5);
    REQUIRE(*entry->transitionPivotIndex() < line->numPoints());
}

TEST_CASE(
    "Feature-layer filter materializes AttrPoint validity geometry",
    "[feature-layer-filter][attr-point]")
{
    auto request = FeatureLayerFilterRequest{
        .filterId_ = "attribute-points",
        .generation_ = 1,
        .channels_ =
            {
                FeatureLayerFilterChannel{
                    .channelId_ = "access",
                    .entryFilter_ = "true",
                    .scope_ = FeatureLayerFilterScope::Attribute,
                    .featureTypes_ = {"Road"},
                    .geometryTypes_ = uint32_t{1} << static_cast<uint8_t>(GeomType::Line),
                },
            },
    };
    auto result = request.filter(*makeAttrPointFilterSource());
    REQUIRE(result);
    REQUIRE(*result);
    auto channel = (*result)->at(0);
    REQUIRE(channel->attributeValidityEntryCount() == 1);
    model_ptr<AttributeValidityEntry> entry;
    REQUIRE(channel->forEachAttributeValidityEntry(
        [&](auto const& value)
        {
            entry = value;
            return true;
        }));
    REQUIRE(entry);
    REQUIRE(entry->geometryDescriptionType() == ValidityData::AttrPointIndexRangeValidity);
    model_ptr<Geometry> line;
    entry->geometry()->forEachGeometry(
        [&](auto const& value)
        {
            line = value;
            return false;
        });
    REQUIRE(line);
    REQUIRE(line->geomType() == GeomType::Line);
    REQUIRE(
        line->toSelfContained().points_ ==
        std::vector<Point>{
            {11.25, 48.0, 0.0},
            {11.5, 48.0, 0.0},
            {11.75, 48.0, 0.0},
        });
    REQUIRE((*result)->issues().empty());
}

TEST_CASE(
    "Filter compilation failure is channel-local and structured values become null",
    "[feature-layer-filter]")
{
    auto source = makeFilterSource();
    auto request = FeatureLayerFilterRequest{
        .filterId_ = "fault-isolation",
        .channels_ =
            {
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
    };
    auto result = request.filter(*source);

    REQUIRE(result.has_value());
    REQUIRE((*result)->at(0)->featureEntryCount() == 0);
    REQUIRE((*result)->at(1)->featureEntryCount() == 1);
    auto json = (*result)->at(1)->toJson();
    REQUIRE(json["featureEntries"][0]["values"] == nlohmann::json::array({nullptr, "Road"}));
    REQUIRE((*result)->issues().size() == 2);
    REQUIRE((*result)->issues()[0].occurrenceCount_ == 1);
    REQUIRE((*result)->issues()[1].occurrenceCount_ == 1);
}

TEST_CASE(
    "Single-channel filters preserve search-query SIMFIL diagnostics",
    "[feature-layer-filter][diagnostics]")
{
    auto source = makeFilterSource();
    auto request = FeatureLayerFilterRequest{
        .filterId_ = "query-diagnostics",
        .channels_ =
            {
                FeatureLayerFilterChannel{
                    .channelId_ = "search-style:0",
                    .entryFilter_ = "typeId == 42",
                    .scope_ = FeatureLayerFilterScope::Feature,
                },
            },
    };
    auto result = request.filter(*source);

    REQUIRE(result.has_value());
    REQUIRE(*result);
    auto messages = simfil::diagnostics((*result)->diagnostics());
    REQUIRE(messages.has_value());
    REQUIRE_FALSE(messages->empty());
}

TEST_CASE(
    "Feature-layer source traversal honors cancellation at feature boundaries",
    "[feature-layer-filter][cancellation]")
{
    auto source = makeFilterSource();
    size_t cancellationChecks = 0;
    auto request = FeatureLayerFilterRequest{
        .filterId_ = "cancelled-filter",
        .channels_ =
            {
                FeatureLayerFilterChannel{
                    .channelId_ = "roads",
                    .scope_ = FeatureLayerFilterScope::Feature,
                },
            },
    };
    auto result = request.filterSource(
        *source,
        true,
        {},
        [&]
        {
            ++cancellationChecks;
            return true;
        });

    REQUIRE(result.has_value());
    REQUIRE(result->layer_);
    REQUIRE(cancellationChecks == 1);
    REQUIRE(result->layer_->at(0)->featureEntryCount() == 0);
}

TEST_CASE(
    "Point groups evaluate completed cross-source feature arrays",
    "[feature-layer-filter][point-group]")
{
    auto first = makePointGroupSource(TileId::fromTileXY(1, 0, 1), 41, Point{11.1, 48.1, 0.0});
    auto second = makePointGroupSource(TileId::fromTileXY(2, 0, 1), 42, Point{11.2, 48.2, 0.0});
    FeatureLayerFilterRequest request{
        .filterId_ = "point-groups",
        .generation_ = 2,
        .channels_ =
            {
                FeatureLayerFilterChannel{
                    .channelId_ = "merged-roads",
                    .featureFilter_ = "typeId == 'Road'",
                    .scope_ = FeatureLayerFilterScope::Feature,
                    .featureTypes_ = {"Road"},
                    .entryFields_ =
                        {
                            "count($features.*)",
                            "typeId",
                        },
                    .geometryTypes_ = uint32_t{1} << static_cast<uint8_t>(GeomType::Points),
                    .geometryName_ = "merge",
                    .group_ =
                        FeatureLayerPointGridGroup{
                            .origin_ = {0.0, 0.0, 0.0},
                            .cellSize_ = {1.0, 1.0, 1.0},
                        },
                },
            },
    };

    auto firstResult = request.filterSource(*first, true);
    auto secondResult = request.filterSource(*second, false);
    REQUIRE(firstResult.has_value());
    REQUIRE(secondResult.has_value());
    REQUIRE(firstResult->layer_);
    REQUIRE_FALSE(secondResult->layer_);
    REQUIRE(firstResult->pointGroupMembers_.size() == 1);
    REQUIRE(secondResult->pointGroupMembers_.size() == 1);

    auto members = firstResult->pointGroupMembers_;
    members.insert(
        members.end(),
        secondResult->pointGroupMembers_.begin(),
        secondResult->pointGroupMembers_.end());
    auto completion = request.completePointGroups(*firstResult->layer_, members);
    REQUIRE(completion.has_value());
    REQUIRE(completion->entriesAdded_ == 1);
    REQUIRE(completion->issues_.empty());

    auto channel = firstResult->layer_->at(0);
    REQUIRE(channel->scope() == Scope::Group);
    REQUIRE(channel->groupEntryCount() == 1);
    model_ptr<GroupEntry> group;
    REQUIRE(channel->forEachGroupEntry(
        [&](auto const& entry)
        {
            group = entry;
            return true;
        }));
    REQUIRE(group);
    REQUIRE(group->values()->toJson() == nlohmann::json::array({2, "Road"}));
    REQUIRE(group->memberFeatureIds()->size() == 2);
    REQUIRE(
        firstResult->layer_->resolve<FeatureId>(*group->memberFeatureIds()->at(0))->addr() ==
        group->representativeFeatureId()->addr());
    REQUIRE(firstResult->layer_->resolve<FeatureId>(*group->memberFeatureIds()->at(0))
                ->toString()
                .ends_with(".41"));
    REQUIRE(firstResult->layer_->resolve<FeatureId>(*group->memberFeatureIds()->at(1))
                ->toString()
                .ends_with(".42"));
}

TEST_CASE(
    "Filter source tiles share request-level SIMFIL compilation",
    "[feature-layer-filter][expression-cache]")
{
    auto first = makePointGroupSource(TileId::fromTileXY(1, 0, 1), 41, Point{11.1, 48.1, 0.0});
    auto second = makePointGroupSource(TileId::fromTileXY(2, 0, 1), 42, Point{11.2, 48.2, 0.0});
    FeatureLayerFilterRequest request{
        .filterId_ = "shared-compilation",
        .channels_ =
            {
                FeatureLayerFilterChannel{
                    .channelId_ = "roads",
                    .featureFilter_ = "typeId == 'Road'",
                    .scope_ = FeatureLayerFilterScope::Feature,
                },
            },
    };
    SimfilExpressionCache cache;

    auto firstResult = request.filterSource(*first, true, {}, {}, &cache);
    auto secondResult = request.filterSource(*second, true, {}, {}, &cache);

    REQUIRE(firstResult);
    REQUIRE(secondResult);
    CHECK(firstResult->layer_->at(0)->featureEntryCount() == 1);
    CHECK(secondResult->layer_->at(0)->featureEntryCount() == 1);
    auto const statistics = cache.statistics();
    CHECK(statistics.entries == 1);
    CHECK(statistics.compiles == 1);
    CHECK(statistics.hits >= 1);
}

TEST_CASE(
    "Stored relations resolve local recursion and merge deterministic reverse pairs",
    "[feature-layer-filter][relation]")
{
    auto source = makeRelationSource();
    FeatureLayerFilterRequest request{
        .filterId_ = "relations",
        .generation_ = 4,
        .channels_ =
            {
                FeatureLayerFilterChannel{
                    .channelId_ = "connected",
                    .featureFilter_ = "typeId == 'Road'",
                    .entryFilter_ = "$twoway",
                    .scope_ = FeatureLayerFilterScope::Relation,
                    .featureTypes_ = {"Road"},
                    .featureFields_ = {"typeId"},
                    .entryFields_ =
                        {
                            "$source.typeId",
                            "$target.typeId",
                            "$twoway",
                        },
                    .geometryTypes_ = uint32_t{1} << static_cast<uint8_t>(GeomType::Points),
                    .geometryName_ = "relation",
                    .relation_ =
                        FeatureLayerStoredRelationOptions{
                            .relationNamePattern_ = "connected",
                            .recursive_ = true,
                            .mergeTwoway_ = true,
                        },
                },
            },
    };

    auto sourceResult = request.filterSource(*source, true);
    REQUIRE(sourceResult.has_value());
    REQUIRE(sourceResult->layer_);
    REQUIRE(sourceResult->relationDescriptors_.size() == 2);
    auto completion = request.completeRelations(
        *sourceResult->layer_,
        sourceResult->relationDescriptors_,
        std::array<MapTileKey, 1>{MapTileKey(*source)});
    REQUIRE(completion.has_value());
    REQUIRE(completion->issues_.empty());
    REQUIRE(completion->entriesAdded_ == 1);
    REQUIRE(completion->relationsSkippedOwnerOutsideCoverage_ == 0);

    auto channel = sourceResult->layer_->at(0);
    REQUIRE(channel->scope() == Scope::Relation);
    REQUIRE(channel->featureEntryCount() == 2);
    REQUIRE(channel->relationEntryCount() == 1);
    model_ptr<RelationEntry> relation;
    REQUIRE(channel->forEachRelationEntry(
        [&](auto const& entry)
        {
            relation = entry;
            return true;
        }));
    REQUIRE(relation);
    REQUIRE(relation->twoway());
    REQUIRE(relation->values()->toJson() == nlohmann::json::array({"Road", "Road", true}));
    REQUIRE(relation->sourceGeometry()->numGeometries() == 1);
    REQUIRE(relation->targetGeometry()->numGeometries() == 1);
}

TEST_CASE(
    "Disabled relation channels do not plan relation targets",
    "[feature-layer-filter][relation]")
{
    auto source = makeRelationSource();
    FeatureLayerFilterRequest request{
        .filterId_ = "disabled-relations",
        .generation_ = 5,
        .channels_ =
            {
                FeatureLayerFilterChannel{
                    .channelId_ = "connected",
                    .entryFilter_ = "showTopology",
                    .scope_ = FeatureLayerFilterScope::Relation,
                    .featureTypes_ = {"Road"},
                    .relation_ =
                        FeatureLayerStoredRelationOptions{
                            .relationNamePattern_ = "connected",
                        },
                },
            },
        .bindings_ = {{"showTopology", false}},
    };

    auto sourceResult = request.filterSource(*source, true);
    REQUIRE(sourceResult.has_value());
    REQUIRE(sourceResult->layer_);
    REQUIRE(sourceResult->relationDescriptors_.empty());
}
