// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "geojsonsource/geojsonsource.h"

#include "mapget/log.h"
#include "mapget/model/sourcedatalayer.h"

#include "nlohmann/json.hpp"
#include "fmt/format.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unordered_map>

namespace
{

simfil::ModelNode::Ptr jsonToMapget(  // NOLINT (recursive)
    mapget::TileFeatureLayer::Ptr const& tfl,
    const nlohmann::json& j)
{
    if (j.is_string())
        return tfl->newValue(j.get<std::string>());
    if (j.is_number_integer())
        return tfl->newValue(j.get<int64_t>());
    if (j.is_number_float())
        return tfl->newValue(j.get<double>());
    if (j.is_boolean())
        return tfl->newSmallValue(j.get<bool>());
    if (j.is_null())
        return {};
    if (j.is_object()) {
        auto subObject = tfl->newObject(j.size());
        for (auto& el : j.items())
            subObject->addField(el.key(), jsonToMapget(tfl, el.value()));
        return subObject;
    }
    if (j.is_array()) {
        auto subArray = tfl->newArray(j.size());
        for (auto& el : j.items())
            subArray->append(jsonToMapget(tfl, el.value()));
        return subArray;
    }

    mapget::log().debug("Unhandled JSON type: {}", j.type_name());
    return {};
}

constexpr auto manifestFilename = "manifest.json";

}  // namespace

