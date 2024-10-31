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
class Relation : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
    friend class TileFeatureLayer;
    friend class Feature;
    template<typename> friend struct simfil::model_ptr;

public:
    /**
     * Source validity accessors.
     */
    [[nodiscard]] model_ptr<ValidityCollection> sourceValidities(bool createIfMissing);
    [[nodiscard]] model_ptr<ValidityCollection> sourceValidities() const;
    void setSourceValidities(model_ptr<ValidityCollection> const& validityGeom);

    /**
     * Target validity accessors.
     */
    [[nodiscard]] model_ptr<ValidityCollection> targetValidities(bool createIfMissing);
    [[nodiscard]] model_ptr<ValidityCollection> targetValidities() const;
    void setTargetValidities(model_ptr<ValidityCollection> const& validityGeom);

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
    /** ModelNode interface. */
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const simfil::StringId &) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    /** Actual per-attribute data that is stored in the model's attributes-column. */
    struct Data {
        simfil::StringId name_ = 0;
        simfil::ModelNodeAddress targetFeatureId_;
        simfil::ModelNodeAddress sourceValidities_;
        simfil::ModelNodeAddress targetValidities_;
        simfil::ModelNodeAddress sourceData_;

        template<typename S>
        void serialize(S& s) {
            s.value2b(name_);
            s.object(targetFeatureId_);
            s.object(sourceValidities_);
            s.object(targetValidities_);
            s.object(sourceData_);
        }
    };

    Relation(Data* data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);
    Relation() = default;

    /** Reference to the actual data stored for the relation. */
    Data* data_{};
};

}
