// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "geojsonsource/geojsonsource.h"
#include "mapget/model/sourcedatalayer.h"
#include "utility.h"
#include "log.h"

#include "ndsmath/packedtileid.h"
#include "nlohmann/json.hpp"
#include "fmt/format.h"

#include <fstream>
#include <iostream>
#include <unordered_map>
#include <thread>

namespace
{

using namespace livesource;

simfil::ModelNode::Ptr jsonToMapget(  // NOLINT (recursive)
    mapget::TileFeatureLayer::Ptr const& tfl,
    const nlohmann::json& j)
{
    if (j.is_string())
        return tfl->newValue(j.get<std::string>());
    else if (j.is_number_integer())
        return tfl->newValue(j.get<int64_t>());
    else if (j.is_number_float())
        return tfl->newValue(j.get<double>());
    else if (j.is_boolean())
        return tfl->newSmallValue(j.get<bool>());
    else if (j.is_null())
        return {};
    else if (j.is_object()) {
        auto subObject = tfl->newObject(j.size());
        for (auto& el : j.items())
            subObject->addField(el.key(), jsonToMapget(tfl, el.value()));
        return subObject;
    }
    else if (j.is_array()) {
        auto subArray = tfl->newArray(j.size());
        for (auto& el : j.items())
            subArray->append(jsonToMapget(tfl, el.value()));
        return subArray;
    }

    log().debug("Unhandled JSON type: {}", j.type_name());
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
            "partId": "packedTileId",
            "description": "NDS Packed Tile ID.",
            "datatype": "U32"
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

namespace livesource
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
        log().debug("Found file {}", file.path().string());
        if (file.path().extension() == ".geojson") {
            auto packedTileId = static_cast<uint32_t>(std::stoull(file.path().stem()));
            auto tileId = packedTileIdToMapgetTileId(ndsmath::PackedTileId(packedTileId));
            coveredMapgetTileIdsToPackedTileIds_[tileId.value_] = packedTileId;
            mapget::Coverage coverage({tileId, tileId, std::vector<bool>()});
            layer->coverage_.emplace_back(coverage);
            log().debug("Added tile {}, packed tile id {}.", tileId.value_, packedTileId);
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

    log().debug("Starting... ");

    auto packedTileIdIt = coveredMapgetTileIdsToPackedTileIds_.find(tile->tileId().value_);
    if (packedTileIdIt == coveredMapgetTileIdsToPackedTileIds_.end()) {
        log().error("Tile not available: {}", tile->tileId().value_);
        return;
    }
    auto packedTileId = packedTileIdIt->second;

    // All features share the same packed tile id.
    tile->setIdPrefix({{"packedTileId", static_cast<int64_t>(packedTileId)}});

    // Parse the GeoJSON file
    auto path = fmt::format("{}/{}.geojson", inputDir_, std::to_string(packedTileId));

    log().debug("Opening: {}", path);

    std::ifstream geojsonFile(path);
    nlohmann::json geojsonData;
    geojsonFile >> geojsonData;

    log().debug("Processing {} features...", geojsonData["features"].size());

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

    log().debug("            done!");
}

}

void GeoJsonSource::fill(mapget::TileSourceDataLayer::Ptr const&)
{
    // Do nothing...
}
