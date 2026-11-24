#include <catch2/catch_test_macros.hpp>
#include "mapget/model/coverage.h"
#include <set>

using namespace mapget;

namespace
{
/** Build one complete geographic layer without schema-dependent test setup. */
LayerInfo geographic(std::vector<SpatialExtent> extents)
{
    LayerInfo layer;
    layer.layerId_ = "Coverage";
    layer.spatialExtent_ = std::move(extents);
    layer.coverageComplete_ = true;
    return layer;
}

/** Collect the bounded fixture's compact rectangles into an exact packed tile set. */
std::set<int32_t> cells(std::vector<CoverageTileRange> const& ranges)
{
    std::set<int32_t> result;
    for (auto r : ranges)
        for (auto y = r.minY_; y <= r.maxY_; ++y)
            for (auto x = r.minX_; x <= r.maxX_; ++x)
                REQUIRE(result.insert(CoverageTileRange{x, y, x, y, r.level_}.tile().value()).second);
    return result;
}
}

TEST_CASE("Coverage distinguishes unknown, complete empty, and service metadata", "[Coverage][DataSourceInfo]")
{
    auto tile = TileId::fromWgs84(15, 45, 13);
    LayerInfo layer;
    layer.layerId_ = "Coverage";
    CHECK(CoverageQuery(layer).permits(tile));
    layer.coverageComplete_ = true;
    CHECK_FALSE(CoverageQuery(layer).permits(tile));
    layer.serviceMetadata_ = true;
    CHECK(CoverageQuery(layer).permits(tile));
    auto parsed = LayerInfo::fromJson(layer.toJson());
    REQUIRE(parsed->coverageComplete_ == true);
    CHECK(parsed->serviceMetadata_);
}

TEST_CASE("Coverage availability distinguishes empty regions from usable partial geometry", "[Coverage]")
{
    auto a = TileId::fromTileXY(1, 1, 3), b = TileId::fromTileXY(2, 2, 3);
    LayerInfo mask;
    mask.coverageComplete_ = true;
    mask.coverage_ = {{a, b, {false, false, false, false}}};
    CoverageQuery emptyMask(mask);
    CHECK(emptyMask.complete());
    CHECK(emptyMask.regionCount() == 1);
    CHECK_FALSE(emptyMask.hasBounds());

    auto invalid = geographic({{{0, 0, 1, 1}, std::vector<std::array<double, 2>>{{0, 0}, {1, 1}}}});
    CoverageQuery invalidQuery(invalid);
    CHECK_FALSE(invalidQuery.valid());
    CHECK(invalidQuery.regionCount() == 1);
    CHECK_FALSE(invalidQuery.hasBounds());
    invalid.spatialExtent_->push_back({{170, -90, -170, 90}});
    CoverageQuery partial(invalid);
    CHECK_FALSE(partial.complete());
    CHECK(partial.hasBounds());
    CHECK(partial.regionForTile(TileId::fromWgs84(179, 45, 13)) == 1);

    auto unknown = geographic({});
    unknown.coverageComplete_.reset();
    auto metadata = geographic({{{0, 0, 1, 1}}});
    metadata.serviceMetadata_ = true;
    for (auto const& layer : std::vector<LayerInfo>{unknown, geographic({}), mask, invalid, metadata,
            geographic({{{0, 0, 0, 0}}}), geographic({{{180, 90, 180, 90}}}),
            geographic({{{-180, -90, -180, -90}}}), geographic({{{-180, -90, 180, 90}}})}) {
        CoverageQuery query(layer);
        for (int level = 0; level <= 15; ++level) {
            CAPTURE(level);
            CHECK(query.hasBounds() == !query.bounds({}, level).empty());
        }
    }
}

TEST_CASE("Full-resolution polygon validity does not depend on geographic translation", "[Coverage]")
{
    double const step = 180. / (uint64_t{1} << 31);
    for (auto [x,y] : {std::pair{22.5,45.}, std::pair{-100.,-70.}, std::pair{179.,80.}}) {
        SpatialExtent tiny{{x,y,x+step,y+step},
            std::vector<std::array<double,2>>{{x,y},{x+step,y},{x+step,y+step},{x,y+step}}};
        CoverageQuery query(geographic({tiny}));
        REQUIRE(query.complete());
        CHECK(query.permits(TileId::fromWgs84(x,y,15)));
        CHECK_FALSE(query.permits(TileId::fromWgs84(0,-40,15)));
        SpatialExtent broad{{x-0.5,y-0.5,x+0.5,y+0.5},
            std::vector<std::array<double,2>>{{x-0.5,y-0.5},{x+0.5,y-0.5},{x+0.5,y+0.5},{x-0.5,y+0.5}}, {*tiny.exterior_}};
        CHECK(CoverageQuery(geographic({broad})).complete());
    }
}

