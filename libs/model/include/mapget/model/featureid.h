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
using Point = simfil::geo::Point<double>;

/**
 * Unique feature ID
 */
class FeatureId : protected simfil::MandatoryDerivedModelPoolNodeBase<TileFeatureLayer>
{
    friend class TileFeatureLayer;
    friend class bitsery::Access;

public:
    /** Convert the FeatureId to a string like `<type-id>.<part-value-0>...<part-value-n>` */
    [[nodiscard]] std::string toString() const;

    /** Get the feature ID's type id. */
    [[nodiscard]] std::string_view typeId() const;

protected:
    /** Get the number of id parts (optionally combined with shared prefix from tile layer). */
    [[nodiscard]] size_t numParts() const;

    /** Get the feature ID's n-th id-part name and value. */
    [[nodiscard]] std::pair<simfil::FieldId, simfil::ModelNode::Ptr> part(size_t i) const;

    /**
     * Internal Node Access APIs
     */
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ScalarValueType value() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const simfil::FieldId &) const override;
    [[nodiscard]] simfil::FieldId keyAt(int64_t) const override;
    void iterate(IterCallback const& cb) const override;

    struct Data {
        bool useCommonTilePrefix_ = false;
        simfil::FieldId typeId_ = 0;
        simfil::ModelNodeAddress idParts_;
    };

    FeatureId(Data& data, FeatureLayerConstPtr l, simfil::ModelNodeAddress a);

    Data& data_;
};

}