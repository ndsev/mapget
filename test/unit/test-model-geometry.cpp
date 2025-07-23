#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "mapget/model/featurelayer.h"
#include "mapget/model/stream.h"
#include "nlohmann/json.hpp"
#include "mapget/log.h"

#include "mapget/model/simfil-geometry.h"
#include "simfil/simfil.h"

using namespace mapget;
using simfil::Environment;
using simfil::ValueType;
using simfil::ScalarValueType;

template<class T>
ModelNode const& asModelNode(model_ptr<T> const& modelNodeDerived)
{
    return static_cast<ModelNode const&>(*modelNodeDerived);
}

auto makeTile() {
    auto layerInfo = LayerInfo::fromJson(R"({
        "layerId": "WayLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Way",
                "uniqueIdCompositions": [
                    [
                        {
                            "partId": "areaId",
                            "description": "String which identifies the map area.",
                            "datatype": "STR"
                        },
                        {
                            "partId": "wayId",
                            "description": "Globally Unique 32b integer.",
                            "datatype": "U32"
                        }
                    ],
                    [
                        {
                            "partId": "wayIdU32",
                            "description": "A 32b uinteger.",
                            "datatype": "U32"
                        },
                        {
                            "partId": "wayIdU64",
                            "description": "A 64b uinteger.",
                            "datatype": "U64"
                        },
                        {
                            "partId": "wayIdUUID128",
                            "description": "A UUID128, must have 16 bytes.",
                            "datatype": "UUID128"
                        }
                    ],
                    [
                        {
                            "partId": "wayIdI32",
                            "description": "A 32b integer.",
                            "datatype": "I32"
                        },
                        {
                            "partId": "wayIdI64",
                            "description": "A 64b integer.",
                            "datatype": "I64"
                        },
                        {
                            "partId": "wayIdUUID128",
                            "description": "A UUID128, must have 16 bytes.",
                            "datatype": "UUID128"
                        }
                    ]
                ]
            }
        ]
    })"_json);

    // Create empty shared autofilled field-name dictionary
    auto strings = std::make_shared<StringPool>("TastyTomatoSaladNode");

    // Create a basic TileFeatureLayer
    auto tile = std::make_shared<TileFeatureLayer>(
        TileId::fromWgs84(42., 11., 13),
        "TastyTomatoSaladNode",
        "Tropico",
        layerInfo,
        strings);

    // Set the tile's feature id prefix.
    tile->setIdPrefix({{"areaId", "TheBestArea"}});

    return tile;
}

#define REQUIRE_EVAL_1(query, type, result)                         \
    do {                                                            \
        Environment env(model_pool->strings());                     \
        env.functions["geo"] = &GeoFn::Fn;                          \
        env.functions["bbox"] = &BBoxFn::Fn;                        \
        env.functions["linestring"] = &LineStringFn::Fn;            \
        auto ast = compile(env, query, false);                      \
        if (!ast)                                                   \
            INFO(ast.error().message);                              \
        REQUIRE(ast.has_value());                                   \
        INFO("AST: " << ast.value()->expr().toString());             \
        auto res = eval(env, **ast, *model_pool->root(0), nullptr); \
        if (!res)                                                   \
            INFO(res.error().message);                              \
        REQUIRE(res.has_value());                                   \
        auto val = std::move(*res);                                 \
        REQUIRE(val.size() == 1);                                   \
        REQUIRE(val[0].as<type>() == result);                       \
    } while (false)

TEST_CASE("Point", "[geo.point]") {
    REQUIRE(Point{0, 1} == Point{0, 1});
}

TEST_CASE("BBox", "[geo.bbox]") {
    REQUIRE(BBox{{0, 1}, {2, 3}} == BBox{{0, 1}, {2, 3}});
}

TEST_CASE("LineString", "[geo.linestring]") {
    SECTION("Two crossing lines") {
        auto l1 = LineString{{Point{-1, -1}, Point{ 1, 1}}};
        auto l2 = LineString{{Point{ 1, -1}, Point{-1, 1}}};
        REQUIRE(l1.intersects(l2));
    }
    SECTION("Two crossing lines vert/horz") {
        auto l1 = LineString{{Point{0, -1}, Point{1, 1}}};
        auto l2 = LineString{{Point{-1, 0}, Point{1, 0}}};
        REQUIRE(l1.intersects(l2));
    }
}

