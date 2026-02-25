#pragma once

#include "point.h"

#include "simfil/model/nodes.h"

#include <cstddef>
#include <cstdint>

namespace mapget
{

enum class GeomType : uint8_t {
    Points,   // Point-cloud
    Line,     // Line-string
    Polygon,  // Auto-closed polygon
    Mesh      // Collection of triangles
};

struct GeometryData
{
    MODEL_COLUMN_TYPE(48);

    GeometryData() = default;
    GeometryData(GeomType t, size_t capacity) : isView_(false), type_(t) {
        detail_.geom_.vertexArray_ = -(simfil::ArrayIndex)capacity;
    }
    GeometryData(GeomType t, uint32_t offset, uint32_t size, simfil::ModelNodeAddress base)
        : isView_(true), type_(t) {
        detail_.view_.offset_ = offset;
        detail_.view_.size_ = size;
        detail_.view_.baseGeometry_ = base;
    }

    // Flag to indicate whether this geometry is just
    // a view into another geometry object.
    bool isView_ = false;

    // Geometry type. A view can have a different geometry type
    // than the base geometry.
    GeomType type_ = GeomType::Points;

    // Geometry reference name if applicable.
    simfil::StringId geomName_ = 0;

    union GeomDetails
    {
        GeomDetails() {new(&geom_) GeomBaseDetails();}

        struct GeomBaseDetails {
            // Vertex array index, or negative requested initial
            // capacity, if no point is added yet.
            simfil::ArrayIndex vertexArray_ = -1;

            // Offset is set when vertexArray is allocated,
            // which happens when the first point is added.
            Point offset_;
        } geom_;

        struct GeomViewDetails {
            // If this geometry is a view, then it references
            // a range of vertices in another geometry.

            // Offset within the other geometry.
            uint32_t offset_ = 0;

            // Number of referenced vertices.
            uint32_t size_ = 0;

            // Address of the referenced geometry - may be a view itself.
            simfil::ModelNodeAddress baseGeometry_;
        } view_;
    } detail_;

    simfil::ModelNodeAddress sourceDataReferences_;
};

}  // namespace mapget
