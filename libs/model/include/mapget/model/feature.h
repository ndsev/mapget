#pragma once

#include "featureid.h"
#include "attrlayer.h"
#include "attr.h"

#include "sfl/small_vector.hpp"

namespace mapget
{

class Feature : protected simfil::MandatoryDerivedModelPoolNodeBase<TileFeatureLayer>
{
    friend class bitsery::Access;
    friend class TileFeatureLayer;

public:
    [[nodiscard]] std::string_view typeId() const;

    [[nodiscard]] model_ptr<FeatureId> id() const;
    [[nodiscard]] model_ptr<GeometryCollection> geom();
    [[nodiscard]] model_ptr<AttributeLayerList> attributeLayers();
    [[nodiscard]] model_ptr<Object> attributes();
    [[nodiscard]] model_ptr<Array> children();

    /**
     * Evaluate a filter expression on this feature.
     */
    std::vector<simfil::Value> evaluate(std::string_view const& expression);

protected:
    /**
     * Simfil Model-Node Functions
     */
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const simfil::FieldId &) const override;
    [[nodiscard]] simfil::FieldId keyAt(int64_t) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

    /**
     * Feature Data
     */
    struct Data
    {
        simfil::ModelNodeAddress id_;
        simfil::ModelNodeAddress geom_;
        simfil::ModelNodeAddress attrLayers_;
        simfil::ModelNodeAddress attrs_;
        simfil::ModelNodeAddress children_;
    };

    Feature(Data& d, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);

    Data& data_;

    // We keep the fields in a tiny vector on the stack,
    // because their number is dynamic, as a variable number
    // of id-part fields is adopted from the feature id.
    sfl::small_vector<std::pair<simfil::FieldId, simfil::ModelNode::Ptr>, 32> fields_;
    void updateFields();

    struct FeaturePropertyView : public simfil::MandatoryDerivedModelPoolNodeBase<TileFeatureLayer>
    {
        [[nodiscard]] simfil::ValueType type() const override;
        [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
        [[nodiscard]] uint32_t size() const override;
        [[nodiscard]] ModelNode::Ptr get(const simfil::FieldId &) const override;
        [[nodiscard]] simfil::FieldId keyAt(int64_t) const override;
        [[nodiscard]] bool iterate(IterCallback const& cb) const override;

        FeaturePropertyView(Data& d, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);

        Data& data_;
        std::optional<model_ptr<Object>> attrs_;
    };
};

}