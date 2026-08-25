#pragma once

#include "attrpoint.h"
#include "geometry.h"
#include "sourcedatareference.h"
#include "validity-data.h"

namespace mapget
{

class Geometry;
class Feature;
class FeatureId;

/** Logical position in one shared interwoven AttrPointSequence. */
class AttrPointIndex final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
    friend class TileFeatureLayer;

public:
    /** Return the shared sequence defining this logical index. */
    [[nodiscard]] model_ptr<AttrPointSequence> sequence() const;

    /** Return the zero-based logical index. */
    [[nodiscard]] uint32_t index() const;

    /** Serialize the compact sequence reference and logical index. */
    [[nodiscard]] nlohmann::json toJson() const override;

    explicit AttrPointIndex(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(key)
    {
    }

    AttrPointIndex(
        simfil::ModelConstPtr model,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);

    AttrPointIndex() = delete;

protected:
    /** Expose object semantics to SIMFIL and generic inspection. */
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t fieldIndex) const override;
    [[nodiscard]] simfil::ModelNode::Ptr get(simfil::StringId const& field) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t fieldIndex) const override;
    [[nodiscard]] uint32_t size() const override;
    bool iterate(IterCallback const& callback) const override;
};

/** Inclusive logical range in one shared interwoven AttrPointSequence. */
class AttrPointIndexRange final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
    friend class TileFeatureLayer;

public:
    /** Return the shared sequence defining this logical range. */
    [[nodiscard]] model_ptr<AttrPointSequence> sequence() const;

    /** Return the inclusive start index. */
    [[nodiscard]] uint32_t start() const;

    /** Return the inclusive end index. */
    [[nodiscard]] uint32_t end() const;

    /** Serialize the compact sequence reference and inclusive endpoints. */
    [[nodiscard]] nlohmann::json toJson() const override;

    explicit AttrPointIndexRange(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(key)
    {
    }

    AttrPointIndexRange(
        simfil::ModelConstPtr model,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);

    AttrPointIndexRange() = delete;

protected:
    /** Expose object semantics to SIMFIL and generic inspection. */
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t fieldIndex) const override;
    [[nodiscard]] simfil::ModelNode::Ptr get(simfil::StringId const& field) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t fieldIndex) const override;
    [[nodiscard]] uint32_t size() const override;
    bool iterate(IterCallback const& callback) const override;
};

/**
 * Represents an attribute or relation validity with respect to a feature's geometry.
 */
class Validity : public simfil::ProceduralObject<7, Validity, TileFeatureLayer>
{
    friend class TileFeatureLayer;
    friend class PointNode;
    friend class AttrPointIndex;
    friend class AttrPointIndexRange;

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
    static constexpr GeometryDescriptionType AttrPointIndexValidity = ValidityData::AttrPointIndexValidity;
    static constexpr GeometryDescriptionType AttrPointIndexRangeValidity = ValidityData::AttrPointIndexRangeValidity;

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

    /** Referenced layer-local geometry name used for validity resolution. */
    [[nodiscard]] std::optional<std::string_view> geometryName() const;
    void setGeometryName(std::optional<std::string_view> geometryName);

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

    /** Set/get one logical position in a shared interwoven AttrPointSequence. */
    void setAttrPointIndex(model_ptr<AttrPointSequence> const& sequence, uint32_t index);
    [[nodiscard]] model_ptr<AttrPointIndex> attrPointIndex() const;

    /** Set/get one inclusive logical range in a shared interwoven AttrPointSequence. */
    void setAttrPointIndexRange(
        model_ptr<AttrPointSequence> const& sequence,
        uint32_t start,
        uint32_t end);
    [[nodiscard]] model_ptr<AttrPointIndexRange> attrPointIndexRange() const;

    /**
     * Get or set a simple geometry for the validity.
     */
    void setSimpleGeometry(model_ptr<Geometry>);
    [[nodiscard]] model_ptr<Geometry> simpleGeometry() const;

    /**
     * Get or set a semantic feature transition validity.
     * The connected ends indicate which endpoint of each referenced feature ID touches the transition.
     * Feature IDs are stored directly so transitions may reference features outside the current tile.
     */
    void setFeatureTransition(
        model_ptr<FeatureId> const& fromFeatureId,
        TransitionEnd fromConnectedEnd,
        model_ptr<FeatureId> const& toFeatureId,
        TransitionEnd toConnectedEnd,
        uint32_t transitionNumber);

    /** Convenience overload for transitions between features in the current tile. */
    void setFeatureTransition(
        model_ptr<Feature> const& fromFeature,
        TransitionEnd fromConnectedEnd,
        model_ptr<Feature> const& toFeature,
        TransitionEnd toConnectedEnd,
        uint32_t transitionNumber);

    [[nodiscard]] model_ptr<FeatureId> transitionFromFeatureId() const;
    [[nodiscard]] model_ptr<FeatureId> transitionToFeatureId() const;

