#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "mapget/model/featureid.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/json-compare.h"
#include "nlohmann/json.hpp"
#include "simfil/byte-array.h"

using namespace mapget;
using namespace std::chrono_literals;
using namespace nlohmann::literals;

namespace
{

struct SourceRefSpec
{
    uint32_t bitOffset_;
    uint32_t bitSize_;
    std::string layerId_;
    std::string qualifier_;
};

std::shared_ptr<LayerInfo> makeRoadLayerInfo()
{
    return LayerInfo::fromJson(R"json(
    {
      "layerId": "RoadLayer",
      "type": "Features",
      "featureTypes": [
        {
          "name": "Road",
          "uniqueIdCompositions": [
            [
              {"partId": "tileId", "datatype": "I64", "isSynthetic": true},
              {"partId": "regionId", "datatype": "STR"},
              {"partId": "roadId", "datatype": "U32"}
            ]
          ]
        }
      ],
      "stages": 3,
      "stageLabels": ["Draft", "High-Fi", "ADAS"],
      "highFidelityStage": 1
    })json"_json);
}

std::shared_ptr<LayerInfo> makeGenericLayerInfo()
{
    return LayerInfo::fromJson(R"json(
    {
      "layerId": "GeoJsonAny",
      "type": "Features",
      "featureTypes": [
        {
          "name": "AnyFeature",
          "uniqueIdCompositions": [[
            {"partId": "tileId", "datatype": "I64", "isSynthetic": true},
            {"partId": "featureIndex", "datatype": "U32", "isSynthetic": true}
          ]]
        }
      ]
    })json"_json);
}

TileFeatureLayer::Ptr makeTile(
    int32_t tileId,
    std::shared_ptr<LayerInfo> const& layerInfo,
    std::string const& nodeId = "GeoJsonImportNode",
    std::string const& mapId = "GeoJsonImportMap")
{
    return std::make_shared<TileFeatureLayer>(
        TileId::fromValue(tileId),
        nodeId,
        mapId,
        layerInfo,
        std::make_shared<StringPool>(nodeId));
}

simfil::ModelNode::Ptr nullNode(TileFeatureLayer& tile)
{
    return tile.resolve<simfil::ModelNode>(
        simfil::ModelNodeAddress{simfil::Model::Null, 1},
        simfil::ScalarValueType{});
}

model_ptr<SourceDataReferenceCollection> makeSourceDataRefs(
    TileFeatureLayer& tile,
    std::initializer_list<SourceRefSpec> refs)
{
    std::vector<QualifiedSourceDataReference> values;
    values.reserve(refs.size());
    for (auto const& spec : refs) {
        auto layerId = tile.strings()->emplace(spec.layerId_);
        if (!layerId) {
            throw std::runtime_error(layerId.error().message);
        }
        auto qualifier = tile.strings()->emplace(spec.qualifier_);
        if (!qualifier) {
            throw std::runtime_error(qualifier.error().message);
        }
        values.push_back(QualifiedSourceDataReference{
            SourceDataAddress::fromBitPosition(spec.bitOffset_, spec.bitSize_),
            *layerId,
            *qualifier,
        });
    }
    return tile.newSourceDataReferenceCollection(values);
}

std::filesystem::path fixturePath(std::string const& fileName)
{
    return std::filesystem::path(__FILE__).parent_path() / "data" / fileName;
}

[[nodiscard]] nlohmann::json loadJsonFixture(std::string const& fileName)
{
    auto path = fixturePath(fileName);
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Failed to open fixture: " + path.string());
    }
    return nlohmann::json::parse(input);
}

template<typename T>
void requireExpectedOk(tl::expected<T, simfil::Error> const& result)
{
    REQUIRE(result);
}

}  // namespace

