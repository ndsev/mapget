#include "feature.h"

namespace mapget
{

Feature::Feature(Feature::Data& d, FeatureLayerConstPtr l, simfil::ModelNodeAddress a)
{

}

model_ptr<FeatureId> Feature::id() const
{
    return {};
}

std::string mapget::Feature::typeId() const
{
    return std::string();
}

model_ptr<simfil::GeometryCollection> Feature::geom()
{
    return model_ptr<GeometryCollection>();
}

model_ptr<AttributeLayers> Feature::attributeLayers()
{
    return mapget::model_ptr<AttributeLayers>();
}

model_ptr<Object> Feature::attributes()
{
    return mapget::model_ptr<Object>();
}

model_ptr<Array> Feature::children()
{
    return mapget::model_ptr<Array>();
}

std::vector<simfil::Value> Feature::evaluate(const std::string_view& expression)
{
    return std::vector<simfil::Value>();
}

simfil::ValueType Feature::type() const
{
    return ModelNodeBase::type();
}

simfil::ModelNode::Ptr Feature::at(int64_t) const
{
    return ModelNodeBase::at(<unnamed>);
}

uint32_t Feature::size() const
{
    return ModelNodeBase::size();
}

simfil::ModelNode::Ptr Feature::get(const simfil::FieldId&) const
{
    return ModelNodeBase::get(<unnamed>);
}

simfil::FieldId Feature::keyAt(int64_t) const
{
    return ModelNodeBase::keyAt(<unnamed>);
}

void Feature::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    ModelNodeBase::iterate(cb);
}

Feature::FeaturePropertyView::FeaturePropertyView(
    Feature::Data& d,
    FeatureLayerConstPtr l,
    simfil::ModelNodeAddress a)
{
}

simfil::ValueType Feature::FeaturePropertyView::type() const
{
    return ModelNodeBase::type();
}
simfil::ModelNode::Ptr Feature::FeaturePropertyView::at(int64_t) const
{
    return ModelNodeBase::at(<unnamed>);
}
uint32_t Feature::FeaturePropertyView::size() const
{
    return ModelNodeBase::size();
}
simfil::ModelNode::Ptr Feature::FeaturePropertyView::get(const simfil::FieldId&) const
{
    return ModelNodeBase::get(<unnamed>);
}
simfil::FieldId Feature::FeaturePropertyView::keyAt(int64_t) const
{
    return ModelNodeBase::keyAt(<unnamed>);
}
void Feature::FeaturePropertyView::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    ModelNodeBase::iterate(cb);
}

}
