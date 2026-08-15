#pragma once

#include "featureid.h"
#include "geometry.h"
#include "sourcedatareference.h"

#include "glm/glm.hpp"
#include "nlohmann/json_fwd.hpp"
#include "simfil/model/nodes.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace mapget
{

class Feature;
class TileFeatureLayer;

/**
 * One explicitly inserted point in an interwoven attribute-point sequence.
 *
 * Shape points remain owned by the referenced Geometry. AttrPoint stores only
 * the points that the source model inserts between those shape points, plus
 * their logical sequence index and optional source-data provenance.
 */
class AttrPoint final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
    friend class TileFeatureLayer;

public:
    /** Return this point's zero-based index in the complete interwoven sequence. */
    [[nodiscard]] uint32_t index() const;

    /** Return this point in the layer's absolute coordinate system. */
    [[nodiscard]] Point point() const;

    /** Return source-data provenance attached specifically to this point. */
    [[nodiscard]] model_ptr<SourceDataReferenceCollection> sourceDataReferences() const;

    /** Serialize the point as its compact strict-GeoJSON representation. */
    [[nodiscard]] nlohmann::json toJson() const override;

    explicit AttrPoint(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(key)
    {
    }

    AttrPoint(
        simfil::ModelConstPtr model,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);

    AttrPoint() = delete;

protected:
    /** Compact persisted representation of one inserted attribute point. */
    struct Data
    {
        MODEL_COLUMN_TYPE(20);

        uint32_t index_ = 0;
        glm::fvec3 point_{};
        simfil::ModelNodeAddress sourceData_{};
    };

    /** Expose object semantics to SIMFIL and generic inspection. */
    [[nodiscard]] simfil::ValueType type() const override;

    /** Resolve one field by ordinal. */
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t index) const override;

    /** Resolve one field by static string ID. */
    [[nodiscard]] simfil::ModelNode::Ptr get(simfil::StringId const& field) const override;

    /** Return the static field ID at an ordinal. */
    [[nodiscard]] simfil::StringId keyAt(int64_t index) const override;

    /** Return the number of exposed fields. */
    [[nodiscard]] uint32_t size() const override;

    /** Visit all exposed fields in stable order. */
    bool iterate(IterCallback const& callback) const override;
};

/**
 * Zero-overhead array view over the inserted points owned by one sequence.
 */
class AttrPointArray final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
    friend class TileFeatureLayer;

public:
    /** Return the inserted point at the requested array position. */
    [[nodiscard]] model_ptr<AttrPoint> attrPointAt(uint32_t index) const;

    explicit AttrPointArray(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(key)
    {
    }

    AttrPointArray(
        simfil::ModelConstPtr model,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);

    AttrPointArray() = delete;

protected:
    /** Expose array semantics to SIMFIL and generic inspection. */
    [[nodiscard]] simfil::ValueType type() const override;

    /** Resolve one inserted point by array index. */
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t index) const override;

    /** Return the number of inserted points. */
    [[nodiscard]] uint32_t size() const override;

    /** Visit all inserted points in logical order. */
    bool iterate(IterCallback const& callback) const override;
};

/**
 * Shared logical sequence interweaving geometry vertices and AttrPoints.
 *
 * The sequence references one canonical feature geometry. Geometry vertices
 * are implicit; only inserted AttrPoints are stored. Logical indices therefore
 * retain source-model semantics without duplicating the render geometry.
 */
class AttrPointSequence final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
    friend class TileFeatureLayer;

