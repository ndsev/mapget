#pragma once

#include "featureid.h"
#include "attrlayer.h"
#include "attr.h"

namespace mapget
{

class Feature : protected simfil::MandatoryDerivedModelPoolNodeBase<TileFeatureLayer>
{
    friend class bitsery::Access;
    friend class TileFeatureLayer;

public:
    [[nodiscard]] std::string typeId() const;

    [[nodiscard]] model_ptr<FeatureId> id() const;
    [[nodiscard]] model_ptr<GeometryCollection> geom();
    [[nodiscard]] model_ptr<AttributeLayers> attributeLayers();
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
    void iterate(IterCallback const& cb) const override;

    /**
     * Feature Data
     */
    struct Data
    {
        simfil::ModelNodeAddress id_;
        simfil::ModelNodeAddress geom_;
        simfil::ModelNodeAddress attrLayers_;
        simfil::ModelNodeAddress attrs_;
    };

    Feature(Data& d, FeatureLayerConstPtr l, simfil::ModelNodeAddress a);

    Data& data_;

    class FeaturePropertyView : protected simfil::MandatoryDerivedModelPoolNodeBase<TileFeatureLayer>
    {
        [[nodiscard]] simfil::ValueType type() const override;
        [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
        [[nodiscard]] uint32_t size() const override;
        [[nodiscard]] ModelNode::Ptr get(const simfil::FieldId &) const override;
        [[nodiscard]] simfil::FieldId keyAt(int64_t) const override;
        void iterate(IterCallback const& cb) const override;

        FeaturePropertyView(Data& d, FeatureLayerConstPtr l, simfil::ModelNodeAddress a);

        Data& data_;
        model_ptr<Object> attrs_;
    };
};

}