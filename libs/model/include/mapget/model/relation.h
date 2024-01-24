#pragma once

#include "featureid.h"

namespace mapget
{

/**
 * The KeyValuePairs type is a vector of pairs, where each pair
 * consists of a string_view key and a variant value that can be
 * either an int64_t or a string_view.
 */
using KeyValuePairs = std::vector<std::pair<
    std::string_view,
    std::variant<int64_t, std::string_view>>>;

class TileFeatureLayer;

/**
 * Represents a feature relation which belongs to a
 * source feature, and points to a destination feature
 * by its id. It may also have a validity geometry on either side.
 */
class Relation : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
    friend class TileFeatureLayer;

public:
    /**
     * Source validity accessors.
     */
    [[nodiscard]] bool hasSourceValidity() const;
    [[nodiscard]] model_ptr<Geometry> sourceValidity() const;
    void setSourceValidity(model_ptr<Geometry> const& validityGeom);

    /**
     * Target validity accessors.
     */
    [[nodiscard]] bool hasTargetValidity() const;
    [[nodiscard]] model_ptr<Geometry> targetValidity() const;
    void setTargetValidity(model_ptr<Geometry> const& validityGeom);

    /**
     * Read-only relation name accessor.
     */
    [[nodiscard]] std::string_view name() const;

    /**
     * Read-only target feature accessors.
     */

    [[nodiscard]] model_ptr<FeatureId> target() const;

protected:
    /** ModelNode interface. */
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const simfil::FieldId &) const override;
    [[nodiscard]] simfil::FieldId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    /** Actual per-attribute data that is stored in the model's attributes-column. */
    struct Data {
        simfil::FieldId name_ = 0;
        simfil::ModelNodeAddress targetFeatureId_;
        simfil::ModelNodeAddress sourceValidity_;
        simfil::ModelNodeAddress targetValidity_;

        template<typename S>
        void serialize(S& s) {
            s.value2b(name_);
            s.object(targetFeatureId_);
            s.object(sourceValidity_);
            s.object(targetValidity_);
        }
    };

    Relation(Data* data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);
    Relation() = default;

    /** Reference to the actual data stored for the relation. */
    Data* data_{};
};

/**
 * Collection of attribute layers - this is merely a typed dict which
 * stores (layer-name, layer) pairs.
 */
class RelationList : protected simfil::Array
{
    friend class TileFeatureLayer;
    friend class bitsery::Access;
    friend class Feature;
    friend ModelNode::Ptr;

public:
    /**
     * Create a new named relation and immediately insert it into the collection.
     * Create a new feature id for the target, based on the given ID parts.
     */
    model_ptr<Relation> newRelation(std::string_view const& name, std::string_view const& targetType, KeyValuePairs const& targetIdParts);

    /**
     * Create a new named relation and immediately insert it into the collection.
     * Use an existing feature id for the target.
     */
    model_ptr<Relation> newRelation(std::string_view const& name, model_ptr<FeatureId> const& target);

    /**
     * Add a relation to the collection which was previously created.
     * You can share a single relation between multiple collections, it will not be copied.
     */
    void addRelation(model_ptr<Relation> r);

protected:
    RelationList(simfil::ArrayIndex i, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);
};

}