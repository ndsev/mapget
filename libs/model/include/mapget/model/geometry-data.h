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
    Mesh      // Collection of triangles
};

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
