#include "attrlayer.h"
#include "featurelayer.h"
#include "mapget/log.h"

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
    auto result = static_cast<TileFeatureLayer&>(model()).newAttribute(name, initialCapacity);
    addAttribute(result);
    return result;
}

void AttributeLayer::addAttribute(model_ptr<Attribute> a)
{
    addField(a->name(), simfil::ModelNode::Ptr(a));
}

bool AttributeLayer::forEachAttribute(const std::function<bool(const model_ptr<Attribute>&)>& cb) const
{
    if (!cb)
        return false;
    for (auto const& [_, value] : fields()) {
        if (value->addr().column() != TileFeatureLayer::ColumnId::Attributes) {
            log().warn("Don't add anything other than Attributes into AttributeLayers!");
            continue;
        }
        auto attr = static_cast<TileFeatureLayer&>(model()).resolveAttribute(*value);
        if (!cb(attr))
            return false;
    }
    return true;
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

bool AttributeLayerList::forEachLayer(
    const std::function<bool(std::string_view, const model_ptr<AttributeLayer>&)>& cb) const
{
    if (!cb)
        return false;
    for(auto const& [fieldId, value] : fields()) {
        if (auto layerName = model().strings()->resolve(fieldId)) {
            if (value->addr().column() != TileFeatureLayer::ColumnId::AttributeLayers) {
                log().warn("Don't add anything other than AttributeLayers into AttributeLayerLists!");
                continue;
            }
            auto attrLayer = static_cast<TileFeatureLayer&>(model()).resolveAttributeLayer(*value);
            if (!cb(*layerName, attrLayer))
                return false;
        }
    }
    return true;
}

}
