#include "attrlayer.h"

namespace mapget
{

AttributeLayer::AttributeLayer(
    simfil::ArrayIndex i,
    FeatureLayerConstPtr l,
    simfil::ModelNodeAddress a)
{
}

model_ptr<Attribute> AttributeLayer::newAttribute(const std::string_view& name)
{
    return mapget::model_ptr<Attribute>();
}

AttributeLayers::AttributeLayers(
    simfil::ArrayIndex i,
    FeatureLayerConstPtr l,
    simfil::ModelNodeAddress a)
{
}

model_ptr<Attribute> AttributeLayers::newLayer(const std::string_view& name)
{
    return mapget::model_ptr<Attribute>();
}

}