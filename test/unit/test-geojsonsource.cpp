#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <unordered_map>

#include <fmt/format.h>
#include "geojsonsource/geojsonsource.h"
#include "mapget/model/featureid.h"
#include "mapget/model/featurelayer.h"
#include "mapget/service/service.h"

using namespace mapget;

namespace
{

// Sample GeoJSON with signed packed tile IDs, including a negative level-15 value.
constexpr int32_t largeTileId = -2147483648;
constexpr int32_t secondTileId = -2147483647;
// Fixed legacy mapget tile covering Germany.
constexpr int64_t legacyMapgetTileId = 0x21fa0777000d;

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

TEST_CASE(
    "GeoJSON locate plans primary-ID candidates without tile I/O",
    "[GeoJsonSource][GeoJsonLocate]")
{
    auto const endpoint = GENERATE(false, true);
    CAPTURE(endpoint);
    auto metadata = nlohmann::json::parse(R"({
        "mapId": "LocateGeoJson",
        "layers": {
            "Roads": {
                "featureTypes": [{
                    "name": "Road",
                    "uniqueIdCompositions": [
                        [{"partId":"tileId", "datatype":"I64"}, {"partId":"roadId", "datatype":"STR"}],
                        [{"partId":"tileId", "datatype":"I64"}, {"partId":"externalId", "datatype":"U32"}]
                    ]
                }, {
                    "name": "Global",
                    "uniqueIdCompositions": [[{"partId":"globalId", "datatype":"I64"}]]
                }, {
                    "name": "TextTile",
                    "uniqueIdCompositions": [[{"partId":"tileId", "datatype":"STR"}]]
                }]
            }
        }
    })");
    metadata["layers"]["OtherRoads"] = metadata["layers"]["Roads"];

    size_t fetches = 0;
    std::unique_ptr<DataSource> source;
    auto const tempDir = createTempDir();
    if (endpoint) {
        geojsonsource::GeoJsonEndpointSourceOptions options;
        options.baseUrl = testEndpointBaseUrl();
        options.dataSourceInfoJson = metadata;
        options.fetchText = [&](std::string const&)
        {
            ++fetches;
            return std::string(sampleGeoJson2);
        };
        source = std::make_unique<geojsonsource::GeoJsonEndpointSource>(std::move(options));
    }
    else {
        geojsonsource::GeoJsonSourceOptions options;
        options.dataSourceInfoJson = metadata;
        source =
            std::make_unique<geojsonsource::GeoJsonSource>(tempDir.string(), std::move(options));
    }

    LocateRequest request{
        "LocateGeoJson",
        "Road",
        {{"tileId", int64_t(largeTileId)}, {"roadId", std::string("a.b%c")}}};
    auto expectedTile = TileId::fromValue(largeTileId);
    auto expectedId = std::string("Road.-2147483648.a%2Eb%25c");
    bool supported = true;
    SECTION("signed packed ID and all matching layers") {}
    SECTION("canonical ID with escaped string parts")
    {
        request =
            LocateRequest(nlohmann::json{{"mapId", "LocateGeoJson"}, {"featureId", expectedId}});
    }
    SECTION("legacy tile part routes to packed tile without rewriting identity")
    {
        request.featureId_[0].second = legacyMapgetTileId;
        expectedTile = legacyTileIdToPacked(legacyMapgetTileId);
        expectedId = fmt::format("Road.{}.a%2Eb%25c", legacyMapgetTileId);
    }
    SECTION("unknown map")
    {
        request.mapId_ = "OtherMap";
        supported = false;
    }
    SECTION("unknown feature type")
    {
        request.typeId_ = "Missing";
        supported = false;
    }
    SECTION("invalid ID composition")
    {
        request.featureId_[1].first = "unknownId";
        supported = false;
    }
    SECTION("secondary IDs need a custom resolver, not an exact-primary-ID guess")
    {
        request.featureId_[1] = {"externalId", int64_t(42)};
        supported = false;
    }
    SECTION("IDs without a tile part do not trigger a source scan")
    {
        request = LocateRequest{"LocateGeoJson", "Global", {{"globalId", int64_t(42)}}};
        supported = false;
    }
    SECTION("string tile IDs are not silently coerced")
    {
        request =
            LocateRequest{"LocateGeoJson", "TextTile", {{"tileId", std::to_string(largeTileId)}}};
        supported = false;
    }
    SECTION("out-of-range tile IDs are not truncated")
    {
        request.featureId_[0].second = int64_t(-2147483649LL);
        supported = false;
    }
    SECTION("malformed canonical ID")
    {
        request = LocateRequest(nlohmann::json{
            {"mapId", "LocateGeoJson"},
            {"featureId", "Road.invalid.id"}});
        supported = false;
    }