TEST_CASE("Feature ID strings roundtrip escaped string parts", "[FeatureId][GeoJsonImport]")
{
    auto layerInfo = makeRoadLayerInfo();

    ParsedFeatureId parsed;
    std::string error;
    REQUIRE(parseFeatureIdString("Road.77.DE%2EBY%25%3A%2F%2C%7E.7", *layerInfo, parsed, &error));
    REQUIRE(parsed.typeId_ == "Road");
    REQUIRE(parsed.keyValuePairs_.size() == 3);
    REQUIRE(parsed.keyValuePairs_[0].first == "tileId");
    REQUIRE(std::get<int64_t>(parsed.keyValuePairs_[0].second) == 77);
    REQUIRE(parsed.keyValuePairs_[1].first == "regionId");
    REQUIRE(std::get<std::string>(parsed.keyValuePairs_[1].second) == "DE.BY%:/,~");
    REQUIRE(parsed.keyValuePairs_[2].first == "roadId");
    REQUIRE(std::get<int64_t>(parsed.keyValuePairs_[2].second) == 7);

    auto tile = makeTile(131073, layerInfo, "FeatureIdNode");
    tile->setIdPrefix({{"tileId", static_cast<int64_t>(77)}, {"regionId", "DE.BY%:/,~"}});
    auto feature = tile->newFeature("Road", {{"roadId", 7}});
    REQUIRE(feature->id()->toString() == "Road.77.DE%2EBY%25%3A%2F%2C%7E.7");
    REQUIRE(tile->find("Road.77.DE%2EBY%25%3A%2F%2C%7E.7"));
}

TEST_CASE("TileFeatureLayer strict GeoJSON import roundtrips mapget JSON", "[GeoJsonImport]")
{
    auto layerInfo = makeRoadLayerInfo();
    auto tile = makeTile(131073, layerInfo, "StrictImportNode", "StrictImportMap");

    tile->setIdPrefix({{"tileId", static_cast<int64_t>(77)}, {"regionId", "DE.BY%"}});
    tile->setGeometryAnchor({11.3, 48.0, 5.0});
    tile->setTimestamp(std::chrono::system_clock::time_point{1714348800s} + 123456us);
    tile->setTtl(2500ms);
    tile->setError(std::optional<std::string>{"partially degraded"});
    tile->setErrorCode(std::optional<int>{206});

    auto roadA = tile->newFeature("Road", {{"roadId", 7}});
    auto roadALine = roadA->geom()->newGeometry(GeomType::Line, 3, true);
    roadALine->append({11.3, 48.0, 0.0});
    roadALine->append({11.31, 48.01, 0.0});
    roadALine->append({11.32, 48.015, 0.0});
    roadALine->setStage(2);
    roadALine->setSourceDataReferences(makeSourceDataRefs(
        *tile,
        {{10, 20, "road-src", "centerline"}}));

    auto roadAMesh = roadA->geom()->newGeometry(GeomType::Mesh, 3, true);
    roadAMesh->append({11.3, 48.0, 0.0});
    roadAMesh->append({11.3, 48.002, 0.0});
    roadAMesh->append({11.302, 48.001, 0.0});
    roadAMesh->setStage(2);

    roadA->setSourceDataReferences(makeSourceDataRefs(
        *tile,
        {{30, 40, "road-src", "feature"}}));
    requireExpectedOk(roadA->attributes()->addField("speedLimit", int64_t{80}));
    requireExpectedOk(roadA->attributes()->addField("optionalNote", nullNode(*tile)));

    auto restrictions = roadA->attributeLayers()->newLayer("restrictions");
    auto turn = restrictions->newAttribute("turn");
    auto turnMeta = tile->newObject(3, true);
    requireExpectedOk(turnMeta->addField("tag", "left"));
    requireExpectedOk(turnMeta->addField("tag", "through"));
    requireExpectedOk(turnMeta->addField("blob", tile->newValue(simfil::ByteArray{"AB"})));
    requireExpectedOk(turn->addField("meta", turnMeta));
    turn->setSourceDataReferences(makeSourceDataRefs(
        *tile,
        {{50, 12, "rules-src", "attribute"}}));

    auto roadB = tile->newFeature("Road", {{"roadId", 9}});
    auto roadBLine = roadB->geom()->newGeometry(GeomType::Line, 2, true);
    roadBLine->append({11.32, 48.015, 0.0});
    roadBLine->append({11.33, 48.02, 0.0});

    turn->validity()->newFeatureTransition(
        roadA,
        Validity::End,
        roadB,
        Validity::Start,
        3,
        Validity::Positive);

    auto externalRoadReference = tile->newFeatureId(
        "Road",
        {
            {"tileId", static_cast<int64_t>(77)},
            {"regionId", "DE.BY%"},
            {"roadId", 11},
        },
        "ValidationMap");
    auto clearance = restrictions->newAttribute("clearance");
    requireExpectedOk(clearance->addField("value", double{3.5}));
    clearance->validity()->newFeatureId(externalRoadReference, Validity::Negative);

    auto relation = tile->newRelation(
        "connectedTo",
        tile->newFeatureId(
            "Road",
            {
                {"tileId", static_cast<int64_t>(77)},
                {"regionId", "DE.BY%"},
                {"roadId", 9},
            },
            "ValidationMap"));
    relation->setSourceDataReferences(makeSourceDataRefs(
        *tile,
        {{70, 8, "rules-src", "relation"}}));
    relation->sourceValidity()->newRange(
        Validity::RelativeLengthOffset,
        0.25,
        0.75,
        2,
        Validity::Positive);
    relation->targetValidity()->newPoint(
        Point{11.32, 48.015, 0.0},
        std::nullopt,
        Validity::Negative);
    roadA->addRelation(relation);

    auto roadC = tile->newFeature("Road", {{"roadId", 11}});
    auto roadCAabb = roadC->geom()->newGeometry(GeomType::AABB, 2, true);
    roadCAabb->setAabb({11.4, 48.1, 1.0}, {0.01, 0.02, 0.5});

    auto originalJson = tile->toJson();
    REQUIRE(originalJson["features"][0]["id"] == "Road.77.DE%2EBY%25.7");
    REQUIRE(originalJson["features"][0]["geometry"]["type"] == "GeometryCollection");
    REQUIRE(originalJson["features"][0]["relations"][0]["target"] == nlohmann::json{
        {"id", "Road.77.DE%2EBY%25.9"},
        {"mapId", "ValidationMap"},
    });
    REQUIRE(
        originalJson["features"][0]["properties"]["layer"]["restrictions"]["clearance"]["validity"]["featureId"] ==
        nlohmann::json{
            {"id", "Road.77.DE%2EBY%25.11"},
            {"mapId", "ValidationMap"},
        });

    auto imported = makeTile(131073, layerInfo, "StrictImportNode", "StrictImportMap");
    REQUIRE_NOTHROW(imported->fromJson(originalJson));
    REQUIRE(imported->toJson() == originalJson);
    REQUIRE(imported->find("Road.77.DE%2EBY%25.7"));
    REQUIRE(imported->find("Road.77.DE%2EBY%25.9"));
}