public:
    /** Return the feature ID whose geometry defines this sequence. */
    [[nodiscard]] model_ptr<FeatureId> featureId() const;

    /** Return the canonical geometry whose vertices form the sequence backbone. */
    [[nodiscard]] model_ptr<Geometry> geometry() const;

    /** Return the geometry's ordinal within the owning feature. */
    [[nodiscard]] uint32_t geometryIndex() const;

    /** Return all explicitly inserted attribute points. */
    [[nodiscard]] model_ptr<AttrPointArray> attrPoints() const;

    /** Return the number of explicitly inserted attribute points. */
    [[nodiscard]] uint32_t attrPointCount() const;

    /** Return the total number of shape and attribute points. */
    [[nodiscard]] uint32_t positionCount() const;

    /** Return the coordinate at one logical interwoven index. */
    [[nodiscard]] Point pointAt(uint32_t index) const;

    /** Materialize an inclusive logical point range in one merged traversal. */
    [[nodiscard]] std::vector<Point> points(uint32_t start, uint32_t end) const;

    /** Return whether one logical index denotes an explicitly inserted AttrPoint. */
    [[nodiscard]] bool isAttrPoint(uint32_t index) const;

    /** Return the distance in metres from the sequence start to one logical point. */
    [[nodiscard]] double metricOffsetAt(uint32_t index) const;

    /**
     * Append an inserted point in strictly increasing logical-index order.
     * Appending is only possible until another sequence starts using storage.
     */
    model_ptr<AttrPoint> appendAttrPoint(
        uint32_t index,
        Point const& point,
        model_ptr<SourceDataReferenceCollection> const& sourceData = {});

    /** Return source-data provenance attached to the sequence as a whole. */
    [[nodiscard]] model_ptr<SourceDataReferenceCollection> sourceDataReferences() const;

    /** Set source-data provenance attached to the sequence as a whole. */
    void setSourceDataReferences(model_ptr<SourceDataReferenceCollection> const& sourceData);

    /** Serialize the shared sequence definition used by strict GeoJSON. */
    [[nodiscard]] nlohmann::json toJson() const override;

    explicit AttrPointSequence(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(key)
    {
    }

    AttrPointSequence(
        simfil::ModelConstPtr model,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);

    AttrPointSequence() = delete;

protected:
    /** Compact persisted representation of one shared sequence. */
    struct Data
    {
        MODEL_COLUMN_TYPE(20);

        simfil::ModelNodeAddress featureId_{};
        simfil::ModelNodeAddress geometry_{};
        uint32_t firstAttrPoint_ = 0;
        uint32_t attrPointCount_ = 0;
        simfil::ModelNodeAddress sourceData_{};
    };

    /** Expose object semantics to SIMFIL and generic inspection. */
    [[nodiscard]] simfil::ValueType type() const override;

    /** Resolve one field by ordinal. */
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t index) const override;

    /** Resolve one field by static string ID. */
    [[nodiscard]] simfil::ModelNode::Ptr get(simfil::StringId const& field) const override;

    /** Return the static field ID at an ordinal. */
    [[nodiscard]] simfil::StringId keyAt(int64_t index) const override;

    /** Return the number of exposed fields. */
    [[nodiscard]] uint32_t size() const override;

    /** Visit all exposed fields in stable order. */
    bool iterate(IterCallback const& callback) const override;
};

/**
 * Compact JSON/SIMFIL reference to a shared AttrPointSequence definition.
 */
class AttrPointSequenceReference final
    : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
    friend class TileFeatureLayer;

public:
    /** Resolve the referenced canonical sequence. */
    [[nodiscard]] model_ptr<AttrPointSequence> sequence() const;

    /** Emit `{ "$mapgetAttrPointSequence": <index> }` for strict GeoJSON. */
    [[nodiscard]] nlohmann::json toJson() const override;

    explicit AttrPointSequenceReference(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(key)
    {
    }

    AttrPointSequenceReference(
        simfil::ModelConstPtr model,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);

    AttrPointSequenceReference() = delete;

protected:
    /** Expose compact object semantics to SIMFIL and generic inspection. */
    [[nodiscard]] simfil::ValueType type() const override;

    /** Resolve the compact reference value by ordinal. */
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t index) const override;

    /** Resolve the compact reference value by static string ID. */
    [[nodiscard]] simfil::ModelNode::Ptr get(simfil::StringId const& field) const override;

    /** Return the compact reference field ID. */
    [[nodiscard]] simfil::StringId keyAt(int64_t index) const override;

    /** Return one compact reference field. */
    [[nodiscard]] uint32_t size() const override;

    /** Visit the compact reference field. */
    bool iterate(IterCallback const& callback) const override;
};

}  // namespace mapget
