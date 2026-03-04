#include "featureid.h"
#include "featurelayer.h"

#include <algorithm>
#include <sstream>

#include "mapget/log.h"

namespace mapget
{

namespace
{
std::vector<simfil::StringId> resolvePartNames(
    TileFeatureLayer const& model,
    simfil::StringId typeId,
    uint8_t idCompositionOffset,
    uint32_t numLocalParts)
{
    std::vector<simfil::StringId> names;
    names.reserve(numLocalParts);
    if (numLocalParts == 0) {
        return names;
    }

    auto typeName = model.strings()->resolve(typeId);
    if (!typeName) {
        return names;
    }

    auto typeInfo = model.layerInfo()->getTypeInfo(*typeName, false);
    if (!typeInfo || typeInfo->uniqueIdCompositions_.empty()) {
        return names;
    }

    auto const& primaryComposition = typeInfo->uniqueIdCompositions_.front();
    uint32_t compositionIndex = std::min<uint32_t>(
        idCompositionOffset,
        static_cast<uint32_t>(primaryComposition.size()));

    while (compositionIndex < primaryComposition.size() &&
           names.size() < static_cast<size_t>(numLocalParts)) {
        auto sid = model.strings()->emplace(primaryComposition[compositionIndex].idPartLabel_);
        if (!sid) {
            break;
        }
        names.push_back(*sid);
        ++compositionIndex;
    }

    return names;
}

template<typename Fn>
void appendTypedKeyValue(
    TileFeatureLayer const& model,
    simfil::StringId key,
    simfil::ModelNode::Ptr const& valueNode,
    Fn&& fn)
{
    auto keyStr = model.strings()->resolve(key);
    if (!keyStr || !valueNode) {
        return;
    }

    std::visit(
        [&](auto&& v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, simfil::ByteArray>) {
                raiseFmt("FeatureId part '{}' cannot be a ByteArray.", *keyStr);
            }
            else if constexpr (!std::is_same_v<T, std::monostate> && !std::is_same_v<T, double>) {
                fn(*keyStr, v);
            }
        },
        valueNode->value());
}

void appendNodeValueToString(std::stringstream& out, simfil::ModelNode::Ptr const& node)
{
    if (!node) {
        return;
    }

    std::visit(
        [&out](auto&& v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, simfil::ByteArray>) {
                raiseFmt("FeatureId part value 'b\"{}\"' cannot be a ByteArray.", v.toHex());
            }
            else if constexpr (!std::is_same_v<T, std::monostate>) {
                out << "." << v;
            }
        },
        node->value());
}
}

FeatureId::FeatureId(FeatureId::Data& data,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(l, a, key),
      data_(data)
{
    if (data_.idPartValues_ != simfil::InvalidArrayIndex) {
        values_ = model().resolve<simfil::Array>(
            simfil::ModelNodeAddress{
                simfil::ModelPool::ColumnId::Arrays,
                static_cast<uint32_t>(data_.idPartValues_)});
    }

    partNames_ = resolvePartNames(
        model(),
        data_.typeId_,
        data_.idCompositionOffset_,
        values_ ? values_->size() : 0U);
}

FeatureId::FeatureId(FeatureId::Data const& data,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(l, a, key),
      data_(data)
{
    if (data_.idPartValues_ != simfil::InvalidArrayIndex) {
        values_ = model().resolve<simfil::Array>(
            simfil::ModelNodeAddress{
                simfil::ModelPool::ColumnId::Arrays,
                static_cast<uint32_t>(data_.idPartValues_)});
    }

    partNames_ = resolvePartNames(
        model(),
        data_.typeId_,
        data_.idCompositionOffset_,
        values_ ? values_->size() : 0U);
}

std::string_view FeatureId::typeId() const
{
    if (auto s = model().strings()->resolve(data_.typeId_)) {
        return *s;
    }
    return "err-unresolved-typename";
}

std::string FeatureId::toString() const
{
    std::stringstream result;
    result << typeId();

    if (data_.useCommonTilePrefix_) {
        if (auto idPrefix = model().getIdPrefix()) {
            for (auto const& [_, value] : idPrefix->fields()) {
                appendNodeValueToString(result, value);
            }
        }
    }

    if (values_) {
        for (auto const& value : *values_) {
            appendNodeValueToString(result, value);
        }
    }

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
    if (i < 0 || !values_ || i >= static_cast<int64_t>(values_->size())) {
        return {};
    }
    return values_->at(i);
}

uint32_t FeatureId::size() const
{
    return values_ ? values_->size() : 0U;
}

simfil::ModelNode::Ptr FeatureId::get(const simfil::StringId& f) const
{
    if (!values_) {
        return {};
    }

    for (size_t i = 0; i < partNames_.size(); ++i) {
        if (partNames_[i] == f && i < values_->size()) {
            return values_->at(static_cast<int64_t>(i));
        }
    }

    return {};
}

simfil::StringId FeatureId::keyAt(int64_t i) const
{
    if (i < 0 || i >= static_cast<int64_t>(partNames_.size())) {
        return {};
    }
    return partNames_[static_cast<size_t>(i)];
}

bool FeatureId::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    for (auto i = 0U; i < size(); ++i) {
        auto node = at(static_cast<int64_t>(i));
        if (node && !cb(*node)) {
            return false;
        }
    }
    return true;
}

KeyValueViewPairs FeatureId::keyValuePairs() const
{
    KeyValueViewPairs result;

    if (data_.useCommonTilePrefix_) {
        if (auto idPrefix = model().getIdPrefix()) {
            for (auto const& [key, value] : idPrefix->fields()) {
                appendTypedKeyValue(model(), key, value, [&](std::string_view keyName, auto&& v) {
                    result.emplace_back(keyName, v);
                });
            }
        }
    }

    if (values_) {
        auto const limit = std::min<size_t>(partNames_.size(), values_->size());
        for (size_t i = 0; i < limit; ++i) {
            auto valueNode = values_->at(static_cast<int64_t>(i));
            appendTypedKeyValue(model(), partNames_[i], valueNode, [&](std::string_view keyName, auto&& v) {
                result.emplace_back(keyName, v);
            });
        }
    }

    return result;
}

}
