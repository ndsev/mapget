#include "attrlayer.h"
#include "feature.h"
#include "featurelayer.h"
#include "mapget/log.h"

namespace mapget
{

void AttributeLayer::setId(uint64_t id)
{
    auto value = model().newValue(static_cast<int64_t>(id));
    auto replaced = false;
    storage_->iterate(members_, [&](auto&& member) {
        if (simfil::detail::objectFieldName(member) == StringPool::IdStr) {
            simfil::detail::objectFieldNode(member) = value->addr();
            replaced = true;
            return false;
        }
        return true;
    });
    if (!replaced) {
        storage_->emplace_back(members_, StringPool::IdStr, value->addr());
    }
}

std::optional<uint64_t> AttributeLayer::id() const
{
    auto valueNode = get(StringPool::IdStr);
    if (!valueNode) {
        return {};
    }

    auto value = valueNode->value();
    if (auto const* signedValue = std::get_if<int64_t>(&value)) {
        if (*signedValue < 0) {
            return {};
        }
        return static_cast<uint64_t>(*signedValue);
    }
    return {};
}

nlohmann::json AttributeLayer::toJson() const
{
    auto result = nlohmann::json::object();
    auto isMultiMap = false;
    if (auto layerId = id()) {
        result[std::string{InstanceIdField}] = *layerId;
    }

    for (auto const& [fieldId, value] : fields()) {
        auto fieldName = model().strings()->resolve(fieldId);
        if (!fieldName) {
            continue;
        }
        if (*fieldName == InstanceIdField) {
            continue;
        }

        auto const fieldKey = std::string{*fieldName};
        auto fieldValue = value->toJson();
        if (result.contains(fieldKey)) {
            isMultiMap = true;
            if (!result[fieldKey].is_array()) {
                result[fieldKey] = nlohmann::json::array({std::move(result[fieldKey])});
            }
            result[fieldKey].push_back(std::move(fieldValue));
        }
        else {
            result[fieldKey] = std::move(fieldValue);
        }
    }

    if (isMultiMap) {
        result["_multimap"] = true;
    }
    return result;
}

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
            if (_ == StringPool::IdStr) {
                continue;
            }
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
    if (isFeatureScopedView()) {
        raise("Cannot mutate a feature-scoped AttributeLayerList view.");
    }
    auto result = modelPtr<TileFeatureLayer>()->newAttributeLayer(initialCapacity, fixedSize);
    addLayer(name, result);
    return result;
}

void AttributeLayerList::addLayer(const std::string_view& name, model_ptr<AttributeLayer> l)
{
    if (isFeatureScopedView()) {
        raise("Cannot mutate a feature-scoped AttributeLayerList view.");
    }
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
    if (isFeatureScopedView()) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InternalError,
            "Cannot mutate a feature-scoped AttributeLayerList view."});
    }
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
    if (local) {
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
    }
    if (auto ext = mergedExtension()) {
        return ext->forEachLayer(cb);
    }
    return true;
}

nlohmann::json AttributeLayerList::toJson() const
{
    auto result = nlohmann::json::object();
    auto isMultiMap = false;

    forEachLayer([&](std::string_view layerName, model_ptr<AttributeLayer> const& layer) {
        auto const layerKey = std::string{layerName};
        auto layerValue = layer->toJson();
        if (result.contains(layerKey)) {
            isMultiMap = true;
            if (!result[layerKey].is_array()) {
                result[layerKey] = nlohmann::json::array({std::move(result[layerKey])});
            }
            result[layerKey].push_back(std::move(layerValue));
        }
        else {
            result[layerKey] = std::move(layerValue);
        }
        return true;
    });

    if (isMultiMap) {
        result["_multimap"] = true;
    }
    return result;
}

simfil::model_ptr<simfil::Object> AttributeLayerList::localObject() const
{
    if (isFeatureScopedView()) {
        auto localList = localConcreteList();
        return localList ? localList->localObject() : simfil::model_ptr<simfil::Object>{};
    }
    return simfil::model_ptr<simfil::Object>::make(members_, model_, addr_);
}

