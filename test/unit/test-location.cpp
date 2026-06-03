#include "mapget/location/geonames-import.h"
#include "mapget/location/location.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

std::filesystem::path makeTempDir()
{
    static std::atomic<uint64_t> counter{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
        ("mapget-location-test-" + std::to_string(now) + "-" + std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(path);
    return path;
}

std::string joinTabs(std::vector<std::string> const& values)
{
    std::string result;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            result.push_back('\t');
        }
        result += values[i];
    }
    result.push_back('\n');
    return result;
}

std::vector<std::string> geonamesRow(
    std::string geonameId,
    std::string name,
    std::string asciiName,
    std::string alternateNames,
    std::string latitude,
    std::string longitude,
    std::string featureCode,
    std::string countryCode,
    std::string population)
{
    return {
        std::move(geonameId),
        std::move(name),
        std::move(asciiName),
        std::move(alternateNames),
        std::move(latitude),
        std::move(longitude),
        "P",
        std::move(featureCode),
        std::move(countryCode),
        "",
        "",
        "",
        "",
        "",
        std::move(population),
        "",
        "",
        "UTC",
        "2026-01-01",
    };
}

}  // namespace

TEST_CASE("GeoNames location database imports rows and ranks matches", "[Location]")
{
    auto tempDir = makeTempDir();
    auto inputPath = tempDir / "cities1000.txt";
    auto dbPath = tempDir / "geonames-cities1000.sqlite";

    {
        std::ofstream input(inputPath);
        input << joinTabs(geonamesRow(
            "2867714",
            "Muenchen",
            "Munich",
            "Muenchen,München",
            "48.13743",
            "11.57549",
            "PPLA",
            "DE",
            "1260391"));
        input << joinTabs(geonamesRow(
            "5690557",
            "Munich",
            "Munich",
            "Munich North Dakota",
            "46.80667",
            "-98.83926",
            "PPL",
            "US",
            "190"));
        input << joinTabs(geonamesRow(
            "2867543",
            "Munsingen",
            "Munsingen",
            "Münsingen",
            "48.41126",
            "9.49704",
            "PPL",
            "DE",
            "14000"));
    }

    auto stats = mapget::createGeonamesLocationDatabase(inputPath, dbPath);
    REQUIRE(stats.rowsRead == 3);
    REQUIRE(stats.rowsImported == 3);

    mapget::SqliteLocationLookup lookup(dbPath);
    REQUIRE(lookup.available());

    auto munichMatches = lookup.search("munich", 10);
    REQUIRE(munichMatches.size() == 2);
    CHECK(munichMatches[0].id == "geonames:2867714");
    CHECK(munichMatches[0].name == "Munich, DE");
    CHECK(std::abs(munichMatches[0].longitude - 11.57549) < 1e-8);
    CHECK(std::abs(munichMatches[0].latitude - 48.13743) < 1e-8);
    REQUIRE(munichMatches[0].population.has_value());
    CHECK(*munichMatches[0].population == 1260391);

    auto alternateNameMatches = lookup.search("muen", 10);
    REQUIRE_FALSE(alternateNameMatches.empty());
    CHECK(alternateNameMatches[0].id == "geonames:2867714");

    auto limitedMatches = lookup.search("mun", 1);
    REQUIRE(limitedMatches.size() == 1);

    auto serialized = munichMatches[0].serialize();
    CHECK(std::abs(serialized["lonLat"][0].get<double>() - 11.57549) < 1e-8);
    CHECK(std::abs(serialized["lonLat"][1].get<double>() - 48.13743) < 1e-8);
    CHECK(std::abs(serialized["aabb"][0][0].get<double>() - 11.57549) < 1e-8);
    CHECK(std::abs(serialized["aabb"][0][1].get<double>() - 48.13743) < 1e-8);
    CHECK(serialized["aabb"][1][0] == 0);
    CHECK(serialized["aabb"][1][1] == 0);

    std::filesystem::remove_all(tempDir);
}

TEST_CASE("GeoNames location lookup ignores empty and unavailable inputs", "[Location]")
{
    auto lookup = mapget::SqliteLocationLookup(std::filesystem::path("does-not-exist.sqlite"));
    CHECK_FALSE(lookup.available());
    CHECK(lookup.search("Munich", 10).empty());
}
