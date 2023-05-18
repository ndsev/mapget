#pragma once

#include "featureid.h"
#include "attr.h"

namespace mapget
{

class AttributeLayer : protected simfil::Object
{
    friend class TileFeatureLayer;
    friend class bitsery::Access;

    model_ptr<Attribute> newAttribute(std::string_view const& name);

protected:
    AttributeLayer(simfil::ArrayIndex i, FeatureLayerConstPtr l, simfil::ModelNodeAddress a);
};

class AttributeLayers : protected simfil::Object
{
    friend class ModelPool;
    friend class bitsery::Access;

    model_ptr<Attribute> newLayer(std::string_view const& name);

protected:
    AttributeLayers(simfil::ArrayIndex i, FeatureLayerConstPtr l, simfil::ModelNodeAddress a);
};

}