    /**
     * Resolve transition endpoint IDs to features in the current tile when possible.
     * Cross-tile, external-map, and secondary-ID references intentionally return null.
     */
    [[nodiscard]] model_ptr<Feature> transitionFromFeature() const;
    [[nodiscard]] model_ptr<Feature> transitionToFeature() const;
    [[nodiscard]] std::optional<TransitionEnd> transitionFromConnectedEnd() const;
    [[nodiscard]] std::optional<TransitionEnd> transitionToConnectedEnd() const;
    [[nodiscard]] std::optional<uint32_t> transitionNumber() const;

    /**
     * Compute the actual shape-points of the validity with respect to one
     * of the geometries in the given collection, or the geometry collection of
     * the directly referenced feature. The geometry is picked based
     * on the validity's geometryName when available. The return value may be one of the following:
     * - An empty vector, indicating that the validity could not be applied.
     *   If an error string was passed, then it would be set to an error message.
     * - A vector containing a single point, if the validity resolved to a point geometry.
     * - A vector containing more than one point, if the validity resolved to a poly-line.
     * For feature transitions, transitionPivotIndex receives the point that
     * separates the incoming and outgoing road slices when provided.
     */
     SelfContainedGeometry computeGeometry(
         model_ptr<GeometryCollection> geometryCollection,
         std::string* error=nullptr,
         uint32_t* transitionPivotIndex=nullptr) const;

protected:
    using Data = ValidityData;

public:
    explicit Validity(simfil::detail::mp_key key)
        : simfil::ProceduralObject<7, Validity, TileFeatureLayer>(key) {}
    Validity(Direction direction,
             simfil::ModelConstPtr layer,
             simfil::ModelNodeAddress a,
             simfil::detail::mp_key key);
    Validity(Direction direction,
             simfil::ModelConstPtr layer,
             simfil::ModelNodeAddress a,
             simfil::ScalarValueType runtimeData,
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
struct MultiValidity : public simfil::BaseArray<TileFeatureModelLayerBase, Validity>
{
    friend class TileFeatureLayer;

    /**
     * Append a new line position validity based on an absolute geographic position.
     */
    model_ptr<Validity> newPoint(
        Point pos,
        std::optional<std::string_view> geometryName = std::nullopt,
        Validity::Direction direction = Validity::Empty);

    /**
     * Append a new line range validity based on absolute geographic positions.
     */
    model_ptr<Validity> newRange(
        Point start,
        Point end,
        std::optional<std::string_view> geometryName = std::nullopt,
        Validity::Direction direction = Validity::Empty);

    /** Append one logical position in a shared interwoven AttrPointSequence. */
    model_ptr<Validity> newAttrPointIndex(
        model_ptr<AttrPointSequence> const& sequence,
        uint32_t index,
        Validity::Direction direction = Validity::Empty);

    /** Append one inclusive logical range in a shared interwoven AttrPointSequence. */
    model_ptr<Validity> newAttrPointIndexRange(
        model_ptr<AttrPointSequence> const& sequence,
        uint32_t start,
        uint32_t end,
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
        std::optional<std::string_view> geometryName = std::nullopt,
        Validity::Direction direction = Validity::Empty);

    /**
     * Append a new line position validity based on an integer offset.
     * Examples:
     *   (BufferOffset, 0) -> Validity at point #0 in the referenced geometry.
     */
    model_ptr<Validity> newPoint(
        Validity::GeometryOffsetType offsetType,
        int32_t pos,
        std::optional<std::string_view> geometryName = std::nullopt,
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
        std::optional<std::string_view> geometryName = std::nullopt,
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
        std::optional<std::string_view> geometryName = std::nullopt,
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
     * Append a validity that references a named geometry in the current feature context.
     */
    model_ptr<Validity>
    newGeomName(std::string_view geometryName, Validity::Direction direction = Validity::Empty);

    /**
     * Append a semantic transition validity connecting two feature endpoints.
     */
    model_ptr<Validity> newFeatureTransition(
        model_ptr<FeatureId> const& fromFeatureId,
        Validity::TransitionEnd fromConnectedEnd,
        model_ptr<FeatureId> const& toFeatureId,
        Validity::TransitionEnd toConnectedEnd,
        uint32_t transitionNumber,
        Validity::Direction direction = Validity::Empty);

    /** Convenience overload for transitions between features in the current tile. */
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

    [[nodiscard]] ModelNode::Ptr at(int64_t i) const override;
    bool iterate(ModelNode::IterCallback const& cb) const override;  // NOLINT (allow discard)

private:
    /**
     * Access the concrete owning feature layer for validity data allocation.
     * The array base is intentionally the common model type so MSVC does not
     * instantiate simfil BaseArray helpers against an incomplete TileFeatureLayer.
     */
    TileFeatureLayer& featureLayer();

    using simfil::BaseArray<TileFeatureModelLayerBase, Validity>::BaseArray;
};

}
