#include "attrlayer.h"
#include "featurelayer.h"

namespace mapget
{

AttributeLayer::AttributeLayer(
    simfil::ArrayIndex i,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a
)
    : simfil::Object(i, std::move(l), a)
{
}

model_ptr<Attribute>
AttributeLayer::newAttribute(const std::string_view& name, size_t initialCapacity)
{
    auto result = reinterpret_cast<TileFeatureLayer&>(model()).newAttribute(name, initialCapacity);
    addAttribute(result);
    return result;
}

void AttributeLayer::addAttribute(model_ptr<Attribute> a)
{
    addField(a->name(), simfil::ModelNode::Ptr(a));
}

AttributeLayerList::AttributeLayerList(
    simfil::ArrayIndex i,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a
)
    : simfil::Object(i, std::move(l), a)
{
}

model_ptr<AttributeLayer>
AttributeLayerList::newLayer(const std::string_view& name, size_t initialCapacity)
{
    auto result = modelPtr<TileFeatureLayer>()->newAttributeLayer(initialCapacity);
    addLayer(name, result);
    return result;
}

void AttributeLayerList::addLayer(const std::string_view& name, model_ptr<AttributeLayer> l)
{
    addField(name, simfil::ModelNode::Ptr(std::move(l)));
}

}