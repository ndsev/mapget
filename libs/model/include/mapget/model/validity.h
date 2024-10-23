#pragma once

#include "geometry.h"
#include "sourcedatareference.h"

namespace mapget
{

class Geometry;

/**
 * Represents an attribute validity which belongs to an
 * Attribute, and may have .... (TODO)
 *
 * ~ Attribute
 *   + *Validity -> Also use for Relations
 *     + enum GeometryDescriptionType
 *       + Geometry = 0
 *       + GeoPosOffset = 1
 *       + BufferOffset = 2
 *       + RelLengthOffset = 3
 *       + AbsLengthOffset = 4
 *       + Point = 0 << 8
 *       + Range = 1 << 8
 *     + using GeometryDescription = std::variant<Range, Point, StringId, ModelNodeAddress>;
 *     + using Range = std::pair<Point, Point>
 * ~ Geometry
 *   + name: StringId
 */
class Validity : public simfil::ProceduralObject<2, Validity>
{
    friend class TileFeatureLayer;
    template <typename>
    friend struct simfil::shared_model_ptr;

public:
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
     * Validity geometry type enumeration. PointValidity and RangeValidity
     * may be combined with one of GeoPosOffset, BufferOffset, RelativeLengthOffset
     * or AbsoluteLengthOffset. In this case, the validity geometry is based on
     * an offset (range) of a feature's geometry. If SimpleGeometry is used,
     * then the validity just references a whole Geometry object.
     */
    enum GeometryDescriptionType : uint16_t {
        SimpleGeometry = 0,
        GeoPosOffset = 1,
        BufferOffset = 2,
        RelativeLengthOffset = 3,
        AbsoluteLengthOffset = 4,
        PointValidity = 0 << 8,
        RangeValidity = 1 << 8,
    };

    /**
     * Attribute direction accessors.
     */
    [[nodiscard]] Direction direction() const;
    void setDirection(Direction const& v);

protected:
    /** Actual per-attribute data that is stored in the model's attributes-column. */
    struct Data
    {
        using Range = std::pair<Point, Point>;
        using GeometryDescription = std::variant<ModelNodeAddress, Range, Point>;

        Direction direction_;
        GeometryDescriptionType geomDescrType_ = SimpleGeometry;
        GeometryDescription geomDescr_;
        StringId referencedGeomName_ = 0;

        template<typename T, typename... Types>
        static T& get_or_default_construct(std::variant<Types...>& v) {
            if (!std::holds_alternative<T>(v)) {
                v.template emplace<T>();
            }
            return std::get<T>(v);
        }

        template <typename S>
        void serialize(S& s)
        {
            s.value1b(direction_);
            s.value2b(geomDescrType_);
            auto pointOrRange = geomDescrType_ & 0xff00;
            auto offsetType = geomDescrType_ & 0xff;

            if (offsetType == SimpleGeometry ) {
                s.object(get_or_default_construct<ModelNodeAddress>(geomDescr_));
                return;
            }

            // The referenced geometry name is only used if the validity
            // does not directly reference a geometry by a ModelNodeAddress.
            s.value2b(referencedGeomName_);

            auto serializeOffsetPoint = [&s, &offsetType](Point& p) {
                switch (offsetType) {
                case GeoPosOffset:
                    s.object(p);
                    break;
                case BufferOffset:
                case RelativeLengthOffset:
                case AbsoluteLengthOffset:
                    s.value4b(p.x);
                    break;
                }
            };

            if (pointOrRange == RangeValidity) {
                auto& [start, end] = get_or_default_construct<Range>(geomDescr_);
                serializeOffsetPoint(start);
                serializeOffsetPoint(end);
            }
            else {
                serializeOffsetPoint(get_or_default_construct<Point>(geomDescr_))
            }
        }
    };

    Validity(Data* data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);
    Validity() = default;

    /**
     * Pointer to the actual data stored for the attribute.
     */
    Data* data_ = nullptr;
};

}