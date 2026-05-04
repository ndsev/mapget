#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "geojsonsource/geojsonsource.h"
#include "mapget/detail/http-server.h"
#include "mapget/model/featurelayer.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <fmt/format.h>

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
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    file << content;
    file.close();
}

class TestGeoJsonEndpointServer : public mapget::HttpServer
{
public:
    explicit TestGeoJsonEndpointServer(std::unordered_map<std::string, std::string> responses)
        : responses_(std::move(responses))
    {
    }

protected:
    void setup(drogon::HttpAppFramework& app) override
    {
        for (const auto& [path, body] : responses_) {
            app.registerHandler(
                path,
                [body](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                    auto response = drogon::HttpResponse::newHttpResponse();
                    response->setStatusCode(drogon::k200OK);
                    response->setBody(body);
                    callback(response);
                },
                {drogon::Get});
        }
    }

private:
    std::unordered_map<std::string, std::string> responses_;
};

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
                    "mytestdata.geojson": { "tileId": 62530591326221, "layer": "Road" }
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
              datatype: U64
            - partId: featureIndex
              datatype: U32
    coverage:
      - {}
  Lane:
    featureTypes:
      - name: LaneFeature
        uniqueIdCompositions:
          - - partId: tileId
              datatype: U64
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
            TileId(largeTileId),
            info.nodeId_,
            info.mapId_,
            info.getLayer("Road"),
            strings);
        REQUIRE_NOTHROW(source.fill(roadTile));
        REQUIRE(roadTile->numRoots() > 0);

        auto laneTile = std::make_shared<TileFeatureLayer>(
            TileId(largeTileId),
            info.nodeId_,
            info.mapId_,
            info.getLayer("Lane"),
            strings);
        REQUIRE_NOTHROW(source.fill(laneTile));
        REQUIRE(laneTile->numRoots() > 0);

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
              datatype: U64
            - partId: featureIndex
              datatype: U32
    coverage:
      - {}
)yaml", largeTileId);

        TestGeoJsonEndpointServer server({
            {"/info.yaml", infoYaml},
            {fmt::format("/tiles/Road/{}.geojson", largeTileId), sampleGeoJson},
            {fmt::format("/{}.geojson", largeTileId), sampleGeoJson},
        });
        server.go("127.0.0.1", 0, 2000);

        auto baseUrl = fmt::format("http://127.0.0.1:{}/tiles", server.port());
        auto infoUrl = fmt::format("http://127.0.0.1:{}/info.yaml", server.port());

        geojsonsource::GeoJsonEndpointSource source({
            .baseUrl = baseUrl,
            .withAttrLayers = false,
            .tileUrlTemplate = "{layerId}/{tileId}.geojson",
            .dataSourceInfoLocation = infoUrl,
        });

        auto info = source.info();
        REQUIRE(info.mapId_ == "RemoteGeoJson");
        auto roadLayer = info.getLayer("Road");
        REQUIRE(roadLayer != nullptr);

        auto strings = std::make_shared<StringPool>(info.nodeId_);
        auto roadTile = std::make_shared<TileFeatureLayer>(
            TileId(largeTileId),
            info.nodeId_,
            info.mapId_,
            roadLayer,
            strings);
        REQUIRE_NOTHROW(source.fill(roadTile));
        REQUIRE(roadTile->numRoots() > 0);
        REQUIRE_FALSE(roadTile->error().has_value());

        geojsonsource::GeoJsonEndpointSource fallbackSource({
            .baseUrl = fmt::format("http://127.0.0.1:{}", server.port()),
            .withAttrLayers = false,
            .mapId = "FallbackEndpoint",
            .tileUrlTemplate = "{tileId}.geojson",
        });

        auto fallbackInfo = fallbackSource.info();
        REQUIRE(fallbackInfo.mapId_ == "FallbackEndpoint");
        auto anyLayer = fallbackInfo.getLayer("GeoJsonAny");
        REQUIRE(anyLayer != nullptr);
        REQUIRE(anyLayer->coverage_.empty());

        auto fallbackStrings = std::make_shared<StringPool>(fallbackInfo.nodeId_);
        auto tile = std::make_shared<TileFeatureLayer>(
            TileId(largeTileId),
            fallbackInfo.nodeId_,
            fallbackInfo.mapId_,
            anyLayer,
            fallbackStrings);
        REQUIRE_NOTHROW(fallbackSource.fill(tile));
        REQUIRE(tile->numRoots() > 0);
        REQUIRE_FALSE(tile->error().has_value());
    }
}
