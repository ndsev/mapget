#pragma once

#include "geometry.h"
#include "sourcedatareference.h"

namespace mapget
{

class Geometry;

/**
 * Represents an attribute validity which belongs to an
 * Attribute, and may have .... (TODO)
 * + *Validity -> Also use for Relations
 *   + computeGeometry(Feature)
 * ~ Geometry
 *   + name: StringId
 *   + name(): string_view
 *   + setName(): string_view
 * ~ ValidityCollection
*   + newValidity(Point pos, std::string_view geomName={}, Direction = Empty)
*   + newValidity(Point start, Point end, std::string_view geomName={}, Direction = Empty)
*   + newValidity(offsetType, double pos, std::string_view geomName={}, Direction = Empty)
*   + newValidity(offsetType, double start, double end, std::string_view geomName={}, Direction = Empty)
*   + newValidity(model_ptr<Geometry>)
*   + newValidity(Direction = Empty)
 */
class Validity : public simfil::ProceduralObject<2, Validity, TileFeatureLayer>
{
    friend class TileFeatureLayer;
    template <typename>
    friend struct simfil::shared_model_ptr;
    friend class PointNode;

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
     * Validity offset type enumeration. OffsetPointValidity and OffsetRangeValidity
     * may be combined with one of GeoPosOffset, BufferOffset, RelativeLengthOffset
     * or AbsoluteLengthOffset. In this case, the validity geometry is based on
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
        AbsoluteLengthOffset = 4,
    };

    /**
     * Validity direction accessors.
     */
    [[nodiscard]] Direction direction() const;
    void setDirection(Direction const& v);

    /**
     * Read-only validity type info accessors.
     */
    [[nodiscard]] GeometryOffsetType geometryOffsetType() const;
    [[nodiscard]] GeometryDescriptionType geometryDescriptionType() const;

    /**
     * Referenced geometry name accessors.
     */
    [[nodiscard]] std::optional<std::string_view> geometryName() const;
    void setGeometryName(std::optional<std::string_view> const& geometryName);

    /**
     * Single offset point accessors. Note for the getter:
     * If the offset type is 1D, i.e. BufferOffset/RelativeLengthOffset/AbsoluteLengthOffset,
     * then the x component of the returned point reflects the used value.
     */
    void setOffsetPoint(Point pos);
    void setOffsetPoint(GeometryOffsetType offsetType, double pos);
    [[nodiscard]] std::optional<Point> offsetPoint() const;

    /**
     * Offset range accessors. Note for the getter:
     * If the offset type is 1D, i.e. BufferOffset/RelativeLengthOffset/AbsoluteLengthOffset,
     * then the x components of the returned points reflect the used values.
     */
    void setOffsetRange(Point start, Point end);
    void setOffsetRange(GeometryOffsetType offsetType, double start, double end);
    [[nodiscard]] std::optional<std::pair<Point, Point>> offsetRange() const;

    /**
     * Get or set a simple geometry for the validity.
     */
    void setSimpleGeometry(model_ptr<Geometry>);
    [[nodiscard]] model_ptr<Geometry> simpleGeometry() const;

protected:
    /** Actual per-validity data that is stored in the model's attributes-column. */
    struct Data
    {
        using Range = std::pair<Point, Point>;
        using GeometryDescription = std::variant<std::monostate, ModelNodeAddress, Range, Point>;

        Direction direction_;
        GeometryDescriptionType geomDescrType_ = NoGeometry;
        GeometryOffsetType geomOffsetType_ = InvalidOffsetType;
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
            s.value1b(geomDescrType_);
            s.value1b(geomOffsetType_);

            if (geomDescrType_ == SimpleGeometry) {
                assert(geomOffsetType_ == InvalidOffsetType);
                s.object(get_or_default_construct<ModelNodeAddress>(geomDescr_));
                return;
            }

            // The referenced geometry name is only used if the validity
            // does not directly reference a geometry by a ModelNodeAddress.
            s.value2b(referencedGeomName_);

            auto serializeOffsetPoint = [this, &s](Point& p) {
                switch (geomOffsetType_) {
                case InvalidOffsetType:
                    break;
                case GeoPosOffset:
                    s.object(p);
                    break;
                case BufferOffset:
                case RelativeLengthOffset:
                case AbsoluteLengthOffset:
                    s.value8b(p.x);
                    break;
                }
            };

            if (geomDescrType_ == OffsetRangeValidity) {
                auto& [start, end] = get_or_default_construct<Range>(geomDescr_);
                serializeOffsetPoint(start);
                serializeOffsetPoint(end);
            }
            else if (geomDescrType_ == OffsetPointValidity) {
                serializeOffsetPoint(get_or_default_construct<Point>(geomDescr_));
            }
        }
    };

protected:
    Validity(Data* data, simfil::ModelConstPtr layer, simfil::ModelNodeAddress a);
    Validity() = default;

    /**
     * Pointer to the actual data stored for the attribute.
     */
    Data* data_ = nullptr;
};

/**
 * Array of Validity objects with convenience constructors.
 */
struct ValidityCollection : public simfil::BaseArray<TileFeatureLayer, Validity>
{
    friend class TileFeatureLayer;
    template <typename>
    friend struct simfil::shared_model_ptr;

    model_ptr<Validity> newValidity(Point pos, std::string_view geomName={}, Validity::Direction direction = Validity::Empty);
    model_ptr<Validity> newValidity(Point start, Point end, std::string_view geomName={}, Validity::Direction direction = Validity::Empty);
    model_ptr<Validity> newValidity(Validity::GeometryOffsetType offsetType, double pos, std::string_view geomName={}, Validity::Direction direction = Validity::Empty);
    model_ptr<Validity> newValidity(Validity::GeometryOffsetType offsetType, double start, double end, std::string_view geomName={}, Validity::Direction direction = Validity::Empty);
    model_ptr<Validity> newValidity(model_ptr<Geometry>, Validity::Direction direction = Validity::Empty);
    model_ptr<Validity> newValidity(Validity::Direction direction = Validity::Empty);

private:
    using simfil::BaseArray<TileFeatureLayer, Validity>::BaseArray;
};

}