TEST_CASE("Polygon", "[geo.polygon]") {
    SECTION("Point in rectangle polygon") {
        auto p = Polygon{{LineString{{Point{0, 0}, Point{1, 0}, Point{1, 1}, Point{0, 1}}}}};
        REQUIRE(p.contains(Point{0.5, 0.5}));   /* center */
        REQUIRE(p.contains(Point{0, 0}));       /* top-left */
        REQUIRE(!p.contains(Point{-0.5, 0.5})); /* left */
        REQUIRE(!p.contains(Point{ 1.5, 0.5})); /* right */
    }
    SECTION("Point in triangle polygon") {
        auto p = Polygon{{LineString{{Point{0, 0}, Point{1, 1}, Point{0, 1}}}}};
        REQUIRE(p.contains(Point{0.4999, 0.5}));  /* on edge */
        REQUIRE(p.contains(Point{0, 0}));         /* top-left */
        REQUIRE(!p.contains(Point{0.5001, 0.5})); /* left to edge */
    }
}

TEST_CASE("GeometryCollection", "[geom.collection]")
{
    auto model_pool = makeTile();
    auto geometry_collection = model_pool->newGeometryCollection();
    auto point_geom = geometry_collection->newGeometry(GeomType::Points);
    point_geom->append({.0, .0, .0});
    point_geom->append({.25, .25, .25});
    point_geom->append({.5, .5, .5});
    point_geom->append({1., 1., 1.});

    SECTION("Construct GeometryCollection")
    {
        REQUIRE(asModelNode(geometry_collection).type() == ValueType::Object);
        REQUIRE(asModelNode(geometry_collection).size() == 3); // 'type' and 'geometries' fields
    }

    SECTION("Recover geometry")
    {
        REQUIRE(asModelNode(point_geom).type() == ValueType::Object);
        REQUIRE(point_geom->geomType() == GeomType::Points);
        REQUIRE(point_geom->numPoints() == 4);
        REQUIRE(point_geom->pointAt(0).x == .0);
        REQUIRE(point_geom->pointAt(1).x == .25);
        REQUIRE(point_geom->pointAt(2).x == .5);
        REQUIRE(point_geom->pointAt(3).x == 1.);
        REQUIRE(geometry_collection->numGeometries() == 1);
    }

    SECTION("GeoJSON representation") {
        // Since the collection only contains one geometry,
        // it hides itself and directly presents the nested geometry,
        // conforming to GeoJSON (a collection must have >1 geometries).
        REQUIRE(asModelNode(geometry_collection).size() == 3); // 'type' and 'geometry' fields
        REQUIRE(asModelNode(geometry_collection).at(1)->type() == ValueType::Array); // 'geometry' field
        REQUIRE(asModelNode(geometry_collection).at(1)->size() == 4); // four points

        // Add nested geometry again two more times, now the view changes.
        geometry_collection->addGeometry(point_geom);
        geometry_collection->addGeometry(point_geom);

        REQUIRE(geometry_collection->numGeometries() == 3);
        REQUIRE(asModelNode(geometry_collection).at(1)->size() == 3); // Three geometries
    }

    SECTION("Geometry View") {
        auto view = model_pool->newGeometryView(GeomType::Line, 1, 2, point_geom);
        REQUIRE(view->pointAt(0).x == .25);
        REQUIRE(view->pointAt(1).x == .5);
        REQUIRE_THROWS(view->pointAt(2));

        auto subview = model_pool->newGeometryView(GeomType::Points, 1, 1, view);
        REQUIRE(subview->pointAt(0).x == .5);
        REQUIRE_THROWS(subview->pointAt(1));
    }
}

TEST_CASE("Spatial Operators", "[spatial.ops]") {
    auto model_pool = makeTile();

    // Create a GeometryCollection with a Point
    auto geometry_collection = model_pool->newGeometryCollection();
    auto point_geom = geometry_collection->newGeometry(GeomType::Points);
    point_geom->append({2., 3., 0.});
    model_pool->addRoot(model_ptr<ModelNode>(model_pool->newObject()->addField(
        "geometry",
        point_geom)));

    Environment env(model_pool->strings());

    SECTION("Point Within BBox") {
        REQUIRE_EVAL_1("geo() within bbox(1, 2, 4, 5)", ValueType::Bool, true);
    }

    SECTION("Point Intersects BBox") {
        REQUIRE_EVAL_1("geo() intersects bbox(1, 2, 4, 5)", ValueType::Bool, true);
    }

    SECTION("BBox Contains Point") {
        REQUIRE_EVAL_1("bbox(1, 2, 4, 5) contains geo()", ValueType::Bool, true);
    }
}

