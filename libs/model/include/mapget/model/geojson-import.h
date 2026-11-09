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
    /** Require mapget ID fields, validate supplied partition metadata and decode
     * MultiPolygon as a triangle mesh. This covers the supported mapget profile;
     * unknown fields are not exhaustively rejected and GLTF import is unsupported.
     */
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
 * performs best-effort adaptation from generic GeoJSON. The layer must also have
 * no ID prefix. Features/geometry precede shared sequences, relations, properties
 * and attribute validities so compact references can resolve their dependencies.
 * Errors throw after any preceding mutations; discard the target on failure.
 * GeometryCollection expansion is one level; nested collections are unsupported.
 */
void importGeoJson(
    PartitionFeatureLayer& tile,
    nlohmann::json const& geoJson,
    GeoJsonImportOptions const& options = {});
}
