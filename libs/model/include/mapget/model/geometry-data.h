#pragma once

#include "simfil/model/nodes.h"

#include <cstddef>
#include <cstdint>

#include <glm/vec3.hpp>

namespace mapget
{

enum class GeomType : uint8_t {
    Points,   // Point-cloud
    Line,     // Line-string
    Polygon,  // Auto-closed polygon
    Mesh,     // Collection of triangles
    AABB,     // Axis-aligned bounding box: [origin, size]
    GltfNodeIndex // Index into TileFeatureLayer::glbAttachmentName() plus per-node AABB bounds
};

enum class GeometryPointViewKind : uint8_t {
    RawSize,
    BoundsOrigin,
    BoundsSize,
    BoundsCorner0,
    BoundsCorner1,
    BoundsCorner2,
    BoundsCorner3,
    BoundsCorner4,
};

inline int64_t encodeGeometryHelperData(
    simfil::ModelNodeAddress baseGeometry,
    uint8_t payload = 0)
{
    return (static_cast<int64_t>(baseGeometry.column()) << 8) | payload;
}

inline uint8_t decodeGeometryHelperBaseColumn(int64_t encoded)
{
    return static_cast<uint8_t>((encoded >> 8) & 0xff);
}

inline simfil::ModelNodeAddress decodeGeometryHelperBaseAddress(
    simfil::ModelNodeAddress helperAddress,
    int64_t encoded)
{
    return {decodeGeometryHelperBaseColumn(encoded), helperAddress.index()};
}

inline GeometryPointViewKind decodeGeometryPointViewKind(int64_t encoded)
{
    return static_cast<GeometryPointViewKind>(encoded & 0xff);
}

struct GeometryViewData
{
    MODEL_COLUMN_TYPE(20);

    GeometryViewData() = default;
    GeometryViewData(
        GeomType t,
        uint32_t offset,
        uint32_t size,
        simfil::ModelNodeAddress base)
        : type_(t),
          offset_(offset),
          size_(size),
          baseGeometry_(base)
    {}

    GeomType type_ = GeomType::Points;

    // View range in base geometry point buffer.
    uint32_t offset_ = 0;
    uint32_t size_ = 0;

    // Address of referenced geometry (may itself be a view).
    simfil::ModelNodeAddress baseGeometry_{};
    simfil::ModelNodeAddress sourceDataReferences_{};
};

}  // namespace mapget
