#pragma once

#include "geometry.h"
#include "sourcedatareference.h"

namespace mapget
{

class Geometry;

/**
 * Represents a feature attribute which belongs to an
 * AttributeLayer, and may have typed `direction` and
 * `validity` fields in addition to other arbitrary object fields.
 */
class Attribute : public simfil::ProceduralObject<2, Attribute>
{
    friend class TileFeatureLayer;
    template<typename> friend struct simfil::shared_model_ptr;

public:
    /**
     * Attribute validity accessors.
     */
    [[nodiscard]] bool hasValidity() const;
    [[nodiscard]] model_ptr<Geometry> validity() const;
    void setValidity(model_ptr<Geometry> const& validityGeom);

    /**
     * Read-only attribute name accessor.
     */
    [[nodiscard]] std::string_view name() const;

    /**
     * Iterate over the attribute's extra fields. The passed lambda must return
     * true to continue iterating, or false to abort iteration.
     * @return True if all fields were visited, false if the callback ever returned false.
     */
    bool forEachField(std::function<bool(std::string_view const& k, simfil::ModelNode::Ptr const& val)> const& cb) const;

    /**
     * Source data related accessors.
     */
    model_ptr<SourceDataReferenceCollection> sourceDataReferences() const;
    void setSourceDataReferences(simfil::ModelNode::Ptr const& node);

protected:

    /** Actual per-attribute data that is stored in the model's attributes-column. */
    struct Data {
        simfil::ModelNodeAddress validity_;
        simfil::ArrayIndex fields_ = -1;
        simfil::StringId name_ = 0;
        simfil::ModelNodeAddress sourceDataRefs_;

        template<typename S>
        void serialize(S& s) {
            s.object(validity_);
            s.value4b(fields_);
            s.value2b(name_);
            s.object(sourceDataRefs_);
        }
    };

    Attribute(Data* data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);
    Attribute() = default;

    /**
     * Pointer to the actual data stored for the attribute.
     */
    Data* data_ = nullptr;
};

}