TEST_CASE("TileFeatureLayer best-effort GeoJSON import shares the same pipeline", "[GeoJsonImport]")
{
    auto layerInfo = makeGenericLayerInfo();
    auto tile = makeTile(131073, layerInfo, "BestEffortNode");

    auto input = R"json(
    {
      "type": "FeatureCollection",
      "features": [
        {
          "type": "Feature",
          "geometry": {
            "type": "MultiLineString",
            "coordinates": [
              [[11.3, 48.0, 0.0], [11.31, 48.01, 1.0]],
              [[11.31, 48.01, 1.0], [11.32, 48.02, 2.0]]
            ]
          },
          "properties": {
            "name": "Main",
            "restrictions": {
              "turn": {
                "value": "left",
                "validity": {
                  "direction": "POSITIVE"
                }
              }
            }
          }
        }
      ]
    })json"_json;

    REQUIRE_NOTHROW(tile->fromJson(
        input,
        GeoJsonImportOptions{
            .strict_ = false,
            .fallbackFeatureType_ = "AnyFeature",
            .objectPropertiesAsAttributeLayers_ = true,
        }));

    auto output = tile->toJson();
    REQUIRE(output["features"].size() == 1);
    REQUIRE(output["features"][0]["id"] == "AnyFeature.131073.0");
    REQUIRE(output["features"][0]["geometry"]["type"] == "GeometryCollection");
    REQUIRE(output["features"][0]["properties"]["name"] == "Main");
    REQUIRE(output["features"][0]["properties"]["layer"]["restrictions"]["turn"]["value"] == "left");
    REQUIRE(
        output["features"][0]["properties"]["layer"]["restrictions"]["turn"]["validity"]["direction"] ==
        "POSITIVE");
}