TEST_CASE("Polygon overlap is queryable without forbidding generic coverage unions", "[Coverage]")
{
    auto rectangle=[](double west,double south,double east,double north) {
        return SpatialExtent{{west,south,east,north}, std::vector<std::array<double,2>>{
            {west,south},{east,south},{east,north},{west,north}}};
    };
    auto a=rectangle(10,40,12,42);
    for(auto b : {rectangle(11,41,13,43),rectangle(12,40,14,42),rectangle(12,42,14,44),rectangle(10.5,40.5,11.5,41.5),a}) {
        CoverageQuery query(geographic({a,b}));
        CHECK(query.complete());
        CHECK(query.overlappingPolygons() == std::pair<size_t,size_t>{0,1});
    }
    CHECK_FALSE(CoverageQuery(geographic({a,rectangle(13,40,14,42)})).overlappingPolygons());
    auto large=rectangle(0,0,20,60);
    large.holes_.push_back(*rectangle(5,35,15,45).exterior_);
    CHECK_FALSE(CoverageQuery(geographic({large,a})).overlappingPolygons());
    CHECK(CoverageQuery(geographic({large,rectangle(5,40,12,42)})).overlappingPolygons());
    a.bounds_={0,0,30,60};
    auto b=rectangle(13,40,14,42); b.bounds_=a.bounds_;
    CHECK_FALSE(CoverageQuery(geographic({a,b})).overlappingPolygons());
    auto dateline=rectangle(170,-20,-170,20);
    CHECK(CoverageQuery(geographic({dateline,rectangle(-175,0,-160,10)})).overlappingPolygons());
    CHECK(CoverageQuery(geographic({rectangle(170,0,180,10),rectangle(-180,0,-170,10)})).overlappingPolygons());
}

TEST_CASE("Coverage preserves every extent and authoritative holes", "[Coverage]")
{
    SpatialExtent polygon{{-30, -30, 30, 30},
        std::vector<std::array<double, 2>>{{-30,-30},{30,-30},{30,30},{-30,30}},
        {{{-10,-10},{-10,10},{10,10},{10,-10}}}};
    auto layer = geographic({polygon, {{100, 20, 105, 25}}});
    // Legacy approximation must not refill a polygon's hole.
    auto coarse = TileId::fromWgs84(1, 1, 0);
    layer.coverage_.push_back({coarse, coarse, {}});
    CoverageQuery query(layer);
    REQUIRE(query.complete());
    CHECK_FALSE(query.permits(TileId::fromWgs84(1, 1, 10)));
    CHECK(query.permits(TileId::fromWgs84(20, 20, 10)));
    CHECK(query.permits(TileId::fromWgs84(102, 22, 10)));
    CHECK(query.regionForTile(TileId::fromWgs84(102, 22, 10)) == 1);
    CHECK_FALSE(query.permits(TileId::fromWgs84(50, 40, 10)));
    auto parsed = LayerInfo::fromJson(layer.toJson());
    CHECK(parsed->toJson() == layer.toJson());
    auto ranges = query.rectangles({-180,-90,180,90}, 4);
    auto actual = cells(ranges);
    for (auto all : CoverageQuery::viewportRanges({-180,-90,180,90}, 4))
        for (auto y = all.minY_; y <= all.maxY_; ++y)
            for (auto x = all.minX_; x <= all.maxX_; ++x) {
                auto tile = CoverageTileRange{x,y,x,y,4}.tile();
                CHECK(actual.contains(tile.value()) == query.permits(tile));
            }
}

TEST_CASE("Geographic boundaries include touching tiles and hole edges", "[Coverage]")
{
    auto layer = geographic({{{0,0,45,45},
        std::vector<std::array<double,2>>{{0,0},{45,0},{45,45},{0,45}},
        {{{11.25,11.25},{33.75,11.25},{33.75,33.75},{11.25,33.75}}}}});
    CoverageQuery query(layer);
    REQUIRE(query.complete());
    CHECK(query.permits(TileId::fromWgs84(-1, -1, 4))); // exterior vertex
    CHECK(query.permits(TileId::fromWgs84(12, 12, 4))); // hole boundary
    CHECK_FALSE(query.permits(TileId::fromWgs84(20,20,8))); // open hole interior
    CHECK(query.permits(TileId::fromWgs84(1,1,0))); // polygon contained in tile
}

