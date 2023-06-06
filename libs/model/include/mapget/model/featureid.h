#pragma once

#include "simfil/model/nodes.h"
#include <functional>

namespace mapget
{

class TileFeatureLayer;
using FeatureLayerConstPtr = std::shared_ptr<TileFeatureLayer const>;

template<typename T>
using model_ptr = simfil::shared_model_ptr<T>;

using Object = simfil::Object;
using Array = simfil::Array;
using GeometryCollection = simfil::GeometryCollection;
using Geometry = simfil::Geometry;
using GeomType = simfil::Geometry::GeomType;

/**
 * Unique feature ID
 */
class FeatureId : protected simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
    friend class TileFeatureLayer;
    friend class Feature;
    friend class bitsery::Access;

public:
    /** Convert the FeatureId to a string like `<type-id>.<part-value-0>...<part-value-n>` */
    [[nodiscard]] std::string toString() const;

    /** Get the feature ID's type id. */
    [[nodiscard]] std::string_view typeId() const;

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
            s.value4b(idParts_.value_);
        }
    };

    FeatureId(Data& data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);

    Data& data_;

    // Optional because resolve must be called, in
    // the constructor body.
    model_ptr<Object> fields_;
};

}