TEST_CASE("GeometryCollection Multiple Geometries", "[geom.collection.multiple]") {
    auto model_pool = makeTile();

    // Points
    Point a{2, 3}, b{1, 1}, c{4, 4}, d{0, 0}, e{5, 0}, f{2.5, 5};

    // Create a GeometryCollection
    auto geometry_collection = model_pool->newGeometryCollection();

    // Create and add Point geometry
    auto point_geom = geometry_collection->newGeometry(GeomType::Points);
    point_geom->append(a);

    // Create and add LineString geometry
    auto linestring_geom = geometry_collection->newGeometry(GeomType::Line);
    linestring_geom->append(b);
    linestring_geom->append(c);

    // Create and add Polygon geometry
    auto polygon_geom = geometry_collection->newGeometry(GeomType::Polygon);
    polygon_geom->append(d);
    polygon_geom->append(e);
    polygon_geom->append(f);

    model_pool->addRoot(model_ptr<ModelNode>(model_pool->newObject()->addField(
        "geometry",
        ModelNode::Ptr(geometry_collection))));

    SECTION("Retrieve points") {
        // Check stored points in Point geometry
        REQUIRE(asModelNode(point_geom).get(StringPool::CoordinatesStr)->size() == 1);
        REQUIRE(asModelNode(point_geom).get(StringPool::CoordinatesStr)->at(0)->type() == ValueType::Array); // Point
        REQUIRE(asModelNode(point_geom).get(StringPool::CoordinatesStr)->at(0)->get(StringPool::LonStr)->value() == ScalarValueType(a.x));
        REQUIRE(asModelNode(point_geom).get(StringPool::CoordinatesStr)->at(0)->get(StringPool::LatStr)->value() == ScalarValueType(a.y));

        // Check stored points in LineString geometry
        REQUIRE(asModelNode(linestring_geom).get(StringPool::CoordinatesStr)->size() == 2);
        REQUIRE(asModelNode(linestring_geom).get(StringPool::CoordinatesStr)->at(0)->get(StringPool::LonStr)->value() == ScalarValueType(b.x));
        REQUIRE(asModelNode(linestring_geom).get(StringPool::CoordinatesStr)->at(0)->get(StringPool::LatStr)->value() == ScalarValueType(b.y));
        REQUIRE(asModelNode(linestring_geom).get(StringPool::CoordinatesStr)->at(1)->get(StringPool::LonStr)->value() == ScalarValueType(c.x));
        REQUIRE(asModelNode(linestring_geom).get(StringPool::CoordinatesStr)->at(1)->get(StringPool::LatStr)->value() == ScalarValueType(c.y));

        // Check stored points in Polygon geometry
        REQUIRE(asModelNode(polygon_geom).get(StringPool::CoordinatesStr)->size() == 1);
        REQUIRE(asModelNode(polygon_geom).get(StringPool::CoordinatesStr)->at(0)->size() == 4);
        REQUIRE(asModelNode(polygon_geom).get(StringPool::CoordinatesStr)->at(0)->at(0)->get(StringPool::LonStr)->value() == ScalarValueType(d.x));
        REQUIRE(asModelNode(polygon_geom).get(StringPool::CoordinatesStr)->at(0)->at(0)->get(StringPool::LatStr)->value() == ScalarValueType(d.y));
        REQUIRE(asModelNode(polygon_geom).get(StringPool::CoordinatesStr)->at(0)->at(1)->get(StringPool::LonStr)->value() == ScalarValueType(e.x));
        REQUIRE(asModelNode(polygon_geom).get(StringPool::CoordinatesStr)->at(0)->at(1)->get(StringPool::LatStr)->value() == ScalarValueType(e.y));
        REQUIRE(asModelNode(polygon_geom).get(StringPool::CoordinatesStr)->at(0)->at(2)->get(StringPool::LonStr)->value() == ScalarValueType(f.x));
        REQUIRE(asModelNode(polygon_geom).get(StringPool::CoordinatesStr)->at(0)->at(2)->get(StringPool::LatStr)->value() == ScalarValueType(f.y));
        REQUIRE(asModelNode(polygon_geom).get(StringPool::CoordinatesStr)->at(0)->at(3)->get(StringPool::LonStr)->value() == ScalarValueType(d.x));
        REQUIRE(asModelNode(polygon_geom).get(StringPool::CoordinatesStr)->at(0)->at(3)->get(StringPool::LatStr)->value() == ScalarValueType(d.y));
    }

    SECTION("For-each") {
        auto pts = std::vector<Point>{a, b, c, d, e, f};
        auto numPts = 0;
        auto numGeoms = 0;
        geometry_collection->forEachGeometry(
            [&numPts, &numGeoms, &pts](const auto& g)
            {
                g->forEachPoint(
                    [&numPts, &pts](const auto& p)
                    {
                        REQUIRE(p == pts[numPts]);
                        ++numPts;
                        return true;
                    });
                ++numGeoms;
                return true;
            });
        REQUIRE(numGeoms == 3);
        REQUIRE(numPts == 6);
    }

    SECTION("type") {
        REQUIRE_EVAL_1("geometry.type == 'GeometryCollection'", ValueType::Bool, true);
    }

    SECTION("geo-fn") {
        // Pass sub-geometries unwrapped
        REQUIRE_EVAL_1("count(geo(geometry.geometries.*))", ValueType::Int, 3);

        // Implicit resolve "geometry" field
        REQUIRE_EVAL_1("count(geo())", ValueType::Int, 3);

        // Explicit pass geometry object
        REQUIRE_EVAL_1("count(geo(geometry))", ValueType::Int, 3);
    }
}

