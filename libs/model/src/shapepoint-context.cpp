#include "shapepoint-context.h"
#include "mapget/model/shapepoint.h"
#include "simfil/model/schema-pool.h"

namespace mapget::shape
{
struct Context::Node final : simfil::MandatoryDerivedModelNodeBase<Context>
{
    Node(simfil::ModelNode const& node, simfil::detail::mp_key key)
        : MandatoryDerivedModelNodeBase(node, key)
    {
    }
    simfil::ValueType type() const override
    {
        return model().size_ ? simfil::ValueType::Array : simfil::ValueType::Object;
    }
    simfil::TypeId schema() const override { return model().type_.id(); }
    // Compound projection copies child nodes directly, without SIMFIL's scalar
    // field inference. Carry that same edge on inline values and virtual children.
    Ptr child(Ptr value, std::optional<Id> key) const
    {
        if (!value || !model().type_ || value->schemaType())
            return value;
        auto kind = value->type();
        if (kind == simfil::ValueType::Null || kind == simfil::ValueType::Undef)
            return value;
        auto compound = kind == simfil::ValueType::Array || kind == simfil::ValueType::Object;
        auto source = compound ? dynamic_cast<Context const*>(value->borrowedModel()) : nullptr;
        if (compound && !source)
            return value;  // Raw producer containers keep their own storage and namespace.
        auto name = key ? model().strings_->resolve(*key) : std::nullopt;
        auto type = key ? (name ? model().type_.field(*name).type() : simfil::SchemaType{}) :
                          model().type_.itemType();
        if (!type)
            return value;
        if (source) {
            // The untyped sample/candidate may be shared; qualify this use, not its storage.
            auto view = std::make_shared<Context>(source->strings_, type);
            view->fields_ = source->fields_;
            view->size_ = source->size_;
            view->at_ = source->at_;
            view->lookup_ = source->lookup_;
            return Ptr::make(std::move(view), value->addr());
        }
        auto scalar = value->value();
        if (auto text = std::get_if<std::string_view>(&scalar))
            scalar = std::string(*text);
        return Ptr::make(owningModel(), simfil::ModelNodeAddress{Model::Scalar, type.id()},
                         std::move(scalar));
    }
    uint32_t size() const override
    try {
        return model().size_ ? model().size_() : static_cast<uint32_t>(model().fields_.size());
    }
    catch (PointQueryError const&) {
        if (!PointQueryError::Capture::recordCurrent())
            throw;
        return 0;
    }
    simfil::StringId keyAt(int64_t i) const override
    {
        return !model().size_ && i >= 0 && static_cast<uint64_t>(i) < model().fields_.size() ?
            model().fields_[i].first :
            simfil::StringPool::Empty;
    }
    Ptr at(int64_t i) const override
    try {
        if (i < 0 || static_cast<uint64_t>(i) >= size())
            return {};
        auto result = model().at_ ? model().at_(i) : model().fields_[i].second();
        return result ? child(result, model().size_ ? std::nullopt : std::optional{keyAt(i)}) :
                        Context::scalar(std::monostate{});
    }
    catch (PointQueryError const&) {
        if (!PointQueryError::Capture::recordCurrent())
            throw;
        return Context::scalar(std::monostate{});
    }
    Ptr get(simfil::StringId const& key) const override
    try {
        for (auto const& [id, read] : model().fields_)
            if (id == key) {
                auto node = read();
                return node ? child(node, key) : Context::scalar(std::monostate{});
            }
        return model().lookup_ ? child(model().lookup_(key), key) : Ptr{};
    }
    catch (PointQueryError const&) {
        if (!PointQueryError::Capture::recordCurrent())
            throw;
        return Context::scalar(std::monostate{});
    }
    bool iterate(IterCallback const& cb) const override
    {
        auto count = size();
        for (uint32_t i = 0; i < count; ++i) {
            auto item = at(i);
            if (!item || !cb(*item))
                return false;
        }
        return true;
    }
};

simfil::SchemaType Context::resolveSchemaType(simfil::ModelNode const& node) const
{
    if (node.addr().column() == Scalar && type_)
        return type_.pool()->schemaType(static_cast<simfil::TypeId>(node.addr().index()));
    return node.addr().column() == FirstNontrivialColumnId ? type_ : simfil::SchemaType{};
}

tl::expected<void, simfil::Error>
Context::resolve(simfil::ModelNode const& node, ResolveFn const& cb) const
{
    if (node.addr().column() != FirstNontrivialColumnId)
        return Model::resolve(node, cb);
    cb(*simfil::model_ptr<Node>::make(node));
    return {};
}

Context::Ptr
Context::object(Dictionary strings, Fields fields, Lookup lookup, simfil::SchemaType type)
{
    auto model = std::make_shared<Context>(std::move(strings), std::move(type));
    model->fields_ = std::move(fields);
    model->lookup_ = std::move(lookup);
    return Ptr::make(std::move(model), simfil::ModelNodeAddress{FirstNontrivialColumnId});
}

Context::Ptr Context::array(
    Dictionary strings,
    std::function<uint32_t()> size,
    std::function<Ptr(uint32_t)> at,
    Lookup lookup,
    simfil::SchemaType type)
{
    auto model = std::make_shared<Context>(std::move(strings), std::move(type));
    model->size_ = std::move(size);
    model->at_ = std::move(at);
    model->lookup_ = std::move(lookup);
    return Ptr::make(std::move(model), simfil::ModelNodeAddress{FirstNontrivialColumnId});
}

Context::Ptr Context::array(
    Dictionary strings,
    std::vector<Ptr> elements,
    Lookup lookup,
    simfil::SchemaType type)
{
    auto count = static_cast<uint32_t>(elements.size());
    return array(
        std::move(strings),
        [count] { return count; },
        [items = std::move(elements)](uint32_t i) { return items[i]; },
        std::move(lookup),
        std::move(type));
}
}  // namespace mapget::shape
