#pragma once

#include <span>

#include "geometry.h"
#include "simfil/overlay.h"
#include "sourcedatareference.h"
#include "validity.h"

namespace mapget
{

class Geometry;
class Attribute;

/** Read-only public validity layout for one attachment of a possibly shared Attribute.
 * Keep this view while iterating: its record readers and geometry axes are query-local.
 * A detached Attribute's validityOrNull() always refers to stored entries, never synthesized
 * markers.
 */
class AttributeValidityView
{
public:
    /** Public validity: stored entries first, then tagged measurement records in source order. */
    [[nodiscard]] uint32_t size() const;
    [[nodiscard]] model_ptr<Validity> at(uint32_t index) const;

    /** Exact candidates keyed by output field. Assignments and annotations not selected by a
     * series have no sample, whether or not they share coordinates with a measurement.
     * Candidate contributors carry the path into the raw attribute and its record ordinal;
     * lookup-only _sourceData on a candidate retains the enclosing Attribute's source references.
     */
    [[nodiscard]] simfil::ModelNode::Ptr markerSamples(uint32_t index) const;
    [[nodiscard]] simfil::ModelNode::Ptr markerSample(uint32_t index, std::string_view field) const;

    /** Attribute payload with one public validity list (a singleton is unwrapped). */
    [[nodiscard]] simfil::ModelNode::Ptr node() const { return node_; }

    /** Attribute filter/inspection context. Index zero is also the no-validity fallback. */
    [[nodiscard]] model_ptr<simfil::OverlayNode>
    queryContext(uint32_t attributeIndex, uint32_t validityIndex) const;

private:
    friend class Attribute;
    struct State;
    AttributeValidityView() = default;
    [[nodiscard]] simfil::ModelNode::Ptr publicNode() const;
    [[nodiscard]] PartitionFeatureLayer const& model() const;
    simfil::ModelNode::Ptr node_, host_;
    model_ptr<MultiValidity> stored_;
    simfil::StringId layerKey_, nameKey_;
    std::string unknownLayer_;
    std::shared_ptr<State> state_;
};

/**
 * Represents a named feature attribute which belongs to an AttributeLayer.
 * It may reference several Validity objects and carry source-data references
 * in addition to arbitrary producer-defined payload fields.
 */
class Attribute : public simfil::ProceduralObject<2, Attribute, PartitionFeatureLayer>
{
    friend class PartitionFeatureLayer;

public:
    /**
     * Stored assignments and supplemental annotations only. validity() creates storage on demand;
     * validityOrNull() inspects without allocation. Supplied validities must
     * belong to the same model pool. Use validityView() for public markers.
     */
    [[nodiscard]] model_ptr<MultiValidity> validity();
    [[nodiscard]] model_ptr<MultiValidity> validityOrNull() const;
    void setValidity(const model_ptr<MultiValidity>& validities) const;

    /** Append value annotations without mutating another attribute's shared assignment.
     * Returns the first added ordinal. Bind property references after this call. */
    uint32_t appendTaggedValidities(std::span<model_ptr<Validity> const> annotations);

    [[nodiscard]] AttributeValidityView
    validityView(model_ptr<Feature> const& feature, std::string_view layerName) const;

    /** Optional source disambiguators on this occurrence. Omit both for uniquely
     * resolvable declarations. Restricted feature clones carry these as model fields. */
    void setPointSeriesBranch(uint32_t referencedBranch);
    void setPointSeriesGeometry(uint32_t geometryOrdinal);
    [[nodiscard]] std::optional<uint32_t> pointSeriesBranch() const;
    [[nodiscard]] std::optional<uint32_t> pointSeriesGeometry() const;

    /**
     * Read-only attribute name accessor.
     */
    [[nodiscard]] std::string_view name() const;

    /**
     * Compatibility access to validityView(feature, layerName).queryContext(...).
     * The public count is derived from that occurrence, not the supplied legacy count.
     */
    [[nodiscard]] model_ptr<simfil::OverlayNode> queryContext(
        model_ptr<Feature> const& feature,
        std::string_view layerName,
        uint32_t attributeIndex,
        uint32_t validityIndex,
        uint32_t validityCount) const;

    /**
     * Iterate over the attribute's extra fields. The passed lambda must return
     * true to continue iterating, or false to abort iteration.
     * @return True if all fields were visited, false if the callback ever returned false.
     */
    bool forEachField(std::function<bool(std::string_view const& k, simfil::ModelNode::Ptr const& val)> const& cb) const;

    /**
     * Source data related accessors.
     */
    [[nodiscard]] model_ptr<SourceDataReferenceCollection> sourceDataReferences() const;
    void setSourceDataReferences(simfil::ModelNode::Ptr const& node);

protected:

    /** Actual per-attribute data that is stored in the model's attributes-column. */
    struct Data {
        MODEL_COLUMN_TYPE(16);

        simfil::ModelNodeAddress validities_;
        simfil::ArrayIndex fields_ = simfil::InvalidArrayIndex;
        simfil::StringId name_ = 0;
        simfil::ModelNodeAddress sourceDataRefs_;
    };

public:
    explicit Attribute(simfil::detail::mp_key key)
        : simfil::ProceduralObject<2, Attribute, PartitionFeatureLayer>(key)
    {
    }
    Attribute(
        Data* data,
        simfil::ModelConstPtr l,
        simfil::ModelNodeAddress a,
        simfil::detail::mp_key key);

    Attribute() = delete;

    /**
     * Pointer to the actual data stored for the attribute.
     */
protected:
    Data* data_ = nullptr;
};

}
