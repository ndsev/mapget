#include "shapepoint-context.h"
#include "mapget/model/shapepoint.h"

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
        return result ? result : Context::scalar(std::monostate{});
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
                return node ? node : Context::scalar(std::monostate{});
            }
        return model().lookup_ ? model().lookup_(key) : Ptr{};
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
