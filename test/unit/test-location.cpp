#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include "mapget/location/location.h"

TEST_CASE("Plazs lookup preserves mapget identities, labels and extents", "[Location]")
{
    mapget::SqliteLocationLookup lookup(MAPGET_TEST_LOCATION_DB);
    REQUIRE(lookup.available());
    auto matches = lookup.search("Allemagne", 10);
    REQUIRE(matches.size() == 1);
    auto const& match = matches.front();
    CHECK(match.id == "wof:1");
    CHECK(match.name == "Germany, DE");
    CHECK(match.source == "whosonfirst");
    CHECK(match.placeType == "country");
    CHECK(match.geometryAvailable);
    CHECK_FALSE(match.geometry);
    CHECK(match.aabb.serialize() == nlohmann::json({{10, 47}, {4, 2}}));
    CHECK(std::abs(match.lonLat.longitude - 11.5) < 1e-7);
    CHECK(std::abs(match.lonLat.latitude - 48) < 1e-7);
    CHECK(match.serialize()["attribution"]["name"] == "Who's On First");
    REQUIRE(lookup.find("wof:1"));
    REQUIRE(lookup.find("wof:1")->geometry);
    CHECK((*lookup.find("wof:1")->geometry)["type"] == "Polygon");
    CHECK(lookup.find("wof:9")->aabb.extent.longitude == 15);
    CHECK_FALSE(lookup.find("wof:999"));
    CHECK_FALSE(lookup.find("wof:1junk"));
    CHECK_FALSE(lookup.find("wof:"));
    CHECK_FALSE(lookup.find("wof:-1"));
    CHECK_FALSE(lookup.find("geonames:1"));
}

TEST_CASE("Missing optional location database does not break the service", "[Location]")
{
    mapget::SqliteLocationLookup lookup("does-not-exist.sqlite");
    CHECK_FALSE(lookup.available());
    CHECK(lookup.search("Munich", 1).empty());
    CHECK_FALSE(lookup.find("wof:1"));
    mapget::LocationMatch custom;
    custom.source = "custom";
    CHECK_FALSE(custom.serialize().contains("attribution"));
}
