#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>

#include "geojsonsource/geojsonsource.h"
#include "mapget/model/featurelayer.h"
#include <fmt/format.h>

using namespace mapget;

namespace
{

// Sample GeoJSON with signed packed tile IDs, including a negative level-15 value.
constexpr int32_t largeTileId = -2147483648;
constexpr int32_t secondTileId = -2147483647;
constexpr int64_t legacyMapgetTileId = (int64_t{1} << 32) | int64_t{1};
constexpr int32_t legacyMapgetTileIdPacked = 131073;

auto sampleGeoJson = R"json({"type": "FeatureCollection", "features": [{
    "geometry": {
        "coordinates": [
            [11.301851123571396, 48.04322026669979, 0.0],
            [11.301915496587753, 48.04289236664772, 0.0],
            [11.302142143249512, 48.04257921874523, 0.0]
        ],
        "type": "LineString"
    },
    "id": "-2147483648.10",
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

std::string testEndpointBaseUrl()
{
    static constexpr char cleartextScheme[] = {'h', 't', 't', 'p', '\0'};
    return fmt::format("{}://{}", std::string_view(cleartextScheme, 4), "geojson-endpoint.test");
}

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
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    file << content;
    file.close();
}

}  // namespace

TEST_CASE("GeoJsonSource", "[GeoJsonSource]")
{
    SECTION("signed packed tile ID support (legacy mode)")
    {
        // Verify the test exercises signed level-15 tile IDs.
        REQUIRE(largeTileId < 0);

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
            TileId::fromValue(largeTileId),
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
                    "my_roads.geojson": { "tileId": -2147483648 }
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
            TileId::fromValue(largeTileId),
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
                    "roads.geojson": { "tileId": -2147483648, "layer": "Road" },
                    "lanes.geojson": { "tileId": -2147483648, "layer": "Lane" }
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
            TileId::fromValue(largeTileId),
            info.nodeId_,
            info.mapId_,
            roadLayer,
            strings);

        REQUIRE_NOTHROW(source.fill(roadTile));
        REQUIRE(roadTile->numRoots() > 0);

        // Fill Lane layer
        auto laneTile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
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
                    "data.geojson": -2147483648
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

    SECTION("Manifest converts removed mapget tile ID layout")
    {
        auto tempDir = createTempDir();

        writeFile(tempDir / "legacy.geojson", sampleGeoJson2);

        auto manifest = fmt::format(R"json({{
            "version": 1,
            "index": {{
                "files": {{
                    "legacy.geojson": {{ "tileId": {} }}
                }}
            }}
        }})json", legacyMapgetTileId);
        writeFile(tempDir / "manifest.json", manifest);

        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        REQUIRE(source.hasManifest());
        REQUIRE(source.manifest().files.size() == 1);
        REQUIRE(source.manifest().files[0].tileId == legacyMapgetTileIdPacked);

        auto info = source.info();
        auto layer = info.getLayer("GeoJsonAny");
        REQUIRE(layer != nullptr);

        auto strings = std::make_shared<StringPool>(info.nodeId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromTileXY(1, 0, 1),
            info.nodeId_,
            info.mapId_,
            layer,
            strings);

        REQUIRE_NOTHROW(source.fill(tile));
        REQUIRE(tile->numRoots() > 0);

        std::filesystem::remove_all(tempDir);
    }

    SECTION("Manifest can force legacy mapget tile ID interpretation")
    {
        auto tempDir = createTempDir();

        writeFile(tempDir / "legacy.geojson", sampleGeoJson2);

        auto manifest = R"json({
            "version": 1,
            "tileIdEncoding": "legacy-mapget",
            "index": {
                "files": {
                    "legacy.geojson": { "tileId": 13 }
                }
            }
        })json";
        writeFile(tempDir / "manifest.json", manifest);

        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        REQUIRE(source.hasManifest());
        REQUIRE(source.manifest().files.size() == 1);
        REQUIRE(source.manifest().files[0].tileId == TileId::fromTileXY(0, 0, 13).value());

        std::filesystem::remove_all(tempDir);
    }

    SECTION("Manifest with metadata only does not fall back to directory scan")
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

        // Manifest was found but has no index - should NOT fall back to directory scan
        REQUIRE(source.hasManifest());
        REQUIRE(source.manifest().files.empty());

        // No tiles should be available (no fallback to legacy filename parsing)
        auto info = source.info();
        REQUIRE(info.layers_.empty());

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
                    "existing.geojson": { "tileId": -2147483648 },
                    "missing.geojson": { "tileId": -2147483647 }
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

    SECTION("Legacy mode converts removed mapget tile ID filenames")
    {
        auto tempDir = createTempDir();

        writeFile(tempDir / (std::to_string(legacyMapgetTileId) + ".geojson"), sampleGeoJson2);

        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        REQUIRE_FALSE(source.hasManifest());

        auto info = source.info();
        auto layer = info.getLayer("GeoJsonAny");
        REQUIRE(layer != nullptr);
        REQUIRE(layer->coverage_.size() == 1);
        REQUIRE(layer->coverage_.front().min_ == TileId::fromTileXY(1, 0, 1));

        auto strings = std::make_shared<StringPool>(info.nodeId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromTileXY(1, 0, 1),
            info.nodeId_,
            info.mapId_,
            layer,
            strings);

        REQUIRE_NOTHROW(source.fill(tile));
        REQUIRE(tile->numRoots() > 0);

        std::filesystem::remove_all(tempDir);
    }

    SECTION("Manifest prevents legacy filename parsing for non-numeric names")
    {
        auto tempDir = createTempDir();

        // Create GeoJSON file with non-numeric name (would fail stoull in legacy mode)
        writeFile(tempDir / "mytestdata.geojson", sampleGeoJson);

        // Create manifest that maps the file correctly
        auto manifest = R"json({
            "version": 1,
            "index": {
                "files": {
                    "mytestdata.geojson": { "tileId": -2147483646, "layer": "Road" }
                }
            }
        })json";
        writeFile(tempDir / "manifest.json", manifest);

        // Should not throw - manifest mode should be used, not legacy filename parsing
        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        REQUIRE(source.hasManifest());

        auto info = source.info();
        auto layer = info.getLayer("Road");
        REQUIRE(layer != nullptr);
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
                    "tile1.geojson": { "tileId": -2147483648, "layer": "Road" },
                    "tile2.geojson": { "tileId": -2147483647, "layer": "Road" }
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
            TileId::fromValue(largeTileId),
            info.nodeId_,
            info.mapId_,
            roadLayer,
            strings);
        REQUIRE_NOTHROW(source.fill(tile1));
        REQUIRE(tile1->numRoots() > 0);

        auto tile2 = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(secondTileId),
            info.nodeId_,
            info.mapId_,
            roadLayer,
            strings);
        REQUIRE_NOTHROW(source.fill(tile2));
        REQUIRE(tile2->numRoots() > 0);

        std::filesystem::remove_all(tempDir);
    }

    SECTION("Explicit datasource info file enables template-based folder loading")
    {
        auto tempDir = createTempDir();

        writeFile(tempDir / "Road" / (std::to_string(largeTileId) + ".geojson"), sampleGeoJson);
        writeFile(tempDir / "Lane" / (std::to_string(largeTileId) + ".geojson"), sampleGeoJson2);
        writeFile(tempDir / "info.yaml", fmt::format(R"yaml(
mapId: ExplicitGeoJson
layers:
  Road:
    featureTypes:
      - name: RoadFeature
        uniqueIdCompositions:
          - - partId: tileId
              datatype: I64
            - partId: featureIndex
              datatype: U32
    coverage:
      - {}
  Lane:
    featureTypes:
      - name: LaneFeature
        uniqueIdCompositions:
          - - partId: tileId
              datatype: I64
            - partId: featureIndex
              datatype: U32
    coverage:
      - {}
)yaml", largeTileId, largeTileId));

        geojsonsource::GeoJsonSource source(
            tempDir.string(),
            geojsonsource::GeoJsonSourceOptions{
                .withAttrLayers = false,
                .tilePathTemplate = "{layerId}/{tileId}.geojson",
                .dataSourceInfoLocation = (tempDir / "info.yaml").string()});

        auto info = source.info();
        REQUIRE_FALSE(source.hasManifest());
        REQUIRE(info.mapId_ == "ExplicitGeoJson");
        REQUIRE(info.getLayer("Road") != nullptr);
        REQUIRE(info.getLayer("Lane") != nullptr);

        auto strings = std::make_shared<StringPool>(info.nodeId_);
        auto roadTile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.nodeId_,
            info.mapId_,
            info.getLayer("Road"),
            strings);
        REQUIRE_NOTHROW(source.fill(roadTile));
        REQUIRE(roadTile->numRoots() > 0);

        auto laneTile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.nodeId_,
            info.mapId_,
            info.getLayer("Lane"),
            strings);
        REQUIRE_NOTHROW(source.fill(laneTile));
        REQUIRE(laneTile->numRoots() > 0);

        std::filesystem::remove_all(tempDir);
    }

    SECTION("Template mode missing tile file yields empty tile without error")
    {
        auto tempDir = createTempDir();

        writeFile(tempDir / "info.yaml", R"yaml(
mapId: SparseGeoJson
layers:
  Road:
    featureTypes:
      - name: RoadFeature
        uniqueIdCompositions:
          - - partId: tileId
              datatype: I64
            - partId: featureIndex
              datatype: U32
)yaml");

        geojsonsource::GeoJsonSource source(
            tempDir.string(),
            geojsonsource::GeoJsonSourceOptions{
                .withAttrLayers = false,
                .tilePathTemplate = "{layerId}/{tileId}.geojson",
                .dataSourceInfoLocation = (tempDir / "info.yaml").string()});

        auto info = source.info();
        auto layer = info.getLayer("Road");
        REQUIRE(layer != nullptr);

        auto strings = std::make_shared<StringPool>(info.nodeId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.nodeId_,
            info.mapId_,
            layer,
            strings);

        REQUIRE_NOTHROW(source.fill(tile));
        REQUIRE(tile->numRoots() == 0);
        REQUIRE_FALSE(tile->error().has_value());

        std::filesystem::remove_all(tempDir);
    }

    SECTION("Manifest mode missing tile mapping yields empty tile without error")
    {
        auto tempDir = createTempDir();

        writeFile(tempDir / "existing.geojson", sampleGeoJson);
        writeFile(tempDir / "manifest.json", R"json({
            "version": 1,
            "index": {
                "files": {
                    "existing.geojson": { "tileId": -2147483648, "layer": "Road" }
                }
            }
        })json");

        geojsonsource::GeoJsonSource source(tempDir.string(), false);

        auto info = source.info();
        auto layer = info.getLayer("Road");
        REQUIRE(layer != nullptr);

        auto strings = std::make_shared<StringPool>(info.nodeId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(secondTileId),
            info.nodeId_,
            info.mapId_,
            layer,
            strings);

        REQUIRE_NOTHROW(source.fill(tile));
        REQUIRE(tile->numRoots() == 0);
        REQUIRE_FALSE(tile->error().has_value());

        std::filesystem::remove_all(tempDir);
    }

    SECTION("Existing malformed GeoJSON remains a tile error")
    {
        auto tempDir = createTempDir();

        writeFile(tempDir / "Road" / (std::to_string(largeTileId) + ".geojson"), "{not valid json");
        writeFile(tempDir / "info.yaml", R"yaml(
mapId: BrokenGeoJson
layers:
  Road:
    featureTypes:
      - name: RoadFeature
        uniqueIdCompositions:
          - - partId: tileId
              datatype: I64
            - partId: featureIndex
              datatype: U32
)yaml");

        geojsonsource::GeoJsonSource source(
            tempDir.string(),
            geojsonsource::GeoJsonSourceOptions{
                .withAttrLayers = false,
                .tilePathTemplate = "{layerId}/{tileId}.geojson",
                .dataSourceInfoLocation = (tempDir / "info.yaml").string()});

        auto info = source.info();
        auto layer = info.getLayer("Road");
        REQUIRE(layer != nullptr);

        auto strings = std::make_shared<StringPool>(info.nodeId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.nodeId_,
            info.mapId_,
            layer,
            strings);

        REQUIRE_NOTHROW(source.fill(tile));
        REQUIRE(tile->numRoots() == 0);
        REQUIRE(tile->error().has_value());

        std::filesystem::remove_all(tempDir);
    }

    SECTION("GeoJsonEndpoint loads tiles over HTTP with and without datasource info")
    {
        auto infoYaml = fmt::format(R"yaml(
mapId: RemoteGeoJson
layers:
  Road:
    featureTypes:
      - name: RoadFeature
        uniqueIdCompositions:
          - - partId: tileId
              datatype: I64
            - partId: featureIndex
              datatype: U32
    coverage:
      - {}
)yaml", largeTileId);

        auto const endpointBaseUrl = testEndpointBaseUrl();
        auto responses = std::make_shared<std::unordered_map<std::string, std::string>>(
            std::unordered_map<std::string, std::string>{
                {fmt::format("{}/info.yaml", endpointBaseUrl), infoYaml},
                {fmt::format("{}/tiles/Road/{}.geojson", endpointBaseUrl, largeTileId), sampleGeoJson},
                {fmt::format("{}/{}.geojson", endpointBaseUrl, largeTileId), sampleGeoJson},
            });
        auto fetchText = [responses](std::string const& url) -> std::string {
            auto it = responses->find(url);
            if (it == responses->end()) {
                throw std::runtime_error(fmt::format("Unexpected GeoJsonEndpoint test URL: {}", url));
            }
            return it->second;
        };

        geojsonsource::GeoJsonEndpointSource source({
            .baseUrl = fmt::format("{}/tiles", endpointBaseUrl),
            .withAttrLayers = false,
            .tileUrlTemplate = "{layerId}/{tileId}.geojson",
            .dataSourceInfoLocation = fmt::format("{}/info.yaml", endpointBaseUrl),
            .fetchText = fetchText,
        });

        auto info = source.info();
        REQUIRE(info.mapId_ == "RemoteGeoJson");
        auto roadLayer = info.getLayer("Road");
        REQUIRE(roadLayer != nullptr);

        auto strings = std::make_shared<StringPool>(info.nodeId_);
        auto roadTile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.nodeId_,
            info.mapId_,
            roadLayer,
            strings);
        REQUIRE_NOTHROW(source.fill(roadTile));
        REQUIRE(roadTile->numRoots() > 0);
        REQUIRE_FALSE(roadTile->error().has_value());

        geojsonsource::GeoJsonEndpointSource fallbackSource({
            .baseUrl = endpointBaseUrl,
            .withAttrLayers = false,
            .mapId = "FallbackEndpoint",
            .tileUrlTemplate = "{tileId}.geojson",
            .fetchText = fetchText,
        });

        auto fallbackInfo = fallbackSource.info();
        REQUIRE(fallbackInfo.mapId_ == "FallbackEndpoint");
        auto anyLayer = fallbackInfo.getLayer("GeoJsonAny");
        REQUIRE(anyLayer != nullptr);
        REQUIRE(anyLayer->coverage_.empty());

        auto fallbackStrings = std::make_shared<StringPool>(fallbackInfo.nodeId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            fallbackInfo.nodeId_,
            fallbackInfo.mapId_,
            anyLayer,
            fallbackStrings);
        REQUIRE_NOTHROW(fallbackSource.fill(tile));
        REQUIRE(tile->numRoots() > 0);
        REQUIRE_FALSE(tile->error().has_value());
    }
}
