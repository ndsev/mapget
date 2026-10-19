#include <catch2/catch_test_macros.hpp>
#include <set>
#include "mapget/model/pointcloud-sampler.h"
#include "mapget/model/pointcloud-geometry.h"

namespace
{
struct Record
{
    std::array<double, 3> position;
    uint32_t source;
};
}

TEST_CASE("Point-cloud selection preserves complete small inputs and source records", "[pointcloud]")
{
    mapget::PointCloudGrid grid({-8., -8., -8.}, {8., 8., 8.}, 8);
    mapget::PointCloudSampler<Record> sampler(grid, 8);
    for (uint32_t i = 0; i < 8; ++i) sampler.add({{0., 0., 0.}, i});
    REQUIRE(sampler.records().size() == 8);
    sampler.add({{0., 0., 0.}, 8});
    REQUIRE(sampler.sourceCount() == 9);
    REQUIRE(sampler.records().size() == 1);
    REQUIRE(sampler.records()[0].source == 0);
    sampler.add({{-8., -8., -8.}, 9});
    REQUIRE(sampler.records().size() == 2);
    REQUIRE(sampler.records()[1].source == 9);
}

TEST_CASE("Point-cloud grids bound occupancy and cover endpoints and degenerate extents", "[pointcloud]")
{
    mapget::PointCloudGrid grid({-10., 0., 0.}, {10., 0., 0.}, 7);
    REQUIRE(grid.cellCount() == 4);
    REQUIRE(grid.dimensions() == std::array<uint32_t, 3>{4, 1, 1});
    REQUIRE(grid.index(std::array{-10., 0., 0.}) == 0);
    REQUIRE(grid.index(std::array{10., 0., 0.}) == 3);
    REQUIRE_THROWS(grid.index(std::array{11., 0., 0.}));
    REQUIRE_THROWS(grid.index(std::array{std::numeric_limits<double>::quiet_NaN(), 0., 0.}));
    REQUIRE_THROWS(mapget::PointCloudGrid({0., 0., 0.}, {1., 1., 1.}, 0));
    mapget::PointCloudSampler<Record> omitted(grid, 0);
    omitted.add({{0., 0., 0.}, 1});
    REQUIRE(omitted.sourceCount() == 1);
    REQUIRE(omitted.records().empty());
}

TEST_CASE("Cached first-point representatives reproduce a direct aligned coarse selection", "[pointcloud]")
{
    mapget::PointCloudGrid fineGrid({-16., -16., -16.}, {16., 16., 16.}, 256);
    mapget::PointCloudGrid coarseGrid({-16., -16., -16.}, {16., 16., 16.}, 16);
    mapget::PointCloudSampler<Record> fine(fineGrid, 256, false);
    mapget::PointCloudSampler<Record> direct(coarseGrid, 16, false);
    for (uint32_t i = 0; i < 20000; ++i) {
        Record r{{double((i * 13) % 33) - 16., double((i * 7) % 33) - 16.,
                  double((i / 31) % 33) - 16.}, i};
        fine.add(r);
        direct.add(r);
    }
    mapget::PointCloudSampler<Record> cached(coarseGrid, 16, false);
    for (auto const& record : fine.records()) cached.add(record);
    REQUIRE(cached.records().size() == direct.records().size());
    for (size_t i = 0; i < cached.records().size(); ++i)
        REQUIRE(cached.records()[i].source == direct.records()[i].source);
    REQUIRE(fine.records().size() <= 256);
    REQUIRE(fine.allocatedBytes() <= 256 * sizeof(Record) + fineGrid.occupancyBytes());
}

TEST_CASE("Grid coverage retains the first original point in every occupied compact or thin cell", "[pointcloud]")
{
    for (bool diagonal : {false, true}) {
        mapget::PointCloudGrid grid({0., 0., 0.}, {1., 1., diagonal ? 1. : 0.}, 64);
        mapget::PointCloudSampler<Record> sampler(grid, 64);
        std::set<size_t> expected, actual;
        for (uint32_t i = 0; i < 10000; ++i) {
            double x = double(i % 100) / 99., y = diagonal ? x : double(i / 100) / 99.;
            Record point{{x, y, diagonal ? x : 0.}, i};
            expected.insert(grid.index(point.position));
            sampler.add(point);
        }
        uint32_t previous = 0;
        for (auto const& record : sampler.records()) {
            REQUIRE(record.source >= previous);
            previous = record.source;
            actual.insert(grid.index(record.position));
        }
        REQUIRE(actual == expected);
        REQUIRE(sampler.records().size() == expected.size());
        REQUIRE(sampler.records().size() <= 64);
    }
}

TEST_CASE("Sparse cached geometry still merges occupied coarse cells", "[pointcloud]")
{
    mapget::PointCloudGeometrySample sample;
    sample.lower = {0., 0., 0.};
    sample.upper = {8., 0., 0.};
    sample.quota = 8;
    sample.sourceCount = 100;
    sample.records = {{{0., 0., 0.}, {0., 0., 0.}, 10},
                      {{1.5, 0., 0.}, {0., 0., 1.5}, 11},
                      {{8., 0., 0.}, {0., 0., 8.}, 12}};
    auto coarse = sample.coarsen(4, 0);
    REQUIRE(coarse.size() == 2);
    REQUIRE(coarse[0].geometryAddress == 10);
    REQUIRE(coarse[1].geometryAddress == 12);
    sample.sourceCount = 3;
    REQUIRE(sample.coarsen(4, 0).size() == 3);
    REQUIRE(sample.coarsen(4, 100).size() == 3);
}
