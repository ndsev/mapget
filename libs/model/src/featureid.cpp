#include "featureid.h"

namespace mapget
{

FeatureId::FeatureId(FeatureId::Data& data, FeatureLayerConstPtr l, simfil::ModelNodeAddress a)
{

}
std::string_view FeatureId::typeId() const
{
    return std::string_view();
}

std::string FeatureId::toString() const
{
    return std::string();
}

size_t FeatureId::numParts() const
{
    return 0;
}

std::pair<simfil::FieldId, simfil::ModelNode::Ptr> FeatureId::part(size_t i) const
{
    return std::pair<simfil::FieldId, simfil::ModelNode::Ptr>();
}

simfil::ValueType FeatureId::type() const
{
    return ModelNodeBase::type();
}

simfil::ScalarValueType FeatureId::value() const
{
    return ModelNodeBase::value();
}

simfil::ModelNode::Ptr FeatureId::at(int64_t) const
{
    return ModelNodeBase::at(<unnamed>);
}

uint32_t FeatureId::size() const
{
    return ModelNodeBase::size();
}

simfil::ModelNode::Ptr FeatureId::get(const simfil::FieldId&) const
{
    return ModelNodeBase::get(<unnamed>);
}

simfil::FieldId FeatureId::keyAt(int64_t) const
{
    return ModelNodeBase::keyAt(<unnamed>);
}

void FeatureId::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    ModelNodeBase::iterate(cb);
}

}