simfil::model_ptr<AttributeLayerList> AttributeLayerList::localConcreteList() const
{
    auto feature = featureScopedFeature();
    if (!feature) {
        return {};
    }
    auto const localAddress = feature->attributeLayerNodeAddress();
    return localAddress ? model().resolve<AttributeLayerList>(localAddress) : simfil::model_ptr<AttributeLayerList>{};
}

bool AttributeLayerList::isFeatureScopedView() const
{
    return addr_.column() == TileFeatureLayer::ColumnId::FeatureAttributeLayerListView;
}

model_ptr<Feature> AttributeLayerList::featureScopedFeature() const
{
    if (!isFeatureScopedView()) {
        return {};
    }
    auto rootResult = model().root(addr().index());
    if (!rootResult || !*rootResult) {
        return {};
    }
    return model().resolve<Feature>(**rootResult);
}

AttributeLayerList::ExtensionPtr AttributeLayerList::mergedExtension() const
{
    if (!isFeatureScopedView()) {
        return {};
    }
    auto overlay = model().overlay();
    if (!overlay || addr().index() >= overlay->size()) {
        return {};
    }
    return overlay->resolve<AttributeLayerList>(
        simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::FeatureAttributeLayerListView, addr().index()});
}

simfil::SchemaId AttributeLayerList::schema() const
{
    if (auto local = localObject()) {
        return local->schema();
    }
    if (auto feature = featureScopedFeature()) {
        return model().attributeLayerMapSchemaId(feature->typeId());
    }
    return simfil::NoSchemaId;
}

tl::expected<void, simfil::Error> AttributeLayerList::setObjectSchema(simfil::SchemaId schemaId)
{
    auto local = localObject();
    if (!local) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InternalError,
            "Cannot assign schema to an empty feature-scoped AttributeLayerList view."});
    }
    return local->setSchema(schemaId);
}

uint32_t AttributeLayerList::localMergedSize() const
{
    auto local = localObject();
    return local ? local->size() : 0;
}

simfil::ModelNode::Ptr AttributeLayerList::localMergedAt(int64_t i) const
{
    auto local = localObject();
    return local ? local->at(i) : simfil::ModelNode::Ptr{};
}

bool AttributeLayerList::localMergedIterate(simfil::ModelNode::IterCallback const& cb) const
{
    auto local = localObject();
    return local ? local->iterate(cb) : true;
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

    auto local = localObject();
    auto localSize = local ? static_cast<int64_t>(local->size()) : 0;
    if (i < localSize) {
        return local->at(i);
    }

    if (auto ext = mergedExtension()) {
        return ext->at(i - localSize);
    }
    return {};
}

uint32_t AttributeLayerList::size() const
{
    auto local = localObject();
    auto result = local ? local->size() : 0;
    if (auto ext = mergedExtension()) {
        result += ext->size();
    }
    return result;
}

simfil::ModelNode::Ptr AttributeLayerList::get(const simfil::StringId& field) const
{
    auto local = localObject();
    if (local) {
        if (auto localValue = local->get(field)) {
            return localValue;
        }
    }
    if (auto ext = mergedExtension()) {
        return ext->get(field);
    }
    return {};
}

simfil::StringId AttributeLayerList::keyAt(int64_t i) const
{
    if (i < 0) {
        return {};
    }

    auto local = localObject();
    auto localSize = local ? static_cast<int64_t>(local->size()) : 0;
    if (i < localSize) {
        return local->keyAt(i);
    }

    if (auto ext = mergedExtension()) {
        return ext->keyAt(i - localSize);
    }
    return {};
}

bool AttributeLayerList::iterate(simfil::ModelNode::IterCallback const& cb) const
{
    auto local = localObject();
    if (local && !local->iterate(cb)) {
        return false;
    }
    if (auto ext = mergedExtension()) {
        return ext->iterate(cb);
    }
    return true;
}

}
