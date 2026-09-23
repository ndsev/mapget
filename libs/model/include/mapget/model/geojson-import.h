#pragma once

#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace mapget
{

class PartitionFeatureLayer;

/**
 * Controls whether GeoJSON import behaves like a strict mapget roundtrip
 * reader or as a permissive adapter for generic external GeoJSON.
 */
struct GeoJsonImportOptions
{
    /** Require mapget-style structure and reject lossy or ambiguous input. */
    bool strict_ = true;

    /** Feature type to assume when the GeoJSON feature does not carry `typeId`. */
    std::optional<std::string> fallbackFeatureType_;

    /**
     * In best-effort mode, treat object-valued entries under `properties`
     * or its `attributes` alias as attribute-layer payloads instead of plain
     * JSON attributes.
     */
    bool objectPropertiesAsAttributeLayers_ = false;
};

/**
 * Import a FeatureCollection into an empty PartitionFeatureLayer.
 *
 * Depending on `options`, this either expects mapget's JSON profile or
 * performs best-effort adaptation from generic GeoJSON.
 */
void importGeoJson(
    PartitionFeatureLayer& tile,
    nlohmann::json const& geoJson,
    GeoJsonImportOptions const& options = {});
}