TEST_CASE("Grid coverage projects all levels without filling adjacent false bits", "[Coverage]")
{
    LayerInfo layer;
    layer.coverageComplete_ = true;
    auto a = TileId::fromTileXY(1,1,3), b = TileId::fromTileXY(2,2,3);
    layer.coverage_ = {{a,b,{false,true,true,false}}};
    CoverageQuery query(layer);
    CHECK_FALSE(query.permits(a));
    CHECK(query.permits(TileId::fromTileXY(2,1,3)));
    auto p = TileId::fromTileXY(2,1,3).centerWgs84();
    CHECK(query.permits(TileId::fromWgs84(p.first,p.second,13)));
    CHECK(query.permits(TileId::fromWgs84(p.first,p.second,1)));
    layer.coverage_[0].filled_ = {false};
    CHECK_FALSE(CoverageQuery(layer).complete());
    CHECK(CoverageQuery(layer).permits(a));
}

TEST_CASE("Coverage handles datelines, packed seams, poles and signed IDs", "[Coverage]")
{
    auto layer = geographic({{{170,-89,-170,89}}, {{-2,-2,2,2}}});
    CoverageQuery query(layer);
    CHECK(query.permits(TileId::fromWgs84(179,80,15)));
    CHECK(query.permits(TileId::fromWgs84(-179,-80,15)));
    CHECK(query.permits(TileId::fromWgs84(-1,-1,15)));
    CHECK_FALSE(query.permits(TileId::fromWgs84(90,0,15)));
    CHECK(cells(query.rectangles({160,-85,200,85},5)).size() > 0);
    auto world = geographic({{{-180,-90,180,90}}});
    CHECK(CoverageQuery(world).rectangles({-180,-90,180,90},15).size() == 1);
    CHECK(cells(CoverageQuery(world).rectangles({-180,-90,180,90},0)).size() == 2);
}

TEST_CASE("Malformed optional shapes fail open without losing the layer", "[Coverage][DataSourceInfo]")
{
    auto layer = geographic({{{0,0,10,10}, std::vector<std::array<double,2>>{{0,0},{10,10},{0,10},{10,0}}}});
    CHECK_FALSE(CoverageQuery(layer).complete());
    CHECK(CoverageQuery(layer).permits(TileId::fromWgs84(-50,20,13)));
    auto json = layer.toJson();
    json["spatialExtent"] = "invalid";
    auto parsed = LayerInfo::fromJson(json);
    CHECK(parsed->coverageComplete_ == false);
    auto policy = SpatialExtentPolicy::fromJson({{"defaultLimit",true},{"maps",{{"map/exception",false}}}});
    CHECK(policy.limits("map/default"));
    CHECK_FALSE(policy.limits("map/exception"));
    CHECK(SpatialExtentPolicy::fromJson(policy.toJson()) == policy);
}

TEST_CASE("Malformed packed coverage cannot become authoritative through numeric narrowing", "[Coverage][DataSourceInfo]") {
    using Json=nlohmann::json;
    LayerInfo source;source.layerId_="Grid";source.coverageComplete_=true;
    for(auto invalid : std::vector<Json>{uint64_t{4295032832},int64_t{-4294901760},65536.5,65536.0,"65536",true}) {
        for(auto entry : {invalid,Json{{"min",invalid},{"max",65536}},Json{{"min",65536},{"max",invalid}}}) {
            auto json=source.toJson();json["coverage"]=Json::array({entry});
            auto layer=LayerInfo::fromJson(json);
            CHECK_FALSE(CoverageQuery(*layer).complete());
            CHECK(CoverageQuery(*layer).permits(TileId::fromWgs84(0,0,13)));
        }
    }
    auto signedTile=TileId::fromWgs84(20,30,15);REQUIRE(signedTile.value()<0);
    auto json=source.toJson();json["coverage"]=Json::array({signedTile.value()});
    auto valid=LayerInfo::fromJson(json);CHECK(CoverageQuery(*valid).complete());CHECK(CoverageQuery(*valid).permits(signedTile));
}

TEST_CASE("Malformed extent array arity fails open instead of truncating coordinates", "[Coverage][DataSourceInfo]") {
    using Json=nlohmann::json;
    for(auto extent : std::vector<Json>{
        {{"bounds",{0,0,1,1,999}}},
        {{"bounds",{0,0,1,1}},{"exterior",{{0,0,42},{1,0},{1,1},{0,1}}}},
        {{"bounds",{0,0,1,1}},{"exterior",{{0,0},{1,0},{1,1},{0,1}}},{"holes",{{{.2,.2,42},{.3,.2},{.3,.3},{.2,.3}}}}}
    }) {
        LayerInfo source;source.layerId_="MalformedExtent";source.coverageComplete_=true;
        auto json=source.toJson();json["spatialExtent"]=Json::array({extent});
        auto layer=LayerInfo::fromJson(json);CHECK_FALSE(CoverageQuery(*layer).complete());
        CHECK(CoverageQuery(*layer).permits(TileId::fromWgs84(100,-40,13)));
    }
}
