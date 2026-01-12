#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

#include "geojsonsource/geojsonsource.h"
#include "mapget/model/featurelayer.h"

using namespace mapget;

namespace
{

// Sample GeoJSON with a 64-bit tile ID (37392110387213 > UINT32_MAX)
constexpr uint64_t largeTileId = 37392110387213;

auto sampleGeoJson = R"json({"type": "FeatureCollection", "features": [{
    "geometry": {
        "coordinates": [
            [11.301851123571396, 48.04322026669979, 0.0],
            [11.301915496587753, 48.04289236664772, 0.0],
            [11.302142143249512, 48.04257921874523, 0.0]
        ],
        "type": "LineString"
    },
    "id": "37392110387213.10",
    "properties": {
        "length": 100
    },
    "featureIndex": 0,
    "type": "Feature"
}]})json";

std::filesystem::path createTempGeoJsonDir()
{
    auto tempDir = std::filesystem::temp_directory_path() / "mapget_geojson_test";
    std::filesystem::create_directories(tempDir);

    auto geojsonPath = tempDir / (std::to_string(largeTileId) + ".geojson");
    std::ofstream file(geojsonPath);
    file << sampleGeoJson;
    file.close();

    return tempDir;
}

}  // namespace

TEST_CASE("GeoJsonSource", "[GeoJsonSource]")
{
    SECTION("64-bit tile ID support")
    {
        // Verify our test tile ID exceeds 32-bit max
        REQUIRE(largeTileId > UINT32_MAX);

        auto tempDir = createTempGeoJsonDir();

        // Create GeoJsonSource - should not throw with 64-bit tile IDs
        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        // Get source info and verify coverage includes our tile
        auto info = source.info();
        auto layer = info.getLayer("GeoJsonAny");
        REQUIRE(layer != nullptr);
        REQUIRE(!layer->coverage_.empty());

        // Create a TileFeatureLayer to fill
        auto strings = std::make_shared<StringPool>(info.nodeId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId(largeTileId),
            info.nodeId_,
            info.mapId_,
            layer,
            strings);

        // fill() should succeed without ID validation errors
        REQUIRE_NOTHROW(source.fill(tile));

        // Verify feature was created
        REQUIRE(tile->numRoots() > 0);

        // Cleanup
        std::filesystem::remove_all(tempDir);
    }
}