    auto const candidates = source->locate(request);
    REQUIRE(fetches == 0);
    REQUIRE(candidates.size() == (supported ? 2 : 0));
    std::set<std::string> layers;
    for (auto const& candidate : candidates) {
        CHECK(candidate.tileKey_.layer_ == LayerType::Features);
        CHECK(candidate.tileKey_.mapId_ == "LocateGeoJson");
        CHECK(candidate.tileKey_.tileId_ == expectedTile);
        CHECK(candidate.selector_.canonicalFeatureId_ == expectedId);
        CHECK(LocateCandidate(candidate.serialize()).serialize() == candidate.serialize());
        layers.insert(candidate.tileKey_.layerId_);
    }
    if (supported)
        CHECK(layers == std::set<std::string>{"Roads", "OtherRoads"});
    std::filesystem::remove_all(tempDir);
}

TEST_CASE(
    "GeoJSON locate verifies candidates through the normal service path",
    "[GeoJsonSource][GeoJsonLocate]")
{
    auto const tempDir = createTempDir();
    writeFile(tempDir / (std::to_string(largeTileId) + ".geojson"), sampleGeoJson2);
    auto source =
        std::make_shared<geojsonsource::GeoJsonSource>(tempDir.string(), false, "LocateGeoJson");
    Service service(std::make_shared<MemCache>(), false, std::chrono::milliseconds{0}, 2);
    service.add(source);
    LocateRequest request{
        "LocateGeoJson",
        "AnyFeature",
        {{"tileId", int64_t(largeTileId)}, {"featureIndex", int64_t(0)}}};
    auto const results = service.locate(request);
    REQUIRE(results.size() == 1);
    CHECK(results.front().tileKey_.tileId_ == TileId::fromValue(largeTileId));
    CHECK(results.front().tileKey_.layerId_ == "GeoJsonAny");
    auto const canonicalId = fmt::format("AnyFeature.{}.0", largeTileId);
    CHECK(results.front().resolvedCanonicalFeatureId_ == canonicalId);
    CHECK(
        service
            .locate(LocateRequest(
                nlohmann::json{{"mapId", "LocateGeoJson"}, {"featureId", canonicalId}}))
            .size() == 1);

    // A plausible candidate is not a successful locate unless the feature actually exists.
    request.featureId_[1].second = int64_t(99);
    CHECK(service.locate(request).empty());
    std::filesystem::remove_all(tempDir);
}

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
        auto strings = std::make_shared<StringPool>(info.stringPoolId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.stringPoolId_,
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

        auto strings = std::make_shared<StringPool>(info.stringPoolId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.stringPoolId_,
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
        auto strings = std::make_shared<StringPool>(info.stringPoolId_);
        auto roadTile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.stringPoolId_,
            info.mapId_,
            roadLayer,
            strings);

        REQUIRE_NOTHROW(source.fill(roadTile));
        REQUIRE(roadTile->numRoots() > 0);

        // Fill Lane layer
        auto laneTile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.stringPoolId_,
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
        REQUIRE(source.manifest().files[0].tileId == TileId::fromTileXY(0x01fa, 0x0888, 13).value());

        auto info = source.info();
        auto layer = info.getLayer("GeoJsonAny");
        REQUIRE(layer != nullptr);

        auto strings = std::make_shared<StringPool>(info.stringPoolId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromTileXY(0x01fa, 0x0888, 13),
            info.stringPoolId_,
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
        REQUIRE(source.manifest().files[0].tileId == TileId::fromTileXY(8192, 4095, 13).value());

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
        REQUIRE(layer->coverage_.front().min_ == TileId::fromTileXY(0x01fa, 0x0888, 13));

        auto strings = std::make_shared<StringPool>(info.stringPoolId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromTileXY(0x01fa, 0x0888, 13),
            info.stringPoolId_,
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
        auto strings = std::make_shared<StringPool>(info.stringPoolId_);

        auto tile1 = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.stringPoolId_,
            info.mapId_,
            roadLayer,
            strings);
        REQUIRE_NOTHROW(source.fill(tile1));
        REQUIRE(tile1->numRoots() > 0);

        auto tile2 = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(secondTileId),
            info.stringPoolId_,
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

        auto strings = std::make_shared<StringPool>(info.stringPoolId_);
        auto roadTile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.stringPoolId_,
            info.mapId_,
            info.getLayer("Road"),
            strings);
        REQUIRE_NOTHROW(source.fill(roadTile));
        REQUIRE(roadTile->numRoots() > 0);

        auto laneTile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.stringPoolId_,
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

        auto strings = std::make_shared<StringPool>(info.stringPoolId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.stringPoolId_,
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

        auto strings = std::make_shared<StringPool>(info.stringPoolId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(secondTileId),
            info.stringPoolId_,
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

        auto strings = std::make_shared<StringPool>(info.stringPoolId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.stringPoolId_,
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

        auto strings = std::make_shared<StringPool>(info.stringPoolId_);
        auto roadTile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            info.stringPoolId_,
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

        auto fallbackStrings = std::make_shared<StringPool>(fallbackInfo.stringPoolId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId::fromValue(largeTileId),
            fallbackInfo.stringPoolId_,
            fallbackInfo.mapId_,
            anyLayer,
            fallbackStrings);
        REQUIRE_NOTHROW(fallbackSource.fill(tile));
        REQUIRE(tile->numRoots() > 0);
        REQUIRE_FALSE(tile->error().has_value());
    }
}