TEST_CASE("Attribute Validity", "[validity]") {
    auto modelPool = makeTile();

    // Create a GeometryCollection.
    auto geometryCollection = modelPool->newGeometryCollection();

    // Create and add LineString geometry without name.
    auto linestringGeom = geometryCollection->newGeometry(GeomType::Line);
    linestringGeom->append({0., 0.});
    linestringGeom->append({.5, .5});
    linestringGeom->append({1., 1.});

    // Create and add LineString geometry with name.
    auto linestringGeomNamed = geometryCollection->newGeometry(GeomType::Line);
    linestringGeomNamed->append({-0., -0.});
    linestringGeomNamed->append({-.5, -.5});
    linestringGeomNamed->append({-1., -1.});
    linestringGeomNamed->setName("BestGeometry");

    // Create a validity collection.
    auto metresAtFortyPercent = Point({-0., -0.}).geographicDistanceTo(Point({-1., -1.})) * 0.4;
    auto metresAtEightyPercent = Point({-0., -0.}).geographicDistanceTo(Point({-1., -1.})) * 0.8;
    auto validities = modelPool->newValidityCollection();
    validities->newDirection(Validity::Direction::Positive);
    validities->newGeometry(linestringGeom);
    validities->newPoint({.2, .25});
    validities->newRange({.2, .25}, {.75, .7});
    validities->newPoint(Validity::BufferOffset, 0);
    validities->newPoint(Validity::RelativeLengthOffset, .4);
    validities->newPoint(Validity::MetricLengthOffset, metresAtFortyPercent);
    validities->newRange(Validity::BufferOffset, 0, 1);
    validities->newRange(Validity::RelativeLengthOffset, .4, .8);
    validities
        ->newRange(Validity::MetricLengthOffset, metresAtFortyPercent, metresAtEightyPercent);
    validities->newGeometry(linestringGeomNamed);
    validities->newPoint({-.2, -.25}, "BestGeometry");
    validities->newRange({-.2, -.25}, {-.75, -.7}, "BestGeometry");
    validities->newPoint(Validity::BufferOffset, 0, "BestGeometry");
    validities->newPoint(Validity::RelativeLengthOffset, .4, "BestGeometry");
    validities->newPoint(Validity::MetricLengthOffset, metresAtFortyPercent, "BestGeometry");
    validities->newRange(Validity::BufferOffset, 0, 1, "BestGeometry");
    validities->newRange(Validity::RelativeLengthOffset, .4, .8, "BestGeometry");
    validities->newRange(
        Validity::MetricLengthOffset,
        metresAtFortyPercent,
        metresAtEightyPercent,
        "BestGeometry");
    auto json = validities->toJson();
    REQUIRE(json.size() == 19);

    // Fill out the expectedGeometry vector.
    std::vector<std::vector<Point>> expectedGeometry = {
        // Validity::Direction::Positive 💚
        {{0.0,0.0,0.0}, {0.5,0.5,0.0}, {1.0,1.0,0.0}},
        // linestringGeom 💚
        {{0.0,0.0,0.0}, {0.5,0.5,0.0}, {1.0,1.0,0.0}},
        // {.2, .25} 💚
        {{0.225,0.225,0.0}},
        // {.2, .25}, {.75, .7} 💚
        {{0.225,0.225,0.0}, {0.5,0.5,0.0}, {0.725,0.725,0.0}},
        // Validity::BufferOffset, 0 💚
        {{0.0,0.0,0.0}},
        // Validity::RelativeLengthOffset, .4 💚
        {{0.39999238466117465,0.39999238466117465,0.0}},
        // Validity::MetricLengthOffset, metresAtFortyPercent 💚
        {{0.39999238400870357,0.39999238400870357,0.0}},
        // Validity::BufferOffset, 0, 1 💚
        {{0.0,0.0,0.0}, {0.5,0.5,0.0}},
        // Validity::RelativeLengthOffset, .4, .8 💚
        {{0.39999238466117465,0.39999238466117465,0.0}, {0.5,0.5,0.0}, {0.7999961921855985,0.7999961921855985,0.0}},
        // Validity::MetricLengthOffset, metresAtFortyPercent, metresAtEightyPercent 💚
        {{0.39999238400870357,0.39999238400870357,0.0}, {0.5,0.5,0.0}, {0.7999961908806066,0.7999961908806066,0.0}},
        // linestringGeomNamed 💚
        {{-0.0,-0.0,0.0}, {-0.5,-0.5,0.0}, {-1.0,-1.0,0.0}},
        // {-.2, -.25}, "BestGeometry" 💚
        {{-0.225,-0.225,0.0}},
        // {-.2, -.25}, {-.75, -.7}, "BestGeometry" 💚
        {{-0.225,-0.225,0.0}, {-0.5,-0.5,0.0}, {-0.725,-0.725,0.0}},
        // Validity::BufferOffset, 0, "BestGeometry" 💚
        {{-0.0,-0.0,0.0}},
        // Validity::RelativeLengthOffset, .4, "BestGeometry" 💚
        {{-0.39999238466117465,-0.39999238466117465,0.0}},
        // Validity::MetricLengthOffset, metresAtFortyPercent, "BestGeometry" 💚
        {{-0.39999238400870357,-0.39999238400870357,0.0}},
        // Validity::BufferOffset, 0, 1, "BestGeometry" 💚
        {{-0.0,-0.0,0.0}, {-0.5,-0.5,0.0}},
        // Validity::RelativeLengthOffset, .4, .8, "BestGeometry" 💚
        {{-0.39999238466117465,-0.39999238466117465,0.0}, {-0.5,-0.5,0.0}, {-0.7999961921855985,-0.7999961921855985,0.0}},
        // Validity::MetricLengthOffset, metresAtFortyPercent, metresAtEightyPercent, "BestGeometry" 💚
        {{-0.39999238400870357,-0.39999238400870357,0.0}, {-0.5,-0.5,0.0}, {-0.7999961908806066,-0.7999961908806066,0.0}},
    };

    // Compare expected validity geometries against computed ones.
    auto validityIndex = 0;
    validities->forEach([&validityIndex, &geometryCollection, &expectedGeometry](auto&& validity) {
        DYNAMIC_SECTION(fmt::format("Validity Index #{}", validityIndex))
        {
            auto wgsPoints = validity.computeGeometry(geometryCollection);
            log().info("Points #{}: {}", validityIndex, nlohmann::json(wgsPoints.points_).dump());
            auto const& expectedWgsPoints = expectedGeometry[validityIndex];
            REQUIRE(wgsPoints.points_.size() == expectedWgsPoints.size());
            for (auto pointIndex = 0; pointIndex < wgsPoints.points_.size(); ++pointIndex) {
                auto const& computedPoint = wgsPoints.points_[pointIndex];
                auto const& expectedPoint = expectedWgsPoints[pointIndex];
                using namespace Catch::Matchers;
                REQUIRE_THAT(computedPoint.x, WithinRel(expectedPoint.x));
                REQUIRE_THAT(computedPoint.y, WithinRel(expectedPoint.y));
                REQUIRE_THAT(computedPoint.z, WithinRel(expectedPoint.z));
            }
        }
        ++validityIndex;
        return true;
    });
}
