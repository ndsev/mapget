#include <catch2/catch_test_macros.hpp>
#include <limits>
#include "mapget/model/pointcloud-sampler.h"
#include "mapget/model/pointcloud-geometry.h"

TEST_CASE("Proportional selection keeps small clouds complete and fills the large-cloud allowance", "[pointcloud]")
{
    constexpr auto quota = mapget::DefaultPointCloudMaxPoints;
    for (uint64_t count : {0ULL, 1ULL, 9'000'000ULL, 13'999'999ULL, 14'000'000ULL, 16'000'000ULL}) {
        mapget::PointCloudIndexSelection selection(count, quota);
        REQUIRE(selection.size() == std::min<uint64_t>(count, quota));
        if (!count) {
            REQUIRE_THROWS_AS(selection.index(0), std::out_of_range);
            continue;
        }
        REQUIRE(selection.index(0) == 0);
        REQUIRE(selection.index(selection.size() - 1) == count - 1);
        // Every source index survives below the threshold; larger inputs are
        // evenly spaced over the entire original object, including its tail.
        uint64_t previous = 0;
        for (uint64_t i = 1; i < selection.size(); ++i) {
            auto index = selection.index(i);
            if (count <= quota) {
                if (index != i) FAIL("A small cloud lost a source record");
            } else if (index <= previous || index - previous > 2) {
                FAIL("Large-cloud selection is not progressive and evenly spaced");
            }
            previous = index;
        }
        REQUIRE_THROWS_AS(selection.index(selection.size()), std::out_of_range);
    }
}

TEST_CASE("Proportional indices avoid overflow and preserve both ends of sparse input", "[pointcloud]")
{
    mapget::PointCloudIndexSelection selection(std::numeric_limits<uint64_t>::max(), 4);
    REQUIRE(selection.size() == 4);
    REQUIRE(selection.index(0) == 0);
    REQUIRE(selection.index(1) == (std::numeric_limits<uint64_t>::max() - 1) / 3);
    REQUIRE(selection.index(3) == std::numeric_limits<uint64_t>::max() - 1);
    REQUIRE(mapget::PointCloudIndexSelection(100, 1).index(0) == 0);
    REQUIRE(mapget::PointCloudIndexSelection(100, 0).size() == 0);
}
