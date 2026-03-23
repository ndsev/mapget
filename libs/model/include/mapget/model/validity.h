#pragma once

#include "geometry.h"
#include "sourcedatareference.h"
#include "validity-data.h"

namespace mapget
{

class Geometry;
class Feature;
class FeatureId;

/**
 * Represents an attribute or relation validity with respect to a feature's geometry.
 */
class Validity : public simfil::ProceduralObject<7, Validity, TileFeatureLayer>
{
    friend class TileFeatureLayer;
    friend class PointNode;

public:
    using Direction = ValidityData::Direction;
    using GeometryDescriptionType = ValidityData::GeometryDescriptionType;
    using GeometryOffsetType = ValidityData::GeometryOffsetType;
    using TransitionEnd = ValidityData::TransitionEnd;

    // Keep existing Validity::Empty-style API surface.
    static constexpr Direction Empty = ValidityData::Empty;
    static constexpr Direction Positive = ValidityData::Positive;
    static constexpr Direction Negative = ValidityData::Negative;
    static constexpr Direction Both = ValidityData::Both;
    static constexpr Direction None = ValidityData::None;

    static constexpr GeometryDescriptionType NoGeometry = ValidityData::NoGeometry;
    static constexpr GeometryDescriptionType SimpleGeometry = ValidityData::SimpleGeometry;
    static constexpr GeometryDescriptionType OffsetPointValidity = ValidityData::OffsetPointValidity;
    static constexpr GeometryDescriptionType OffsetRangeValidity = ValidityData::OffsetRangeValidity;
    static constexpr GeometryDescriptionType FeatureTransition = ValidityData::FeatureTransition;

    static constexpr GeometryOffsetType InvalidOffsetType = ValidityData::InvalidOffsetType;
    static constexpr GeometryOffsetType GeoPosOffset = ValidityData::GeoPosOffset;
    static constexpr GeometryOffsetType BufferOffset = ValidityData::BufferOffset;
    static constexpr GeometryOffsetType RelativeLengthOffset = ValidityData::RelativeLengthOffset;
    static constexpr GeometryOffsetType MetricLengthOffset = ValidityData::MetricLengthOffset;

    static constexpr TransitionEnd Start = ValidityData::Start;
    static constexpr TransitionEnd End = ValidityData::End;

    /**
     * Feature on which the validity applies.
     */
    [[nodiscard]] model_ptr<FeatureId> featureId() const;
    void setFeatureId(model_ptr<FeatureId> feature);

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
     * Referenced geometry stage accessors.
     * If unset, validity resolution falls back to the first matching line geometry.
     */
    [[nodiscard]] std::optional<uint32_t> geometryStage() const;
    void setGeometryStage(std::optional<uint32_t> geometryStage);

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
     * Get or set a semantic feature transition validity.
     * The connected ends indicate which endpoint of each referenced feature touches the transition.
     */
    void setFeatureTransition(
        model_ptr<Feature> const& fromFeature,
        TransitionEnd fromConnectedEnd,
        model_ptr<Feature> const& toFeature,
        TransitionEnd toConnectedEnd,
        uint32_t transitionNumber);
    [[nodiscard]] model_ptr<Feature> transitionFromFeature() const;
    [[nodiscard]] model_ptr<Feature> transitionToFeature() const;
    [[nodiscard]] std::optional<TransitionEnd> transitionFromConnectedEnd() const;
    [[nodiscard]] std::optional<TransitionEnd> transitionToConnectedEnd() const;
    [[nodiscard]] std::optional<uint32_t> transitionNumber() const;

    /**
     * Compute the actual shape-points of the validity with respect to one
     * of the geometries in the given collection, or the geometry collection of
     * the directly referenced feature. The geometry is picked based
     * on the validity's geometryStage when available. The return value may be one of the following:
     * - An empty vector, indicating that the validity could not be applied.
     *   If an error string was passed, then it would be set to an error message.
     * - A vector containing a single point, if the validity resolved to a point geometry.
     * - A vector containing more than one point, if the validity resolved to a poly-line.
     */
     SelfContainedGeometry computeGeometry(model_ptr<GeometryCollection> geometryCollection, std::string* error=nullptr) const;

protected:
    using Data = ValidityData;

public:
    explicit Validity(simfil::detail::mp_key key)
        : simfil::ProceduralObject<7, Validity, TileFeatureLayer>(key) {}
    Validity(Direction direction,
             simfil::ModelConstPtr layer,
             simfil::ModelNodeAddress a,
             simfil::detail::mp_key key);
    Validity(Data* data,
             simfil::ModelConstPtr layer,
             simfil::ModelNodeAddress a,
             simfil::detail::mp_key key);
    Validity() = delete;

protected:
    /**
     * Pointer to the actual data stored for the attribute.
     */
    void ensureMaterialized();

    Data* data_ = nullptr;
    Direction simpleDirection_ = Empty;
};

/**
 * Array of Validity objects with convenience constructors.
 */
struct MultiValidity : public simfil::BaseArray<TileFeatureLayer, Validity>
{
    friend class TileFeatureLayer;

    /**
     * Append a new line position validity based on an absolute geographic position.
     */
    model_ptr<Validity> newPoint(
        Point pos,
        std::optional<uint32_t> geometryStage = std::nullopt,
        Validity::Direction direction = Validity::Empty);

    /**
     * Append a new line range validity based on absolute geographic positions.
     */
    model_ptr<Validity> newRange(
        Point start,
        Point end,
        std::optional<uint32_t> geometryStage = std::nullopt,
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
        std::optional<uint32_t> geometryStage = std::nullopt,
        Validity::Direction direction = Validity::Empty);

    /**
     * Append a new line position validity based on an integer offset.
     * Examples:
     *   (BufferOffset, 0) -> Validity at point #0 in the referenced geometry.
     */
    model_ptr<Validity> newPoint(
        Validity::GeometryOffsetType offsetType,
        int32_t pos,
        std::optional<uint32_t> geometryStage = std::nullopt,
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
        std::optional<uint32_t> geometryStage = std::nullopt,
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
        std::optional<uint32_t> geometryStage = std::nullopt,
        Validity::Direction direction = Validity::Empty);

    /**
     * Append an arbitrary validity geometry. Note: You may use non-line geometries here.
     */
    model_ptr<Validity>
    newGeometry(model_ptr<Geometry>, Validity::Direction direction = Validity::Empty);

    /**
     * Append a validity that references a feature ID without restricting the geometry.
     * The referenced feature's geometry is resolved when the validity is evaluated.
     */
    model_ptr<Validity>
    newFeatureId(model_ptr<FeatureId> const& featureId, Validity::Direction direction = Validity::Empty);

    /**
     * Append a validity that references a staged geometry in the current feature context.
     */
    model_ptr<Validity>
    newGeomStage(uint32_t geometryStage, Validity::Direction direction = Validity::Empty);

    /**
     * Append a semantic transition validity connecting two feature endpoints.
     */
    model_ptr<Validity> newFeatureTransition(
        model_ptr<Feature> const& fromFeature,
        Validity::TransitionEnd fromConnectedEnd,
        model_ptr<Feature> const& toFeature,
        Validity::TransitionEnd toConnectedEnd,
        uint32_t transitionNumber,
        Validity::Direction direction = Validity::Empty);

    /**
     * Append a complete validity. If no explicit direction is given,
     * it is represented as complete coverage in both directions.
     */
    model_ptr<Validity> newComplete(Validity::Direction direction = Validity::Empty);

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
