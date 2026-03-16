#pragma once

#include "point.h"

#include "simfil/model/nodes.h"

#include <cstdint>

namespace mapget
{

struct ValidityData
{
    MODEL_COLUMN_TYPE(64);

    /**
     * Validity direction values - may be used as flags.
     */
    enum Direction : uint8_t {
        Empty = 0x0,     // No set direction
        Positive = 0x1,  // Positive (digitization) direction
        Negative = 0x2,  // Negative (against digitization) direction
        Both = 0x3,      // Both positive and negative direction
        None = 0x4,      // Not in any direction
    };

    /**
     * Validity offset type enumeration. OffsetPointValidity and OffsetRangeValidity
     * may be combined with one of GeoPosOffset, BufferOffset, RelativeLengthOffset
     * or MetricLengthOffset. In this case, the validity geometry is based on
     * an offset (range) of a feature's geometry. If SimpleGeometry is used,
     * then the validity just references a whole Geometry object.
     */
    enum GeometryDescriptionType : uint8_t {
        NoGeometry = 0,
        SimpleGeometry = 1,
        OffsetPointValidity = 2,
        OffsetRangeValidity = 3,
    };
    enum GeometryOffsetType : uint8_t {
        InvalidOffsetType = 0,
        GeoPosOffset = 1,
        BufferOffset = 2,
        RelativeLengthOffset = 3,
        MetricLengthOffset = 4,
    };

    struct Range {
        Point first;
        Point second;
    };

    union GeometryDescription {
        GeometryDescription() : simpleGeometry_() {}
        simfil::ModelNodeAddress simpleGeometry_;
        Range range_;
        Point point_;
    };

    Direction direction_ = Empty;
    GeometryDescriptionType geomDescrType_ = NoGeometry;
    GeometryOffsetType geomOffsetType_ = InvalidOffsetType;
    GeometryDescription geomDescr_{};
    static constexpr int8_t InvalidReferencedStage = -1;
    int8_t referencedStage_ = InvalidReferencedStage;
    simfil::ModelNodeAddress featureAddress_;
};

}  // namespace mapget
