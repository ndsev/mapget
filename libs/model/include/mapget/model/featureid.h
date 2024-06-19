#pragma once

#include "simfil/model/nodes.h"
#include "info.h"
#include "sfl/small_vector.hpp"

namespace mapget
{

class TileFeatureLayer;
using FeatureLayerConstPtr = std::shared_ptr<TileFeatureLayer const>;

template<typename T>
using model_ptr = simfil::shared_model_ptr<T>;

using Object = simfil::Object;
using Array = simfil::Array;

/**
 * Unique feature ID
 */
class FeatureId : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
    friend class TileFeatureLayer;
    friend class Feature;
    friend class Relation;
    friend class bitsery::Access;
    template<typename> friend struct simfil::shared_model_ptr;

public:
    /** Convert the FeatureId to a string like `<type-id>.<part-value-0>...<part-value-n>` */
    [[nodiscard]] std::string toString() const;

    /** Get the feature ID's type id. */
    [[nodiscard]] std::string_view typeId() const;

    /** Get all id-part key-value-pairs (including the common prefix). */
    [[nodiscard]] KeyValueViewPairs keyValuePairs() const;

protected:
    /**
     * Internal Node Access APIs
     */
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ScalarValueType value() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const simfil::FieldId &) const override;
    [[nodiscard]] simfil::FieldId keyAt(int64_t) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

    struct Data {
        bool useCommonTilePrefix_ = false;
        simfil::FieldId typeId_ = 0;
        simfil::ModelNodeAddress idParts_;

        template<typename S>
        void serialize(S& s) {
            s.value1b(useCommonTilePrefix_);
            s.value2b(typeId_);
            s.object(idParts_);
        }
    };

    FeatureId(Data& data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);
    FeatureId() = default;

    Data* data_ = nullptr;

    model_ptr<Object> fields_;
};

}
