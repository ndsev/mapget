#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "geojsonsource/geojsonsource.h"
#include "mapget/model/featurelayer.h"

using namespace mapget;

namespace
{

// Sample GeoJSON with a 64-bit tile ID (37392110387213 > UINT32_MAX)
constexpr uint64_t largeTileId = 37392110387213;
constexpr uint64_t secondTileId = 37392110387214;

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

auto sampleGeoJson2 = R"json({"type": "FeatureCollection", "features": [{
    "geometry": {
        "coordinates": [11.30, 48.04, 0.0],
        "type": "Point"
    },
    "properties": {
        "name": "Test Point"
    },
    "type": "Feature"
}]})json";

std::filesystem::path createTempDir()
{
    auto now = std::chrono::system_clock::now();
    auto epochTime = std::chrono::system_clock::to_time_t(now);
    auto tempDir = std::filesystem::temp_directory_path() /
        ("mapget_geojson_test_" + std::to_string(epochTime) + "_" +
         std::to_string(std::rand()));

    if (std::filesystem::exists(tempDir)) {
        std::filesystem::remove_all(tempDir);
    }
    std::filesystem::create_directories(tempDir);

    return tempDir;
}

void writeFile(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream file(path);
    file << content;
    file.close();
}

}  // namespace

TEST_CASE("GeoJsonSource", "[GeoJsonSource]")
{
    SECTION("64-bit tile ID support (legacy mode)")
    {
        // Verify our test tile ID exceeds 32-bit max
        REQUIRE(largeTileId > UINT32_MAX);

        auto tempDir = createTempDir();

        // Create GeoJSON file with tile ID as filename (legacy mode)
        writeFile(tempDir / (std::to_string(largeTileId) + ".geojson"), sampleGeoJson);

        // Create GeoJsonSource - should not throw with 64-bit tile IDs
        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        // Should be in legacy mode (no manifest)
        REQUIRE_FALSE(source.hasManifest());

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

    SECTION("Manifest with single layer")
    {
        auto tempDir = createTempDir();

        // Create GeoJSON file with custom name
        writeFile(tempDir / "my_roads.geojson", sampleGeoJson);

        // Create manifest.json
        auto manifest = R"json({
            "version": 1,
            "metadata": {
                "name": "Test Dataset",
                "source": "Unit Test"
            },
            "index": {
                "files": {
                    "my_roads.geojson": { "tileId": 37392110387213 }
                }
            }
        })json";
        writeFile(tempDir / "manifest.json", manifest);

        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        REQUIRE(source.hasManifest());
        REQUIRE(source.manifest().metadata.name == "Test Dataset");
        REQUIRE(source.manifest().metadata.source == "Unit Test");

        auto info = source.info();
        auto layer = info.getLayer("GeoJsonAny");
        REQUIRE(layer != nullptr);

        auto strings = std::make_shared<StringPool>(info.nodeId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId(largeTileId),
            info.nodeId_,
            info.mapId_,
            layer,
            strings);

        REQUIRE_NOTHROW(source.fill(tile));
        REQUIRE(tile->numRoots() > 0);

        std::filesystem::remove_all(tempDir);
    }

    SECTION("Manifest with multiple layers")
    {
        auto tempDir = createTempDir();

        // Create GeoJSON files for different layers
        writeFile(tempDir / "roads.geojson", sampleGeoJson);
        writeFile(tempDir / "lanes.geojson", sampleGeoJson2);

        // Create manifest with multiple layers
        auto manifest = R"json({
            "version": 1,
            "index": {
                "defaultLayer": "GeoJsonAny",
                "files": {
                    "roads.geojson": { "tileId": 37392110387213, "layer": "Road" },
                    "lanes.geojson": { "tileId": 37392110387213, "layer": "Lane" }
                }
            }
        })json";
        writeFile(tempDir / "manifest.json", manifest);

        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        REQUIRE(source.hasManifest());

        auto info = source.info();

        // Verify both layers exist
        auto roadLayer = info.getLayer("Road");
        auto laneLayer = info.getLayer("Lane");
        REQUIRE(roadLayer != nullptr);
        REQUIRE(laneLayer != nullptr);

        // Verify feature type names
        REQUIRE(roadLayer->featureTypes_.size() == 1);
        REQUIRE(roadLayer->featureTypes_[0].name_ == "RoadFeature");
        REQUIRE(laneLayer->featureTypes_.size() == 1);
        REQUIRE(laneLayer->featureTypes_[0].name_ == "LaneFeature");

        // Fill Road layer
        auto strings = std::make_shared<StringPool>(info.nodeId_);
        auto roadTile = std::make_shared<TileFeatureLayer>(
            TileId(largeTileId),
            info.nodeId_,
            info.mapId_,
            roadLayer,
            strings);

        REQUIRE_NOTHROW(source.fill(roadTile));
        REQUIRE(roadTile->numRoots() > 0);

        // Fill Lane layer
        auto laneTile = std::make_shared<TileFeatureLayer>(
            TileId(largeTileId),
            info.nodeId_,
            info.mapId_,
            laneLayer,
            strings);

        REQUIRE_NOTHROW(source.fill(laneTile));
        REQUIRE(laneTile->numRoots() > 0);

        std::filesystem::remove_all(tempDir);
    }

    SECTION("Manifest with short tile ID format")
    {
        auto tempDir = createTempDir();

        writeFile(tempDir / "data.geojson", sampleGeoJson);

        // Use short format (just tile ID number)
        auto manifest = R"json({
            "version": 1,
            "index": {
                "files": {
                    "data.geojson": 37392110387213
                }
            }
        })json";
        writeFile(tempDir / "manifest.json", manifest);

        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        REQUIRE(source.hasManifest());
        REQUIRE(source.manifest().files.size() == 1);
        REQUIRE(source.manifest().files[0].tileId == largeTileId);

        std::filesystem::remove_all(tempDir);
    }

    SECTION("Manifest with metadata only falls back to directory scan")
    {
        auto tempDir = createTempDir();

        // Create GeoJSON file with tile ID as filename
        writeFile(tempDir / (std::to_string(largeTileId) + ".geojson"), sampleGeoJson);

        // Create manifest with only metadata (no index)
        auto manifest = R"json({
            "version": 1,
            "metadata": {
                "name": "Metadata Only",
                "description": "Dataset with no index section"
            }
        })json";
        writeFile(tempDir / "manifest.json", manifest);

        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        // Manifest was found but has no index, so falls back to directory scan
        REQUIRE(source.hasManifest());
        REQUIRE(source.manifest().files.empty());

        // But tiles should still be discovered from filenames
        auto info = source.info();
        auto layer = info.getLayer("GeoJsonAny");
        REQUIRE(layer != nullptr);
        REQUIRE(!layer->coverage_.empty());

        std::filesystem::remove_all(tempDir);
    }

    SECTION("Manifest with missing file warns and skips")
    {
        auto tempDir = createTempDir();

        // Create only one of the two files listed in manifest
        writeFile(tempDir / "existing.geojson", sampleGeoJson);

        auto manifest = R"json({
            "version": 1,
            "index": {
                "files": {
                    "existing.geojson": { "tileId": 37392110387213 },
                    "missing.geojson": { "tileId": 37392110387214 }
                }
            }
        })json";
        writeFile(tempDir / "manifest.json", manifest);

        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        REQUIRE(source.hasManifest());
        // Only the existing file should be registered
        REQUIRE(source.manifest().files.size() == 1);
        REQUIRE(source.manifest().files[0].filename == "existing.geojson");

        std::filesystem::remove_all(tempDir);
    }

    SECTION("Legacy mode skips non-numeric filenames")
    {
        auto tempDir = createTempDir();

        // Create files with valid and invalid names
        writeFile(tempDir / (std::to_string(largeTileId) + ".geojson"), sampleGeoJson);
        writeFile(tempDir / "not_a_number.geojson", sampleGeoJson2);
        writeFile(tempDir / "readme.txt", "Not a geojson file");

        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        REQUIRE_FALSE(source.hasManifest());

        auto info = source.info();
        auto layer = info.getLayer("GeoJsonAny");
        REQUIRE(layer != nullptr);
        // Only the valid tile ID file should be registered
        REQUIRE(layer->coverage_.size() == 1);

        std::filesystem::remove_all(tempDir);
    }

    SECTION("Multiple tiles same layer")
    {
        auto tempDir = createTempDir();

        writeFile(tempDir / "tile1.geojson", sampleGeoJson);
        writeFile(tempDir / "tile2.geojson", sampleGeoJson2);

        auto manifest = R"json({
            "version": 1,
            "index": {
                "files": {
                    "tile1.geojson": { "tileId": 37392110387213, "layer": "Road" },
                    "tile2.geojson": { "tileId": 37392110387214, "layer": "Road" }
                }
            }
        })json";
        writeFile(tempDir / "manifest.json", manifest);

        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        auto info = source.info();
        auto roadLayer = info.getLayer("Road");
        REQUIRE(roadLayer != nullptr);
        REQUIRE(roadLayer->coverage_.size() == 2);

        // Fill both tiles
        auto strings = std::make_shared<StringPool>(info.nodeId_);

        auto tile1 = std::make_shared<TileFeatureLayer>(
            TileId(largeTileId),
            info.nodeId_,
            info.mapId_,
            roadLayer,
            strings);
        REQUIRE_NOTHROW(source.fill(tile1));
        REQUIRE(tile1->numRoots() > 0);

        auto tile2 = std::make_shared<TileFeatureLayer>(
            TileId(secondTileId),
            info.nodeId_,
            info.mapId_,
            roadLayer,
            strings);
        REQUIRE_NOTHROW(source.fill(tile2));
        REQUIRE(tile2->numRoots() > 0);

        std::filesystem::remove_all(tempDir);
    }
}
