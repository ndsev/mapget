#pragma once

#include <unordered_set>
#include <unordered_map>
#include <string>
#include <optional>

#include "mapget/model/featurelayer.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/service/datasource.h"

namespace mapget::geojsonsource
{

/**
 * Entry describing a single GeoJSON file in the manifest.
 */
struct FileEntry
{
    std::string filename;
    uint64_t tileId = 0;
    std::string layer;  // Empty means use default layer
};

/**
 * Metadata section of the manifest (all fields optional).
 */
struct ManifestMetadata
{
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::optional<std::string> source;
    std::optional<std::string> created;
    std::optional<std::string> author;
    std::optional<std::string> license;
};

/**
 * Parsed manifest.json structure.
 */
struct Manifest
{
    int version = 1;
    ManifestMetadata metadata;
    std::string defaultLayer = "GeoJsonAny";
    std::vector<FileEntry> files;
};

/**
 * Key for looking up files by (tileId, layer).
 */
struct TileLayerKey
{
    uint64_t tileId;
    std::string layer;

    bool operator==(const TileLayerKey& other) const {
        return tileId == other.tileId && layer == other.layer;
    }
};

struct TileLayerKeyHash
{
    std::size_t operator()(const TileLayerKey& k) const {
        return std::hash<uint64_t>()(k.tileId) ^ (std::hash<std::string>()(k.layer) << 1);
    }
};

/**
 * Data Source which may be used to load GeoJSON files from a directory.
 *
 * Supports two modes of operation:
 *
 * 1. **Manifest mode** (recommended): If a `manifest.json` file exists in the
 *    input directory, it is used to map filenames to tile IDs and layers.
 *    This allows arbitrary filenames and multi-layer support.
 *
 *    Example manifest.json:
 *    ```json
 *    {
 *      "version": 1,
 *      "metadata": {
 *        "name": "My Dataset",
 *        "source": "OpenStreetMap",
 *        "created": "2024-01-15"
 *      },
 *      "index": {
 *        "defaultLayer": "GeoJsonAny",
 *        "files": {
 *          "roads.geojson": { "tileId": 121212121212, "layer": "Road" },
 *          "lanes.geojson": { "tileId": 121212121212, "layer": "Lane" },
 *          "other.geojson": { "tileId": 343434343434 }
 *        }
 *      }
 *    }
 *    ```
 *
 * 2. **Legacy mode**: If no manifest.json exists, falls back to scanning for
 *    files named `<packed-tile-id>.geojson`. All files go into a single
 *    "GeoJsonAny" layer.
 *
 * Note: This data source was mainly developed as a scalability test
 *  scenario for erdblick. In the future, the DBI will export the same
 *  GeoJSON feature model that is understood by mapget, and a GeoJSON
 *  data source will be part of the mapget code base.
 */
class GeoJsonSource : public mapget::DataSource
{
public:
    /**
     * Construct a GeoJSON data source from a directory.
     *
     * If a manifest.json exists in the directory, it will be used for
     * file-to-tile mapping and layer configuration. Otherwise, falls back
     * to legacy mode where files must be named `<tile-id>.geojson`.
     *
     * @param inputDir The directory with the GeoJSON files (and optional manifest.json).
     * @param withAttrLayers Flag indicating whether compound GeoJSON
     *  properties shall be converted to mapget attribute layers.
     * @param mapId Optional map ID override. If empty, derived from inputDir.
     */
    GeoJsonSource(const std::string& inputDir, bool withAttrLayers, const std::string& mapId="");

    /** DataSource Interface */
    mapget::DataSourceInfo info() override;
    void fill(mapget::TileFeatureLayer::Ptr const&) override;
    void fill(mapget::TileSourceDataLayer::Ptr const&) override;

    /** Returns true if a manifest.json was found and used. */
    [[nodiscard]] bool hasManifest() const { return hasManifest_; }

    /** Returns the parsed manifest (only valid if hasManifest() is true). */
    [[nodiscard]] const Manifest& manifest() const { return manifest_; }

private:
    /** Parse manifest.json from the input directory. Returns true if found and valid. */
    bool parseManifest();

    /** Initialize coverage from manifest entries. */
    void initFromManifest();

    /** Initialize coverage by scanning directory for <tile-id>.geojson files (legacy). */
    void initFromDirectory();

    /** Create LayerInfo JSON for a given layer name. */
    static nlohmann::json createLayerInfoJson(const std::string& layerName);

    mapget::DataSourceInfo info_;
    std::string inputDir_;
    bool withAttrLayers_ = true;
    bool hasManifest_ = false;
    Manifest manifest_;

    // Mapping from (tileId, layer) -> filename
    std::unordered_map<TileLayerKey, std::string, TileLayerKeyHash> tileLayerToFile_;

    // Set of covered tile IDs per layer (for legacy single-layer mode compatibility)
    std::unordered_map<std::string, std::unordered_set<uint64_t>> layerCoverage_;
};

}  // namespace mapget::geojsonsource