TEST_CASE("TileFeatureLayer GeoJSON import preserves polygon holes", "[GeoJsonImport][Polygon]")
{
    auto layerInfo = makeGenericLayerInfo();
    auto tile = makeTile(131073, layerInfo, "PolygonHoleNode");

    auto input = R"json(
    {
      "type": "FeatureCollection",
      "geometryAnchor": [0.0, 0.0, 0.0],
      "features": [
        {
          "type": "Feature",
          "id": "AnyFeature.131073.0",
          "typeId": "AnyFeature",
          "tileId": 131073,
          "featureIndex": 0,
          "geometry": {
            "type": "Polygon",
            "coordinates": [
              [[0.0, 0.0, 0.0], [4.0, 0.0, 0.0], [4.0, 4.0, 0.0], [0.0, 4.0, 0.0], [0.0, 0.0, 0.0]],
              [[1.0, 1.0, 0.0], [1.0, 3.0, 0.0], [3.0, 3.0, 0.0], [3.0, 1.0, 0.0], [1.0, 1.0, 0.0]]
            ]
          },
          "properties": {}
        }
      ]
    })json"_json;

    REQUIRE_NOTHROW(tile->fromJson(
        input,
        GeoJsonImportOptions{
            .strict_ = true,
            .fallbackFeatureType_ = "AnyFeature",
            .objectPropertiesAsAttributeLayers_ = true,
        }));

    auto feature = tile->find("AnyFeature.131073.0");
    REQUIRE(feature);
    auto geometry = feature->geomOrNull()->geometryOfTypeAtPreferredStage(GeomType::Polygon, 0);
    REQUIRE(geometry);
    REQUIRE(geometry->numPolygonRings() == 2);
    REQUIRE(geometry->polygonRingStart(0) == 0);
    REQUIRE(geometry->polygonRingStart(1) == 5);

    auto output = tile->toJson();
    auto const& coordinates = output["features"][0]["geometry"]["coordinates"];
    REQUIRE(coordinates.size() == 2);
    REQUIRE(coordinates[0].size() == 5);
    REQUIRE(coordinates[1].size() == 5);
    REQUIRE(coordinates == input["features"][0]["geometry"]["coordinates"]);

    std::stringstream tileBytes;
    tile->write(tileBytes);
    auto const serializedTile = tileBytes.str();
    auto roundtrippedTile = std::make_shared<TileFeatureLayer>(
        std::vector<uint8_t>(serializedTile.begin(), serializedTile.end()),
        [&](auto&& mapId, auto&& layerId) {
            REQUIRE(mapId == "GeoJsonImportMap");
            REQUIRE(layerId == "GeoJsonAny");
            return layerInfo;
        },
        [&](auto&& nodeId) {
            REQUIRE(nodeId == "PolygonHoleNode");
            return tile->strings();
        });

    auto roundtrippedFeature = roundtrippedTile->find("AnyFeature.131073.0");
    REQUIRE(roundtrippedFeature);
    auto roundtrippedGeometry = roundtrippedFeature->geomOrNull()->geometryOfTypeAtPreferredStage(GeomType::Polygon, 0);
    REQUIRE(roundtrippedGeometry);
    REQUIRE(roundtrippedGeometry->numPolygonRings() == 2);
    REQUIRE(roundtrippedGeometry->polygonRingStart(0) == 0);
    REQUIRE(roundtrippedGeometry->polygonRingStart(1) == 5);
    REQUIRE(roundtrippedTile->toJson()["features"][0]["geometry"]["coordinates"] == coordinates);
}

TEST_CASE("Large sanitized GeoJSON fixture roundtrips", "[GeoJsonImport][Fixture]")
{
    auto layerInfoJson = loadJsonFixture("large-geojson-featureset.layer-info.json");
    auto featureCollectionJson = loadJsonFixture("large-geojson-featureset.feature-collection.json");
    auto layerInfo = LayerInfo::fromJson(layerInfoJson);

    REQUIRE(featureCollectionJson.at("mapgetLayerId").get<std::string>() == layerInfo->layerId_);
    REQUIRE(featureCollectionJson.at("features").size() >= 64);

    auto tile = makeTile(
        featureCollectionJson.at("mapgetTileId").get<int32_t>(),
        layerInfo,
        "FixtureImportNode",
        featureCollectionJson.at("mapId").get<std::string>());

    REQUIRE_NOTHROW(tile->fromJson(featureCollectionJson));

    auto roundTrip = tile->toJson();
    std::vector<std::string> errors;
    auto const matches = compareJsonWithTolerance(
        featureCollectionJson,
        roundTrip,
        1e-5,
        &errors);
    INFO(nlohmann::json::diff(featureCollectionJson, roundTrip).dump());
    INFO(formatJsonComparisonErrors(errors));
    REQUIRE(matches);

    REQUIRE(tile->find("Entry.131073.group%2E00%25.0"));
    REQUIRE(tile->find("Entry.131073.group%2E00%25.71"));
}
