#include "attrlayer.h"
#include "featurelayer.h"
#include "mapget/log.h"

namespace mapget
{

AttributeLayer::AttributeLayer(
    simfil::ArrayIndex i,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key
)
    : simfil::BaseObject<TileFeatureLayer, Attribute>(i, std::move(l), a, key)
{
}

model_ptr<Attribute>
AttributeLayer::newAttribute(
    const std::string_view& name,
    size_t initialCapacity,
    bool fixedSize)
{
    auto result = static_cast<TileFeatureLayer&>(model()).newAttribute(
        name,
        initialCapacity,
        fixedSize);
    addAttribute(result);
    return result;
}

void AttributeLayer::addAttribute(model_ptr<Attribute> a)
{
    if (a) {
        auto schemaId = model().childSchemaId(
            schema(),
            a->name(),
            simfil::Schema::Kind::Object);
        model().applyObjectSchema(*a, schemaId);
    }
    addField(a->name(), a);
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
        auto attr = static_cast<TileFeatureLayer&>(model()).resolve<Attribute>(*value);
        if (!cb(attr))
            return false;
    }
    return true;
}

AttributeLayerList::AttributeLayerList(
    simfil::ArrayIndex i,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key
)
    : MergedArrayView<AttributeLayerList, AttributeLayer>(std::move(l), a, key),
      members_(i)
{
}

model_ptr<AttributeLayer>
AttributeLayerList::newLayer(
    const std::string_view& name,
    size_t initialCapacity,
    bool fixedSize)
{
    auto result = modelPtr<TileFeatureLayer>()->newAttributeLayer(initialCapacity, fixedSize);
    addLayer(name, result);
    return result;
}

void AttributeLayerList::addLayer(const std::string_view& name, model_ptr<AttributeLayer> l)
{
    if (l) {
        auto schemaId = model().childSchemaId(
            schema(),
            name,
            simfil::Schema::Kind::Object);
        if (schemaId != simfil::NoSchemaId) {
            // A shared attribute layer may be reached through multiple feature schemas.
            auto const current = l->schema();
            auto const target = current == simfil::NoSchemaId || current == schemaId
                ? schemaId
                : simfil::NoSchemaId;
            if (auto result = l->setSchema(target); !result) {
                log().warn("Failed to set attribute layer schema: {}", result.error().message);
            }
        }
    }
    (void) localObject()->addField(name, l);
}

tl::expected<std::reference_wrapper<AttributeLayerList>, simfil::Error>
AttributeLayerList::addField(std::string_view const& name, model_ptr<AttributeLayer> l)
{
    auto result = localObject()->addField(name, l);
    if (!result) {
        return tl::unexpected(result.error());
    }
    return std::ref(*this);
}

bool AttributeLayerList::forEachLayer(
    const std::function<bool(std::string_view, const model_ptr<AttributeLayer>&)>& cb) const
{
    if (!cb)
        return false;
    auto local = localObject();
    for (auto const& [stringId, value] : local->fields()) {
        if (auto layerName = model().strings()->resolve(stringId)) {
            if (value->addr().column() != TileFeatureLayer::ColumnId::AttributeLayers) {
                log().warn("Don't add anything other than AttributeLayers into AttributeLayerLists!");
                continue;
            }
            auto attrLayer = static_cast<TileFeatureLayer&>(model()).resolve<AttributeLayer>(*value);
            if (!cb(*layerName, attrLayer))
                return false;
        }
    }
    if (auto ext = extension()) {
        return ext->forEachLayer(cb);
    }
    return true;
}

simfil::model_ptr<simfil::Object> AttributeLayerList::localObject() const
{
    return simfil::model_ptr<simfil::Object>::make(members_, model_, addr_);
}

simfil::SchemaId AttributeLayerList::schema() const
{
    return localObject()->schema();
}

tl::expected<void, simfil::Error> AttributeLayerList::setObjectSchema(simfil::SchemaId schemaId)
{
    return localObject()->setSchema(schemaId);
}

uint32_t AttributeLayerList::localMergedSize() const
{
    return localObject()->size();
}

simfil::ModelNode::Ptr AttributeLayerList::localMergedAt(int64_t i) const
{
    return localObject()->at(i);
}

bool AttributeLayerList::localMergedIterate(simfil::ModelNode::IterCallback const& cb) const
{
    return localObject()->iterate(cb);
}

simfil::ValueType AttributeLayerList::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr AttributeLayerList::at(int64_t i) const
{
    if (i < 0) {
        return {};
    }

    auto localSize = static_cast<int64_t>(localObject()->size());
    if (i < localSize) {
        return localObject()->at(i);
    }

    if (auto ext = extension()) {
        return ext->at(i - localSize);
    }
    return {};
}

uint32_t AttributeLayerList::size() const
{
    auto result = localObject()->size();
    if (auto ext = extension()) {
        result += ext->size();
    }
    return result;
}

simfil::ModelNode::Ptr AttributeLayerList::get(const simfil::StringId& field) const
{
    auto local = localObject()->get(field);
    if (local) {
        return local;
    }
    if (auto ext = extension()) {
        return ext->get(field);
    }
    return {};
}

simfil::StringId AttributeLayerList::keyAt(int64_t i) const
{
    if (i < 0) {
        return {};
    }

    auto localSize = static_cast<int64_t>(localObject()->size());
    if (i < localSize) {
        return localObject()->keyAt(i);
    }

    if (auto ext = extension()) {
        return ext->keyAt(i - localSize);
    }
    return {};
}

bool AttributeLayerList::iterate(simfil::ModelNode::IterCallback const& cb) const
{
    if (!localObject()->iterate(cb)) {
        return false;
    }
    if (auto ext = extension()) {
        return ext->iterate(cb);
    }
    return true;
}

}
