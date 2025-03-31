#include <catch2/catch_test_macros.hpp>

#include "mapget/model/point.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/stringpool.h"
#include "simfil/model/nodes.h"
#include "simfil/simfil.h"

using namespace mapget;
using simfil::ValueType;

auto makeLayer()
{
    // Create layer info which has a single feature type with
    // several allowed feature id compositions.
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
        TileId::fromWgs84(1, 2,3),
        "TastyTomatoSaladNode",
        "Tropico",
        layerInfo,
        strings);

    // Set the tile's feature id prefix.
    tile->setIdPrefix({{"areaId", "TheBestArea"}});

    auto feature1 = tile->newFeature("Way", {{"wayId", 42}});

    return tile;
}

#define REQUIRE_QUERY(query, type, result)              \
    do {                                                \
        auto pool = makeLayer();                        \
        auto res = pool->evaluate(query, false, false); \
        REQUIRE(res.size() == 1);                       \
        INFO("simifil res: " << res[0].toString());     \
        REQUIRE(res[0].as<type>() == result);           \
    } while (false)

TEST_CASE("Construct Point", "[simfil.geometry]")
{
    REQUIRE_QUERY("point(1,2) as string", ValueType::String, "[1,2,0]");
}

TEST_CASE("Construct BBox", "[simfil.geometry]")
{
    REQUIRE_QUERY("bbox(1,2,3,4) as string", ValueType::String, "[[1,2,0],[3,4,0]]");
}

TEST_CASE("Construct Linestring", "[simfil.geometry]")
{
    REQUIRE_QUERY("linestring(point(1,2), point(3,4), point(5,6)) as string", ValueType::String, "[[1,2,0],[3,4,0],[5,6,0]]");
}

TEST_CASE("Linestring Intersection", "[simfil.geometry]")
{
    // Crossing lines
    REQUIRE_QUERY("linestring(point(1,-1), point(-1,1)) intersects linestring(point(-1,-1), point(1,1))", ValueType::Bool, true);
    REQUIRE_QUERY("linestring(point(0,-1), point(1,1)) intersects linestring(point(-1,0), point(1,0))", ValueType::Bool, true);

    // Parallel lines
    REQUIRE_QUERY("linestring(point(0,0), point(0,1)) intersects linestring(point(1,0), point(1,1))", ValueType::Bool, false);

    // Intersection outsides the start/end points
    REQUIRE_QUERY("linestring(point(0,0), point(1,1)) intersects linestring(point(2,0), point(2,1))", ValueType::Bool, false);
}
