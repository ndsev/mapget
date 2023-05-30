#include "featureid.h"
#include "featurelayer.h"

#include <sstream>

namespace mapget
{

FeatureId::FeatureId(FeatureId::Data& data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(std::move(l), a),
      data_(data),
      fields_(model().resolveObject(Ptr::make(l, data_.idParts_)))
{
}

std::string_view FeatureId::typeId() const
{
    if (auto s = model().fieldNames()->resolve(data_.typeId_))
        return *s;
    return "err-unresolved-typename";
}

std::string FeatureId::toString() const
{
    std::stringstream result;
    result << typeId();

    auto addIdPart = [&result](auto&& v)
    {
        if constexpr (!std::is_same_v<std::decay_t<decltype(v)>, std::monostate>)
            result << "." << v;
    };

    // Add common id-part fields
    if (data_.useCommonTilePrefix_ && model().featureIdPrefix())
        for (auto const& [_, value] : (*model().featureIdPrefix())->fields())
            std::visit(addIdPart, value->value());

    // Add specific id-part fields
    for (auto const& [_, value] : fields())
        std::visit(addIdPart, value->value());

    return result.str();
}

simfil::ValueType FeatureId::type() const
{
    return simfil::ValueType::String;
}

simfil::ScalarValueType FeatureId::value() const
{
    return toString();
}

simfil::ModelNode::Ptr FeatureId::at(int64_t i) const
{
    return fields_->at(i);
}

uint32_t FeatureId::size() const
{
    return fields_->size();
}

simfil::ModelNode::Ptr FeatureId::get(const simfil::FieldId& f) const
{
    return fields_->get(f);
}

simfil::FieldId FeatureId::keyAt(int64_t i) const
{
    return fields_->keyAt(i);
}

bool FeatureId::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    return true;
}

}
