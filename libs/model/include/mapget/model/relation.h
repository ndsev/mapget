#pragma once

#include "featureid.h"
#include "simfil/model/nodes.h"
#include "sourcedatareference.h"
#include "validity.h"

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
    template<typename> friend struct simfil::model_ptr;

public:
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

protected:
    /** Actual per-attribute data that is stored in the model's attributes-column. */
    struct Data {
        simfil::StringId name_ = 0;
        simfil::ModelNodeAddress targetFeatureId_;
        simfil::ModelNodeAddress sourceValidity_;
        simfil::ModelNodeAddress targetValidity_;
        simfil::ModelNodeAddress sourceData_;

        template<typename S>
        void serialize(S& s) {
            s.value2b(name_);
            s.object(targetFeatureId_);
            s.object(sourceValidity_);
            s.object(targetValidity_);
            s.object(sourceData_);
        }
    };

    Relation(Data* data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);
    Relation() = default;

    /** Reference to the actual data stored for the relation. */
    Data* data_{};
};

}
