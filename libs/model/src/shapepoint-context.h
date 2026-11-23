#pragma once

#include "simfil/model/model.h"
#include "simfil/model/schema-type.h"

namespace mapget::shape
{

// Context objects have a fixed enumerable surface and optional named-only edges.
// Addresses survive ModelNode slicing because the model, not the wrapper, owns the readers.
class Context final : public simfil::Model
{
    using Id = simfil::StringId;

public:
    using Ptr = simfil::ModelNode::Ptr;
    using Dictionary = std::shared_ptr<simfil::StringPool>;
    using Getter = std::function<Ptr()>;
    using Lookup = std::function<Ptr(Id)>;
    using Fields = std::vector<std::pair<Id, Getter>>;

    static Ptr
    object(Dictionary strings, Fields fields, Lookup lookup = {}, simfil::SchemaType type = {});
    static Ptr array(
        Dictionary strings,
        std::vector<Ptr> elements,
        Lookup lookup = {},
        simfil::SchemaType type = {});
    static Ptr array(
        Dictionary strings,
        std::function<uint32_t()> size,
        std::function<Ptr(uint32_t)> at,
        Lookup lookup = {},
        simfil::SchemaType type = {});
    static Ptr scalar(simfil::ScalarValueType v)
    {
        static auto const model = std::make_shared<simfil::Model>();
        return simfil::model_ptr<simfil::ValueNode>::make(v, model);
    }

    Context(Dictionary strings, simfil::SchemaType type)
        : strings_(std::move(strings)), type_(type.retain())
    {
    }
    simfil::SchemaType resolveSchemaType(simfil::TypeId id) const override
    {
        return type_.id() == id ? type_ : simfil::SchemaType{};
    }
    std::optional<std::string_view> lookupStringId(Id id) const override
    {
        return strings_->resolve(id);
    }
    tl::expected<void, simfil::Error>
    resolve(simfil::ModelNode const& node, ResolveFn const& cb) const override;

private:
    struct Node;
    Dictionary strings_;
    simfil::SchemaType type_;
    Fields fields_;
    std::function<uint32_t()> size_;
    std::function<Ptr(uint32_t)> at_;
    Lookup lookup_;
};

}  // namespace mapget::shape
