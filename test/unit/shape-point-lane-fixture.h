#pragma once

#include "mapget/model/featurelayer-filter.h"
#include "mapget/model/mapschema.h"

namespace mapget::test
{

/**
 * A small native producer for the shared wire/pick contract. The centerline
 * and ADAS line have identical vertices, so filtering or picking by position
 * cannot accidentally pass the geometry-identity test. The sparse records on
 * ADAS expose curvature at its second vertex and leave slope absent.
 */
inline PartitionFeatureLayer::Ptr shapePointLane(
    bool repeatedSample = false,
    PointSeries::Markers markers = PointSeries::Markers::None,
    std::string_view firstGeometryName = "centerline")
{
    auto map = std::make_shared<MapSchema>();
    auto layer = map->newLayer("Lane", LayerType::Features);
    (void)layer->newFeature(
        {"Lane", {{{"tileId", {}, IdPartDataType::U32}, {"laneId", {}, IdPartDataType::U32}}}});
    auto integer = map->schemaType(map->primitive(simfil::SchemaType::Kind::Int));
    auto record = map->newObjectType();
    record.addField("point", integer);
    record.addField("code", integer);
    auto records = map->schemaType(map->arrayOf(record.id()));
    auto attribute = map->newAttribute("adasMeasurement");
    attribute.addField("records", records);
    if (markers == PointSeries::Markers::PointValidities) {
        attribute.addField(
            "pointValidities",
            map->schemaType(map->builtin(MapSchema::Builtin::Validities)));
    }
    layer->addPointCodec("curvatureCode", {.tableOnly_ = true, .codes_ = {{7, -0.15296}}});
    for (auto const name : {std::string_view("CURVATURE"), std::string_view("SLOPE_ARRAY")}) {
        layer->bindAttribute(attribute, "Lane", "LaneAdasLayerWithGeo", name);
        bool const curvature = name == "CURVATURE";
        PointSeries series{
            .featureType_ = "Lane",
            .attributeLayer_ = "LaneAdasLayerWithGeo",
            .attribute_ = std::string(name),
            .geometryName_ = "ADAS",
            .records_ = {"records"},
            .position_ = {.kind_ = PointSeries::Position::Kind::VertexIndex, .field_ = {"point"}},
            .fields_ =
                {{curvature ? "curvature" : "slope",
                  {"code"},
                  curvature ? "m^-1" : "%",
                  curvature ? "curvatureCode" : ""}},
            .markers_ = markers,
        };
        layer->addPointSeries(std::move(series));
    }
    map->finalize();

    DataSourceInfo catalog;
    auto info = std::make_shared<LayerInfo>();
    info->layerId_ = "Lane";
    catalog.layers_.emplace("Lane", info);
    catalog.setMapSchema(map);
    auto strings = std::make_shared<StringPool>("shape-point-fixture-pool");
    auto source = std::make_shared<PartitionFeatureLayer>(
        TileId::fromValue(545555028),
        strings->stringPoolId_,
        "PointFixture",
        catalog.getLayer("Lane"),
        strings);
    source->setGeometryAnchor({11.5, 48., 0.});
    auto lane =
        source->newFeature("Lane", {{"tileId", int64_t{545555028}}, {"laneId", int64_t{2}}});
    for (auto name : {firstGeometryName, std::string_view("ADAS")}) {
        auto line = lane->geom()->newGeometry(GeomType::Line, 2, true);
        line->setName(name);
        line->append({11.56, 48.16, 0.});
        line->append({11.5666279848665, 48.16778398118913, 0.});
    }
    auto curvature =
        lane->attributeLayers()->newLayer("LaneAdasLayerWithGeo")->newAttribute("CURVATURE");
    curvature->validity()->newGeomName("ADAS");
    auto row = source->newObject();
    row->addField("point", source->newValue(int64_t{1}));
    row->addField("code", source->newValue(int64_t{7}));
    auto samples = source->newArray(repeatedSample ? 2 : 1, true);
    samples->append(row);
    if (repeatedSample)
        samples->append(row);
    curvature->addField("records", samples);
    // A feature-restricted inspection response contains only the selected host.
    // Keep a second feature in the complete partition to exercise this distinction.
    (void)source->newFeature("Lane", {{"tileId", int64_t{545555028}}, {"laneId", int64_t{3}}});
    source->setTimestamp({});
    source->publishSourceSnapshot("shape-point-test-source");
    return source;
}

/** The request shared by the native producer and the matching WASM consumer. */
inline FeatureLayerFilterRequest shapePointLaneRequest()
{
    return {
        .filterId_ = "shape-point-fixture",
        .generation_ = 1,
        .channels_ = {{
            .channelId_ = "style-rule:0",
            .featureFilter_ = "typeId == 'Lane'",
            .entryFilter_ = "$pointIndex == 1",
            .scope_ = FeatureLayerFilterScope::ShapePoint,
            .featureTypes_ = {"Lane"},
            .featureFields_ = {"laneId"},
            .entryFields_ =
                {"$attributes.LaneAdasLayerWithGeo.CURVATURE.*.curvature",
                 "$attributes.LaneAdasLayerWithGeo.SLOPE_ARRAY.*.slope"},
            .geometryTypes_ = 1U << static_cast<unsigned>(GeomType::Line),
            .geometryName_ = "ADAS",
        }}};
}
}  // namespace mapget::test
