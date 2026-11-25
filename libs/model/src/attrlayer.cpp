#include "attrlayer.h"
#include "feature.h"
#include "featurelayer.h"
#include "mapget/log.h"
#include "shapepoint-context.h"

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

namespace
{
nlohmann::json exportLayer(simfil::ModelNode const& node, AttributeLayer const& storage)
{
    auto result = nlohmann::json::object();
    auto isMultiMap = false;
    if (auto layerId = storage.id()) {
        result[std::string{AttributeLayer::InstanceIdField}] = *layerId;
    }

    for (auto const& [fieldId, value] : node.fields()) {
        auto fieldName = storage.borrowedModel()->lookupStringId(fieldId);
        if (!fieldName) {
            continue;
        }
        if (*fieldName == AttributeLayer::InstanceIdField) {
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

// Layer storage can be shared by several hosts. Carry the attachment across
// ordinary SIMFIL field traversal, without writing host IDs into the tile.
class AttachedLayer final : public simfil::Model
{
    using Ptr = simfil::ModelNode::Ptr;
    struct Node final : simfil::MandatoryDerivedModelNodeBase<AttachedLayer>
    {
        Node(simfil::ModelNode const& node, simfil::detail::mp_key key)
            : MandatoryDerivedModelNodeBase(node, key)
        {
        }
        simfil::ValueType type() const override { return simfil::ValueType::Object; }
        simfil::TypeId schema() const override { return model().storage_->schema(); }
        uint32_t size() const override { return model().storage_->size(); }
        simfil::StringId keyAt(int64_t i) const override { return model().storage_->keyAt(i); }
        Ptr at(int64_t i) const override { return model().bind(model().storage_->at(i)); }
        Ptr get(simfil::StringId const& k) const override
        {
            return model().bind(model().storage_->get(k));
        }
        bool iterate(IterCallback const& cb) const override
        {
            for (uint32_t i = 0; i < size(); ++i)
                if (!cb(*at(i)))
                    return false;
            return true;
        }
        nlohmann::json toJson() const override { return exportLayer(*this, *model().storage_); }
    };
    model_ptr<Feature> host_;
    model_ptr<AttributeLayer> storage_;
    simfil::StringId name_;
    Ptr bind(Ptr value) const
    {
        if (!value || value->addr().column() != PartitionFeatureLayer::ColumnId::Attributes)
            return value;
        auto attr = host_->model().resolve<Attribute>(*value);
        try {
            return attr->validityView(host_, *lookupStringId(name_)).node();
        }
        catch (PointQueryError const&) {
            if (!PointQueryError::Capture::recordCurrent())
                throw;
            // SIMFIL may be iterating through a noexcept callback before the public node exists.
            return shape::Context::scalar(std::monostate{});
        }
    }

public:
    AttachedLayer(model_ptr<Feature> host, model_ptr<AttributeLayer> storage, simfil::StringId name)
        : host_(std::move(host)), storage_(std::move(storage)), name_(name)
    {
    }
    static Ptr view(model_ptr<Feature> const& host, Ptr value, simfil::StringId name)
    {
        if (!host || !value ||
            value->addr().column() != PartitionFeatureLayer::ColumnId::AttributeLayers)
            return value;
        auto model = std::make_shared<
            AttachedLayer>(host, host->model().resolve<AttributeLayer>(*value), name);
        // The occurrence owner supplies the read protocol; the physical address
        // still lets typed consumers recover the stored AttributeLayer.
        return Ptr::make(std::move(model), value->addr());
    }
    std::optional<std::string_view> lookupStringId(simfil::StringId id) const override
    {
        return storage_->borrowedModel()->lookupStringId(id);
    }
    simfil::SchemaType resolveSchemaType(simfil::ModelNode const& node) const override
    {
        return node.addr() == storage_->addr() ?
            storage_->schemaType() :
            Model::resolveSchemaType(node);
    }
    tl::expected<void, simfil::Error>
    resolve(simfil::ModelNode const& node, ResolveFn const& cb) const override
    {
        if (node.addr() != storage_->addr())
            return Model::resolve(node, cb);
        cb(*model_ptr<Node>::make(node));
        return {};
    }
};
}  // namespace

nlohmann::json AttributeLayer::toJson() const
{
    return exportLayer(*this, *this);
}

AttributeLayer::AttributeLayer(
    simfil::ArrayIndex i,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::BaseObject<PartitionFeatureLayer, Attribute>(i, std::move(l), a, key)
{
}

model_ptr<Attribute>
AttributeLayer::newAttribute(
    const std::string_view& name,
    size_t initialCapacity,
    bool fixedSize)
{
    auto result =
        static_cast<PartitionFeatureLayer&>(model()).newAttribute(name, initialCapacity, fixedSize);
    addAttribute(result);
    return result;
}

void AttributeLayer::addAttribute(model_ptr<Attribute> a)
{
    if (a) {
        auto schemaId =
            model().childSchemaId(schema(), a->name(), simfil::SchemaType::Kind::Object);
        model().applyObjectSchema(*a, schemaId);
    }
    addField(a->name(), a);
}

bool AttributeLayer::forEachAttribute(const std::function<bool(const model_ptr<Attribute>&)>& cb) const
{
    if (!cb)
        return false;
    for (auto const& [_, value] : fields()) {
        if (value->addr().column() != PartitionFeatureLayer::ColumnId::Attributes) {
            if (_ == StringPool::IdStr) {
                continue;
            }
            log().warn("Don't add anything other than Attributes into AttributeLayers!");
            continue;
        }
        auto attr = static_cast<PartitionFeatureLayer&>(model()).resolve<Attribute>(*value);
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
    auto result = modelPtr<PartitionFeatureLayer>()->newAttributeLayer(initialCapacity, fixedSize);
    addLayer(name, result);
    return result;
}

void AttributeLayerList::addLayer(const std::string_view& name, model_ptr<AttributeLayer> l)
{
    if (isFeatureScopedView()) {
        raise("Cannot mutate a feature-scoped AttributeLayerList view.");
    }
    if (l) {
        auto schemaId = model().childSchemaId(schema(), name, simfil::SchemaType::Kind::Object);
        if (schemaId != simfil::NoTypeId) {
            auto const target =
                model().checkedContainerSchema(l->schema(), schemaId, simfil::ValueType::Object);
            if (auto result = l->setSchema(target); !result) {
                raise(result.error().message);
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
                if (value->addr().column() != PartitionFeatureLayer::ColumnId::AttributeLayers) {
                    log().warn("Don't add anything other than AttributeLayers into AttributeLayerLists!");
                    continue;
                }
                auto attrLayer =
                    static_cast<PartitionFeatureLayer&>(model()).resolve<AttributeLayer>(*value);
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

    auto feature = featureScopedFeature();
    forEachLayer([&](std::string_view layerName, model_ptr<AttributeLayer> const& layer) {
        auto const layerKey = std::string{layerName};
        auto layerValue = feature ?
            AttachedLayer::view(feature, layer, model().strings()->get(layerName))->toJson() :
            layer->toJson();
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
    return addr_.column() == PartitionFeatureLayer::ColumnId::FeatureAttributeLayerListView;
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
    return {};
}

simfil::TypeId AttributeLayerList::schema() const
{
    if (auto local = localObject()) {
        return local->schema();
    }
    if (auto feature = featureScopedFeature()) {
        return model().attributeLayerMapSchemaId(feature->typeId());
    }
    return simfil::NoTypeId;
}

tl::expected<void, simfil::Error> AttributeLayerList::setObjectSchema(simfil::TypeId schemaId)
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
    if (isFeatureScopedView())
        return at(i);
    auto local = localObject();
    return local ? local->at(i) : simfil::ModelNode::Ptr{};
}

bool AttributeLayerList::localMergedIterate(simfil::ModelNode::IterCallback const& cb) const
{
    if (isFeatureScopedView())
        return iterate(cb);
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
        auto value = local->at(i);
        return isFeatureScopedView() ?
            AttachedLayer::view(featureScopedFeature(), value, local->keyAt(i)) :
            value;
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
            return isFeatureScopedView() ?
                AttachedLayer::view(featureScopedFeature(), localValue, field) :
                localValue;
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
    if (isFeatureScopedView()) {
        auto count = size();
        for (uint32_t i = 0; i < count; ++i)
            if (!cb(*at(i)))
                return false;
        return true;
    }
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
