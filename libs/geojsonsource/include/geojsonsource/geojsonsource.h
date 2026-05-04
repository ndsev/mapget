#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "mapget/model/featurelayer.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/service/datasource.h"

#include <nlohmann/json.hpp>

namespace mapget::geojsonsource
{

/**
 * Entry describing a single GeoJSON file in the legacy manifest.
 */
struct FileEntry
{
    std::string filename;
    uint64_t tileId = 0;
    std::string layer;
};

/**
 * Metadata section of the legacy GeoJSON manifest.
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
 * Parsed legacy manifest.json structure.
 */
struct Manifest
{
    int version = 1;
    ManifestMetadata metadata;
    std::string defaultLayer = "GeoJsonAny";
    std::vector<FileEntry> files;
};

/**
 * Key for looking up legacy manifest files by tile and layer.
 */
struct TileLayerKey
{
    uint64_t tileId = 0;
    std::string layer;

    bool operator==(const TileLayerKey& other) const
    {
        return tileId == other.tileId && layer == other.layer;
    }
};

struct TileLayerKeyHash
{
    std::size_t operator()(const TileLayerKey& key) const
    {
        return std::hash<uint64_t>()(key.tileId) ^ (std::hash<std::string>()(key.layer) << 1);
    }
};

/**
 * Optional overrides for GeoJsonFolder.
 *
 * `dataSourceInfoJson` and `dataSourceInfoLocation` are mutually exclusive.
 * The location may point to a local YAML/JSON file or to an HTTP(S) URL.
 */
struct GeoJsonSourceOptions
{
    bool withAttrLayers = true;
    std::string mapId;
    std::string tilePathTemplate;
    std::optional<nlohmann::json> dataSourceInfoJson;
    std::string dataSourceInfoLocation;
};

/**
 * Optional overrides for GeoJsonEndpoint.
 *
 * `dataSourceInfoJson` and `dataSourceInfoLocation` are mutually exclusive.
 * The location may point to a local YAML/JSON file or to an HTTP(S) URL.
 */
struct GeoJsonEndpointSourceOptions
{
    std::string baseUrl;
    bool withAttrLayers = true;
    std::string mapId;
    std::string tileUrlTemplate;
    std::optional<nlohmann::json> dataSourceInfoJson;
    std::string dataSourceInfoLocation;

    /**
     * Optional text fetch override used for tests or custom transports.
     * When unset, HTTP(S) URLs are fetched via the built-in HTTP client.
     */
    std::function<std::string(std::string const&)> fetchText;
};

/**
 * Data source which loads GeoJSON tiles from a local folder.
 *
 * The datasource supports three modes:
 * - explicit `dataSourceInfo` plus `tilePathTemplate`
 * - legacy `manifest.json` filename mapping
 * - legacy directory scanning for `<tileId>.geojson`
 */
class GeoJsonSource : public mapget::DataSource
{
public:
    GeoJsonSource(
        const std::string& inputDir,
        bool withAttrLayers,
        const std::string& mapId = "");
    explicit GeoJsonSource(std::string inputDir, GeoJsonSourceOptions options);

    mapget::DataSourceInfo info() override;
    void fill(mapget::TileFeatureLayer::Ptr const&) override;
    void fill(mapget::TileSourceDataLayer::Ptr const&) override;

    [[nodiscard]] bool hasManifest() const { return hasManifest_; }
    [[nodiscard]] const Manifest& manifest() const { return manifest_; }

private:
    [[nodiscard]] bool parseManifest();
    void initFromManifest();
    void initFromDirectory();
    [[nodiscard]] std::string resolveTilePath(uint64_t tileId, std::string_view layerId) const;
    [[nodiscard]] std::string readTileBody(uint64_t tileId, std::string_view layerId) const;
    [[nodiscard]] static nlohmann::json createLayerInfoJson(const std::string& layerName);

    mapget::DataSourceInfo info_;
    std::string inputDir_;
    bool withAttrLayers_ = true;
    bool hasManifest_ = false;
    bool usesTemplatePaths_ = false;
    std::string tilePathTemplate_;
    Manifest manifest_;
    std::unordered_map<TileLayerKey, std::string, TileLayerKeyHash> tileLayerToFile_;
    std::unordered_map<std::string, std::unordered_set<uint64_t>> layerCoverage_;
};

/**
 * Data source which loads GeoJSON tiles from an HTTP endpoint.
 */
class GeoJsonEndpointSource : public mapget::DataSource
{
public:
    explicit GeoJsonEndpointSource(GeoJsonEndpointSourceOptions options);
    ~GeoJsonEndpointSource();

    mapget::DataSourceInfo info() override;
    void fill(mapget::TileFeatureLayer::Ptr const&) override;
    void fill(mapget::TileSourceDataLayer::Ptr const&) override;

private:
    [[nodiscard]] std::string renderTileUrl(uint64_t tileId, std::string_view layerId) const;
    [[nodiscard]] std::string fetchTileBody(uint64_t tileId, std::string_view layerId) const;

    mapget::DataSourceInfo info_;
    std::string baseUrl_;
    std::string tileUrlTemplate_;
    bool withAttrLayers_ = true;
    std::function<std::string(std::string const&)> fetchText_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mapget::geojsonsource