namespace mapget::geojsonsource
{

nlohmann::json GeoJsonSource::createLayerInfoJson(const std::string& layerName)
{
    // Create feature type name from layer name (e.g., "Road" -> "RoadFeature")
    std::string featureTypeName = layerName;
    if (layerName != "GeoJsonAny") {
        featureTypeName = layerName + "Feature";
    } else {
        featureTypeName = "AnyFeature";
    }

    return nlohmann::json::parse(fmt::format(R"json(
{{
  "featureTypes": [
    {{
      "name": "{}",
      "uniqueIdCompositions": [
        [
          {{
            "partId": "tileId",
            "description": "Mapget Tile ID.",
            "datatype": "U64"
          }},
          {{
            "partId": "featureIndex",
            "description": "Index of the feature within the GeoJSON collection.",
            "datatype": "U32"
          }}
        ]
      ]
    }}
  ]
}})json", featureTypeName));
}

bool GeoJsonSource::parseManifest()
{
    auto manifestPath = std::filesystem::path(inputDir_) / manifestFilename;
    if (!std::filesystem::exists(manifestPath)) {
        return false;
    }

    try {
        std::ifstream manifestFile(manifestPath);
        nlohmann::json manifestJson;
        manifestFile >> manifestJson;

        // Parse version (required)
        manifest_.version = manifestJson.value("version", 1);

        // Parse metadata (optional)
        if (manifestJson.contains("metadata")) {
            auto& meta = manifestJson["metadata"];
            if (meta.contains("name"))
                manifest_.metadata.name = meta["name"].get<std::string>();
            if (meta.contains("description"))
                manifest_.metadata.description = meta["description"].get<std::string>();
            if (meta.contains("source"))
                manifest_.metadata.source = meta["source"].get<std::string>();
            if (meta.contains("created"))
                manifest_.metadata.created = meta["created"].get<std::string>();
            if (meta.contains("author"))
                manifest_.metadata.author = meta["author"].get<std::string>();
            if (meta.contains("license"))
                manifest_.metadata.license = meta["license"].get<std::string>();
        }

        // Parse index (optional - if missing, will fall back to directory scan)
        if (manifestJson.contains("index")) {
            auto& index = manifestJson["index"];

            // Default layer name
            manifest_.defaultLayer = index.value("defaultLayer", "GeoJsonAny");

            // Parse files
            if (index.contains("files")) {
                for (auto& [filename, fileInfo] : index["files"].items()) {
                    FileEntry entry;
                    entry.filename = filename;

                    if (fileInfo.is_object()) {
                        // Full format: { "tileId": 123, "layer": "Road" }
                        entry.tileId = fileInfo.value("tileId", uint64_t{0});
                        entry.layer = fileInfo.value("layer", std::string{});
                    } else if (fileInfo.is_number()) {
                        // Short format: just the tile ID
                        entry.tileId = fileInfo.get<uint64_t>();
                    } else {
                        mapget::log().warn(
                            "Invalid file entry in manifest for '{}': expected object or number",
                            filename);
                        continue;
                    }

                    // Use default layer if not specified
                    if (entry.layer.empty()) {
                        entry.layer = manifest_.defaultLayer;
                    }

                    // Validate file exists
                    auto filePath = std::filesystem::path(inputDir_) / filename;
                    if (!std::filesystem::exists(filePath)) {
                        mapget::log().warn(
                            "File '{}' listed in manifest does not exist, skipping",
                            filename);
                        continue;
                    }

                    manifest_.files.push_back(std::move(entry));
                }
            }
        }

        mapget::log().info(
            "Loaded manifest.json with {} file entries",
            manifest_.files.size());

        return true;

    } catch (const std::exception& e) {
        mapget::log().error("Failed to parse manifest.json: {}", e.what());
        return false;
    }
}

void GeoJsonSource::initFromManifest()
{
    // Build layer coverage and file mapping from manifest entries
    for (const auto& entry : manifest_.files) {
        // Track coverage per layer
        layerCoverage_[entry.layer].insert(entry.tileId);

        // Map (tileId, layer) -> filename
        TileLayerKey key{entry.tileId, entry.layer};
        tileLayerToFile_[key] = entry.filename;

        mapget::log().debug(
            "Registered file '{}' -> tile {} in layer '{}'",
            entry.filename, entry.tileId, entry.layer);
    }

    // Create LayerInfo for each discovered layer
    for (const auto& [layerName, tileIds] : layerCoverage_) {
        auto layerJson = createLayerInfoJson(layerName);
        auto layerInfo = mapget::LayerInfo::fromJson(layerJson, layerName);

        // Add coverage entries
        for (uint64_t tileId : tileIds) {
            mapget::Coverage coverage({tileId, tileId, std::vector<bool>()});
            layerInfo->coverage_.emplace_back(coverage);
        }

        info_.layers_.emplace(layerName, layerInfo);
        mapget::log().info(
            "Layer '{}' initialized with {} tiles",
            layerName, tileIds.size());
    }
}

void GeoJsonSource::initFromDirectory()
{
    // Legacy mode: scan for <tile-id>.geojson files
    const std::string defaultLayer = "GeoJsonAny";
    auto layerJson = createLayerInfoJson(defaultLayer);
    auto layerInfo = mapget::LayerInfo::fromJson(layerJson, defaultLayer);

    for (const auto& file : std::filesystem::directory_iterator(inputDir_)) {
        mapget::log().debug("Found file {}", file.path().string());
        if (file.path().extension() == ".geojson") {
            try {
                auto tileId = static_cast<uint64_t>(std::stoull(file.path().stem()));
                layerCoverage_[defaultLayer].insert(tileId);

                TileLayerKey key{tileId, defaultLayer};
                tileLayerToFile_[key] = file.path().filename().string();

                mapget::Coverage coverage({tileId, tileId, std::vector<bool>()});
                layerInfo->coverage_.emplace_back(coverage);
                mapget::log().debug("Added tile {}", tileId);
            } catch (const std::exception& e) {
                mapget::log().debug(
                    "Skipping file '{}': filename is not a valid tile ID",
                    file.path().filename().string());
            }
        }
    }

    info_.layers_.emplace(defaultLayer, layerInfo);
}

GeoJsonSource::GeoJsonSource(const std::string& inputDir, bool withAttrLayers, const std::string& mapId)
    : inputDir_(inputDir), withAttrLayers_(withAttrLayers)
{
    // Compromise between performance and resource usage
    info_.maxParallelJobs_ = std::max((int)(0.33*std::thread::hardware_concurrency()), 2);
    info_.mapId_ = mapId.empty() ? mapNameFromUri(inputDir) : mapId;
    info_.nodeId_ = generateNodeHexUuid();

    // Try to load manifest.json first
    hasManifest_ = parseManifest();

    if (hasManifest_ && !manifest_.files.empty()) {
        // Use manifest-based initialization
        initFromManifest();
    } else {
        // Fallback to directory scanning
        if (!hasManifest_) {
            mapget::log().warn(
                "No manifest.json found in '{}'. "
                "Falling back to filename-based tile ID detection. "
                "Consider adding a manifest.json for better control over file mapping and layers.",
                inputDir);
        } else {
            mapget::log().info(
                "manifest.json found but has no index/files section, scanning directory");
        }
        initFromDirectory();
    }

    // Log summary
    size_t totalTiles = 0;
    for (const auto& [layer, tileIds] : layerCoverage_) {
        totalTiles += tileIds.size();
    }
    mapget::log().info(
        "GeoJsonSource initialized: {} layers, {} total tile entries",
        info_.layers_.size(), totalTiles);
}

mapget::DataSourceInfo GeoJsonSource::info()
{
    return info_;
}

void GeoJsonSource::fill(const mapget::TileFeatureLayer::Ptr& tile)
{
    using namespace mapget;

    auto tileId = tile->tileId().value_;
    auto layerName = tile->layerInfo()->layerId_;

    mapget::log().debug("Filling tile {} for layer '{}'", tileId, layerName);

    // Look up the file for this (tileId, layer) combination
    TileLayerKey key{tileId, layerName};
    auto fileIt = tileLayerToFile_.find(key);
    if (fileIt == tileLayerToFile_.end()) {
        mapget::log().error(
            "No file registered for tile {} in layer '{}'",
            tileId, layerName);
        return;
    }

    // All features share the same tile id
    tile->setIdPrefix({{"tileId", static_cast<int64_t>(tileId)}});

    // Build the full path
    auto path = (std::filesystem::path(inputDir_) / fileIt->second).string();

    mapget::log().debug("Opening: {}", path);

    std::ifstream geojsonFile(path);
    if (!geojsonFile) {
        mapget::log().error("Failed to open file: {}", path);
        return;
    }

    nlohmann::json geojsonData;
    geojsonFile >> geojsonData;

    mapget::log().debug("Processing {} features...", geojsonData["features"].size());

    // Get the feature type name for this layer
    std::string featureTypeName = (layerName == "GeoJsonAny") ? "AnyFeature" : (layerName + "Feature");

    // Iterate over each feature in the GeoJSON data
    int featureId = 0;
    for (auto& feature_data : geojsonData["features"]) {
        // Create a new feature
        auto feature = tile->newFeature(featureTypeName, {{"featureIndex", featureId}});
        featureId++;

        // Get geometry data
        auto geometry = feature_data["geometry"];
        if (geometry["type"] == "Point") {
            auto coordinates = geometry["coordinates"];
            feature->addPoint({coordinates[0], coordinates[1]});
        }
        else if (geometry["type"] == "LineString") {
            auto line = feature->geom()->newGeometry(GeomType::Line, 2);
            for (auto& coordinates : geometry["coordinates"]) {
                line->append({coordinates[0], coordinates[1]});
            }
        }

        // Add top-level properties as attributes
        for (auto& property : feature_data["properties"].items()) {

            // Always emit scalar properties
            if (!property.value().is_object()) {
                feature->attributes()->addField(property.key(), jsonToMapget(tile, property.value()));
                continue;
            }

            // Emit layers conditionally
            if (!withAttrLayers_)
                continue;

            // If the property value is an object, add an attribute layer
            auto attrLayer = feature->attributeLayers()->newLayer(property.key());
            for (auto& attr : property.value().items()) {
                auto attribute = attrLayer->newAttribute(attr.key());
                attribute->addField(attr.key(), jsonToMapget(tile, attr.value()));

                // Check if the value is an object and contains a "_direction" field
                if (attr.value().is_object() && attr.value().contains("_direction")) {
                    std::string dir = attr.value()["_direction"];
                    auto validDir = dir == "POSITIVE" ? Validity::Positive :
                                    dir == "NEGATIVE" ? Validity::Negative :
                                    dir == "BOTH"     ? Validity::Both :
                                                        Validity::Empty;
                    attribute->validity()->newDirection(validDir);
                }
            }
        }
    }

    mapget::log().debug("            done!");
}

void GeoJsonSource::fill(mapget::TileSourceDataLayer::Ptr const&)
{
    // Do nothing...
}

}  // namespace mapget::geojsonsource
