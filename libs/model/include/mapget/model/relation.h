#pragma once

#include "featureid.h"
#include "nlohmann/json.hpp"
#include "simfil/model/nodes.h"
#include "sourcedatareference.h"
#include "validity.h"

#include <limits>
#include <optional>

namespace mapget
{

class TileFeatureLayer;
class Geometry;

/**
 * Represents a feature relation which belongs to a
 * source feature, and points to a destination feature
 * by its id. It may also have a validity geometry on either side.
 */
class Relation : public simfil::ProceduralObject<6, Relation, TileFeatureLayer>
{
    friend class TileFeatureLayer;
    friend class Feature;

public:
    static constexpr uint16_t InvalidFeatureRelationIndex = std::numeric_limits<uint16_t>::max();

    /**
     * Source validity accessors.
     */
    [[nodiscard]] model_ptr<MultiValidity> sourceValidity();
    [[nodiscard]] model_ptr<MultiValidity> sourceValidityOrNull() const;
    void setSourceValidity(const model_ptr<MultiValidity>& validityGeom);

    /**
     * Target validity accessors.
     */
    [[nodiscard]] model_ptr<MultiValidity> targetValidity();
    [[nodiscard]] model_ptr<MultiValidity> targetValidityOrNull() const;
    void setTargetValidity(const model_ptr<MultiValidity>& validityGeom);

    /**
     * Read-only relation name accessor.
     */
    [[nodiscard]] std::string_view name() const;

    /**
     * Read-only target feature accessors.
     */
    [[nodiscard]] model_ptr<FeatureId> target() const;

    /**
     * SourceData accessors.
     */
    [[nodiscard]] model_ptr<SourceDataReferenceCollection> sourceDataReferences() const;
    void setSourceDataReferences(simfil::ModelNode::Ptr const& addresses);

    /**
     * Feature-local ordinal assigned when the relation is inserted into its
     * owning feature. RelationReference JSON uses this ordinal as a compact,
     * feature-local identity token.
     */
    [[nodiscard]] std::optional<uint16_t> featureRelationIndex() const;

protected:
    /** Actual per-attribute data that is stored in the model's attributes-column. */
    struct Data {
        MODEL_COLUMN_TYPE(20);

        simfil::StringId name_ = 0;
        uint16_t featureRelationIndex_ = InvalidFeatureRelationIndex;
        simfil::ModelNodeAddress targetFeatureId_;
        simfil::ModelNodeAddress sourceValidity_;
        simfil::ModelNodeAddress targetValidity_;
        simfil::ModelNodeAddress sourceData_;
    };

public:
    explicit Relation(simfil::detail::mp_key key)
        : simfil::ProceduralObject<6, Relation, TileFeatureLayer>(key) {}
    Relation(Data* data,
             simfil::ModelConstPtr l,
             simfil::ModelNodeAddress a,
             simfil::detail::mp_key key);
    Relation() = delete;

protected:
    /** Assigns the owning feature's local relation ordinal. */
    void setFeatureRelationIndex(uint16_t index);

    /** Reference to the actual data stored for the relation. */
    Data* data_{};
};

/**
 * Lightweight reference to a canonical feature relation.
 *
 * The node address index points directly at the target relation column entry.
 * Direct field access is forwarded to the canonical relation for simfil use,
 * while indexed/keyed traversal exposes the compact `$mapgetRelation` token
 * needed by generic JSON serialization.
 */
class RelationReference : public simfil::ProceduralObject<6, RelationReference, TileFeatureLayer>
{
    friend class TileFeatureLayer;

public:
    explicit RelationReference(simfil::detail::mp_key key)
        : simfil::ProceduralObject<6, RelationReference, TileFeatureLayer>(key) {}
    RelationReference(simfil::ModelConstPtr l,
                      simfil::ModelNodeAddress a,
                      simfil::detail::mp_key key);
    RelationReference() = delete;

    /** Resolve the canonical relation referenced by this node. */
    [[nodiscard]] model_ptr<Relation> relation() const;

    /** Resolve relation fields while keeping `$mapgetRelation` addressable. */
    [[nodiscard]] simfil::ModelNode::Ptr get(simfil::StringId const& f) const override;

    /** Expose compact JSON fields for generic object serialization. */
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t i) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t i) const override;
    [[nodiscard]] uint32_t size() const override;

    /** Emit a compact relation-reference token for GeoJSON round-tripping. */
    [[nodiscard]] nlohmann::json toJson() const override;
};

}
