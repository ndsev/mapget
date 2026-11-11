#include "mapschema-impl.h"

namespace mapget
{

/** Read attribute-use metadata directly from stable rows in this same schema pool. */
class MapSchema::Impl::OwnerDescriptor final : public simfil::ModelNodeBase
{
public:
    /** Support model_ptr's empty representation without inventing a metadata owner. */
    explicit OwnerDescriptor(simfil::detail::mp_key key) : ModelNodeBase(key) {}

    /** Bind either an owner row or a type's owner-list address. */
    OwnerDescriptor(
        simfil::ModelConstPtr model,
        ModelNodeAddress address,
        simfil::detail::mp_key key)
        : ModelNodeBase(std::move(model), address, key)
    {
    }

    /** Owner memberships are arrays; individual use sites are metadata objects. */
    simfil::ValueType type() const override
    {
        return addr_.column() == AttributeOwners ?
            simfil::ValueType::Array :
            simfil::ValueType::Object;
    }

    /** Metadata does not inherit the represented feature's runtime schema. */
    TypeId schema() const override { return simfil::NoTypeId; }

    /** Count members without materializing a DTO or a descriptor graph. */
    uint32_t size() const override
    {
        auto const& graph = metadata();
        return addr_.column() == AttributeOwners ?
            uint32_t(graph.owners.size(graph.data(TypeId(addr_.index())).owners)) :
            3u + bool(graph.ownerRows[addr_.index()].featureLayer);
    }

    /** Enumerate the same keys used by explicit lookup, omitting unknown feature layers. */
    StringId keyAt(int64_t position) const override
    {
        if (addr_.column() == AttributeOwners || position < 0 || uint64_t(position) >= size())
            return simfil::StringPool::Empty;
        auto const& keys = metadata().descriptorKeys;
        return std::array<StringId, 4>{
            keys[19],
            StringPool::LayerStr,
            StringPool::NameStr,
            keys[20]}[position];
    }

    /** Resolve one metadata field without interning its name. */
    simfil::ModelNode::Ptr get(StringId const& key) const override
    {
        if (!key || addr_.column() == AttributeOwners)
            return {};
        for (uint32_t i = 0; i < size(); ++i)
            if (keyAt(i) == key)
                return at(i);
        return {};
    }

    /** Follow membership IDs or borrow one dictionary-backed textual field. */
    simfil::ModelNode::Ptr at(int64_t position) const override
    {
        if (position < 0 || uint64_t(position) >= size())
            return {};
        auto const& graph = metadata();
        if (addr_.column() == AttributeOwners) {
            auto ids = graph.owners.range(graph.data(TypeId(addr_.index())).owners);
            return simfil::model_ptr<OwnerDescriptor>::make(
                model_,
                ModelNodeAddress{AttributeOwner, ids[size_t(position)]});
        }
        auto const& row = graph.ownerRows[addr_.index()];
        auto id =
            std::array<StringId, 4>{row.feature, row.layer, row.name, row.featureLayer}[position];
        return graph.pool.simfil::Model::resolve(
            ModelNodeAddress(simfil::Model::Scalar),
            simfil::ScalarValueType{graph.name(id)});
    }

    /** Preserve membership order and callback short-circuiting. */
    bool iterate(IterCallback const& callback) const override
    {
        for (uint32_t i = 0; i < size(); ++i)
            if (!callback(*at(i)))
                return false;
        return true;
    }

private:
    /** Borrow the owner already retained by this ordinary ModelNode. */
    Impl const& metadata() const { return *static_cast<MapSchema const&>(*model_).schema_; }
};

std::array<simfil::ModelNodeAddress, 6> MapSchema::descriptorMetadata(simfil::TypeId id) const
{
    if (!schema_->valid(id))
        return {};
    auto const& row = std::as_const(*schema_).data(id);
    return {
        row.key,
        row.pointer,
        row.metaType,
        row.zserioType,
        row.attributeType,
        row.attributeCode};
}

uint32_t MapSchema::descriptorExtensionSize(simfil::TypeId id) const
{
    auto values = descriptorMetadata(id);
    return uint32_t(std::ranges::count_if(values, [](auto address) { return bool(address); })) +
        bool(schema_->valid(id) && std::as_const(*schema_).data(id).owners);
}

simfil::StringId MapSchema::descriptorExtensionKeyAt(simfil::TypeId id, uint32_t position) const
{
    auto values = descriptorMetadata(id);
    for (size_t i = 0; i < values.size(); ++i) {
        if (!values[i])
            continue;
        if (!position--)
            return schema_->descriptorKeys[12 + i];
    }
    if (!position && schema_->valid(id) && std::as_const(*schema_).data(id).owners)
        return schema_->descriptorKeys[18];
    return simfil::StringPool::Empty;
}

simfil::ModelNode::Ptr
MapSchema::descriptorExtensionGet(simfil::TypeId id, simfil::StringId key) const
{
    if (!key)
        return {};
    auto values = descriptorMetadata(id);
    for (size_t i = 0; i < values.size(); ++i)
        if (values[i] && schema_->descriptorKeys[12 + i] == key)
            return resolve(values[i]);
    if (key == schema_->descriptorKeys[18] && schema_->valid(id) &&
        std::as_const(*schema_).data(id).owners)
        return simfil::model_ptr<Impl::OwnerDescriptor>::make(
            shared_from_this(),
            ModelNodeAddress{Impl::AttributeOwners, id});
    return {};
}

tl::expected<void, simfil::Error>
MapSchema::resolveAttributeOwners(simfil::ModelNode const& node, ResolveFn const& callback) const
{
    auto address = node.addr();
    bool valid = false;
    if (finalized()) {
        if (address.column() == Impl::AttributeOwners)
            valid = address.index() <= simfil::MaxTypeId && schema_->valid(TypeId(address.index()));
        else if (address.column() == Impl::AttributeOwner)
            valid = address.index() < schema_->ownerRows.size() &&
                schema_->ownerRows[address.index()].feature;
    }
    if (!valid)
        return tl::unexpected(simfil::Error{
            simfil::Error::IndexOutOfRange,
            "Invalid or unpublished attribute-owner descriptor."});
    auto view = simfil::model_ptr<Impl::OwnerDescriptor>::make(shared_from_this(), address);
    callback(*view);
    return {};
}

}  // namespace mapget
