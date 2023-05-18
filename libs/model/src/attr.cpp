#include "attr.h"

namespace mapget
{

Attribute::Attribute(Attribute::Data& data, FeatureLayerConstPtr l, simfil::ModelNodeAddress a)
{

}

model_ptr<Geometry> Attribute::validity()
{
    return mapget::model_ptr<Geometry>();
}

void Attribute::setDirection(const Attribute::Direction& v)
{

}

Attribute::Direction Attribute::direction() const
{
    return Attribute::Both;
}

}