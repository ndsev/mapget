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

auto geoJsonLayerInfo = R"json(
{
  "featureTypes": [
    {
      "name": "AnyFeature",
      "uniqueIdCompositions": [
        [
          {
            "partId": "tileId",
            "description": "Mapget Tile ID.",
            "datatype": "U64"
          },
          {
            "partId": "featureIndex",
            "description": "Index of the feature within the GeoJSON collection.",
            "datatype": "U32"
          }
        ]
      ]
    }
  ]
})json";

}  // namespace

namespace mapget::geojsonsource
{

GeoJsonSource::GeoJsonSource(const std::string& inputDir, bool withAttrLayers, const std::string& mapId)
    : inputDir_(inputDir), withAttrLayers_(withAttrLayers)
{
    // Initialize DataSourceInfo from JSON
    info_.layers_.emplace(
        "GeoJsonAny",
        mapget::LayerInfo::fromJson(nlohmann::json::parse(geoJsonLayerInfo), "GeoJsonAny"));
    // Compromise between performance and resource usage, so that we don't overload the system.
    // TODO: Find a more sophisticated way to determine the number of parallel jobs.
    info_.maxParallelJobs_ = std::max((int)(0.33*std::thread::hardware_concurrency()), 2);
    info_.mapId_ = mapId.empty() ? mapNameFromUri(inputDir) : mapId;
    info_.nodeId_ = generateNodeHexUuid();

    // Initialize coverage
    auto layer = info_.getLayer("GeoJsonAny");
    for (const auto& file : std::filesystem::directory_iterator(inputDir)) {
        mapget::log().debug("Found file {}", file.path().string());
        if (file.path().extension() == ".geojson") {
            auto tileId = static_cast<uint64_t>(std::stoull(file.path().stem()));
            coveredMapgetTileIds_.insert(tileId);
            mapget::Coverage coverage({tileId, tileId, std::vector<bool>()});
            layer->coverage_.emplace_back(coverage);
            mapget::log().debug("Added tile {}", tileId);
        }
    }
}

mapget::DataSourceInfo GeoJsonSource::info()
{
    return info_;
}

void GeoJsonSource::fill(const mapget::TileFeatureLayer::Ptr& tile)
{
    using namespace mapget;

    mapget::log().debug("Starting... ");

    auto tileIdIt = coveredMapgetTileIds_.find(tile->tileId().value_);
    if (tileIdIt == coveredMapgetTileIds_.end()) {
        mapget::log().error("Tile not available: {}", tile->tileId().value_);
        return;
    }
    auto tileId = *tileIdIt;

    // All features share the same tile id.
    tile->setIdPrefix({{"tileId", static_cast<int64_t>(tileId)}});

    // Parse the GeoJSON file
    auto path = fmt::format("{}/{}.geojson", inputDir_, std::to_string(tileId));

    mapget::log().debug("Opening: {}", path);

    std::ifstream geojsonFile(path);
    nlohmann::json geojsonData;
    geojsonFile >> geojsonData;

    mapget::log().debug("Processing {} features...", geojsonData["features"].size());

    // Iterate over each feature in the GeoJSON data
    int featureId = 0;  // Initialize the running index
    for (auto& feature_data : geojsonData["features"]) {
        // Create a new feature
        auto feature = tile->newFeature("AnyFeature", {{"featureIndex", featureId}});
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
