#pragma once

#include "simfil/model/nodes.h"
#include "info.h"
#include <vector>

namespace mapget
{

class TileFeatureLayer;
using FeatureLayerConstPtr = std::shared_ptr<TileFeatureLayer const>;

template<typename T>
using model_ptr = simfil::model_ptr<T>;

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

public:
    /** Convert the FeatureId to a string like `<type-id>.<part-value-0>...<part-value-n>` */
    [[nodiscard]] std::string toString() const;

    /** Get the feature ID's type id. */
    [[nodiscard]] std::string_view typeId() const;

    /** Get all id-part key-value-pairs (including the common prefix). */
    [[nodiscard]] KeyValueViewPairs keyValuePairs() const;

    struct Data {
        MODEL_COLUMN_TYPE(8);

        bool useCommonTilePrefix_ = false;
        uint8_t idCompositionOffset_ = 0;
        simfil::StringId typeId_ = 0;
        simfil::ArrayIndex idPartValues_ = simfil::InvalidArrayIndex;
    };

protected:
    /**
     * Internal Node Access APIs
     */
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ScalarValueType value() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const simfil::StringId &) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

public:
    explicit FeatureId(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(key) {}
    FeatureId(Data& data,
              simfil::ModelConstPtr l,
              simfil::ModelNodeAddress a,
              simfil::detail::mp_key key);
    FeatureId(Data const& data,
              simfil::ModelConstPtr l,
              simfil::ModelNodeAddress a,
              simfil::detail::mp_key key);
    FeatureId() = delete;

protected:
    Data data_{};
    model_ptr<Array> values_;
    std::vector<simfil::StringId> partNames_;
};

}
