#pragma once

#include "geometry.h"
#include "sourcedatareference.h"

namespace mapget
{

class Geometry;

/**
 * Represents an attribute or relation validity with respect to a feature's geometry.
 */
class Validity : public simfil::ProceduralObject<6, Validity, TileFeatureLayer>
{
    friend class TileFeatureLayer;
    template <typename>
    friend struct simfil::model_ptr;
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
     * If the offset type is 1D, i.e. BufferOffset/RelativeLengthOffset/MetricLengthOffset,
     * then the x component of the returned point reflects the used value.
     */
    void setOffsetPoint(Point pos);
    void setOffsetPoint(GeometryOffsetType offsetType, double pos);
    [[nodiscard]] std::optional<Point> offsetPoint() const;

    /**
     * Offset range accessors. Note for the getter:
     * If the offset type is 1D, i.e. BufferOffset/RelativeLengthOffset/MetricLengthOffset,
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

    /**
     * Compute the actual shape-points of the validity with respect to one
     * of the geometries in the given collection. The geometry is picked based
     * on the validity's geometryName. The return value may be one of the following:
     * - An empty vector, indicating that the validity could not be applied.
     *   If an error string was passed, then it would be set to an error message.
     * - A vector containing a single point, if the validity resolved to a point geometry.
     * - A vector containing more than one point, if the validity resolved to a poly-line.
     */
     SelfContainedGeometry computeGeometry(model_ptr<GeometryCollection> const& geometryCollection, std::string* error=nullptr) const;

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
                case MetricLengthOffset:
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
struct MultiValidity : public simfil::BaseArray<TileFeatureLayer, Validity>
{
    friend class TileFeatureLayer;
    template <typename>
    friend struct simfil::model_ptr;

    /**
     * Append a new line position validity based on an absolute geographic position.
     */
    model_ptr<Validity> newPoint(
        Point pos,
        std::string_view geomName = {},
        Validity::Direction direction = Validity::Empty);

    /**
     * Append a new line range validity based on absolute geographic positions.
     */
    model_ptr<Validity> newRange(
        Point start,
        Point end,
        std::string_view geomName = {},
        Validity::Direction direction = Validity::Empty);

    /**
     * Append a new line position validity based on a fractional offset.
     * Examples:
     *   (RelativeLengthOffset, 0.1) -> Validity at 10% linestring length along the digitization direction.
     *   (MetricLengthOffset, 31.1) -> Validity at 31.1m along the digitization direction.
     */
    model_ptr<Validity> newPoint(
        Validity::GeometryOffsetType offsetType,
        double pos,
        std::string_view geomName = {},
        Validity::Direction direction = Validity::Empty);

    /**
     * Append a new line position validity based on an integer offset.
     * Examples:
     *   (BufferOffset, 0) -> Validity at point #0 in the referenced geometry.
     */
    model_ptr<Validity> newPoint(
        Validity::GeometryOffsetType offsetType,
        int32_t pos,
        std::string_view geomName = {},
        Validity::Direction direction = Validity::Empty);

    /**
     * Append a new line range validity based on fractional offsets.
     * Examples:
     *   (RelativeLengthOffset, 0.1, 0.5) -> Validity from 10% to 50% linestring length along the digitization direction.
     *   (MetricLengthOffset, 31.1, 57.6) -> Validity from 31.1m to 57.6m along the digitization direction
     */
    model_ptr<Validity> newRange(
        Validity::GeometryOffsetType offsetType,
        double start,
        double end,
        std::string_view geomName = {},
        Validity::Direction direction = Validity::Empty);

    /**
     * Append a new line range validity based on integer offsets.
     * Examples:
     *   (BufferOffset, 5, 7) -> Validity from point #5 to #7 (inclusive) in the referenced geometry.
     */
    model_ptr<Validity> newRange(
        Validity::GeometryOffsetType offsetType,
        int32_t start,
        int32_t end,
        std::string_view geomName = {},
        Validity::Direction direction = Validity::Empty);

    /**
     * Append an arbitrary validity geometry. Note: You may use non-line geometries here.
     */
    model_ptr<Validity>
    newGeometry(model_ptr<Geometry>, Validity::Direction direction = Validity::Empty);

    /**
     * Append a direction validity without further restricting the range.
     * The direction value controls, in which direction along the referenced
     * geometry the attribute applies. Positive means "in digitization direction",
     * "negative" means opposite.
     */
    model_ptr<Validity> newDirection(Validity::Direction direction = Validity::Empty);

private:
    using simfil::BaseArray<TileFeatureLayer, Validity>::BaseArray;
};

}