#include "featureid.h"
#include "featurelayer.h"

#include <algorithm>
#include <string>

#include <fmt/format.h>

#include "mapget/log.h"

namespace mapget
{

namespace
{
std::vector<IdPart> const* resolveComposition(
    TileFeatureLayer const& model,
    simfil::StringId typeId,
    uint8_t idCompositionIndex)
{
    auto typeName = model.strings()->resolve(typeId);
    if (!typeName) {
        return nullptr;
    }

    auto typeInfo = model.layerInfo()->getTypeInfo(*typeName, false);
    if (!typeInfo || typeInfo->uniqueIdCompositions_.empty()) {
        return nullptr;
    }

    auto const compositionIndex = std::min<size_t>(
        idCompositionIndex,
        typeInfo->uniqueIdCompositions_.size() - 1U);
    return &typeInfo->uniqueIdCompositions_[compositionIndex];
}

void resolveVisiblePartLayout(
    TileFeatureLayer const& model,
    FeatureId::Data const& data,
    model_ptr<Array> const& values,
    std::vector<simfil::StringId>& partNames,
    std::vector<uint32_t>& visibleValueIndices)
{
    partNames.clear();
    visibleValueIndices.clear();

    if (!values) {
        return;
    }

    auto const* composition = resolveComposition(model, data.typeId_, data.idCompositionIndex_);
    if (!composition) {
        return;
    }

    uint32_t localStartIndex = 0U;
    if (data.useCommonTilePrefix_) {
        KeyValueViewPairs prefixFeatureIdParts;
        if (auto const idPrefix = model.getIdPrefix()) {
            prefixFeatureIdParts.reserve(idPrefix->size());
            for (auto const& [key, value] : idPrefix->fields()) {
                auto const keyStr = model.strings()->resolve(key);
                if (!keyStr || !value) {
                    continue;
                }

                std::visit(
                    [&](auto&& v)
                    {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, std::monostate> ||
                                      std::is_same_v<T, double> ||
                                      std::is_same_v<T, simfil::ByteArray>) {
                        }
                        else if constexpr (std::is_same_v<T, std::string_view> ||
                                           std::is_same_v<T, std::string>) {
                            prefixFeatureIdParts.emplace_back(*keyStr, std::string_view(v));
                        }
                        else {
                            prefixFeatureIdParts.emplace_back(*keyStr, static_cast<int64_t>(v));
                        }
                    },
                    value->value());
            }
        }

        if (!prefixFeatureIdParts.empty()) {
            auto const matchEndIndex = IdPart::compositionMatchEndIndex(
                *composition,
                0,
                prefixFeatureIdParts,
                prefixFeatureIdParts.size());
            if (!matchEndIndex) {
                return;
            }
            localStartIndex = *matchEndIndex;
        }
    }

    auto const maxSlots = std::min<uint32_t>(
        values->size(),
        static_cast<uint32_t>(composition->size()) - std::min<uint32_t>(
            localStartIndex,
            static_cast<uint32_t>(composition->size())));

    for (uint32_t slot = 0; slot < maxSlots; ++slot) {
        auto const valueNode = values->at(static_cast<int64_t>(slot));
        if (!valueNode ||
            std::holds_alternative<std::monostate>(valueNode->value())) {
            continue;
        }

        auto sid = model.strings()->emplace((*composition)[localStartIndex + slot].idPartLabel_);
        if (!sid) {
            break;
        }
        partNames.push_back(*sid);
        visibleValueIndices.push_back(slot);
    }
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

void appendNodeValueToString(std::string& out, simfil::ModelNode::Ptr const& node)
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
                if constexpr (std::is_same_v<T, bool>) {
                    fmt::format_to(std::back_inserter(out), FMT_STRING(".{:d}"), v);
                }
                else {
                    fmt::format_to(std::back_inserter(out), FMT_STRING(".{}"), v);
                }
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

    resolveVisiblePartLayout(model(), data_, values_, partNames_, visibleValueIndices_);
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

    resolveVisiblePartLayout(model(), data_, values_, partNames_, visibleValueIndices_);
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
    std::string result(typeId());

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

    return result;
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
    if (i < 0 || !values_ || i >= static_cast<int64_t>(visibleValueIndices_.size())) {
        return {};
    }
    return values_->at(static_cast<int64_t>(visibleValueIndices_[static_cast<size_t>(i)]));
}

uint32_t FeatureId::size() const
{
    return static_cast<uint32_t>(visibleValueIndices_.size());
}

simfil::ModelNode::Ptr FeatureId::get(const simfil::StringId& f) const
{
    if (!values_) {
        return {};
    }

    for (size_t i = 0; i < partNames_.size(); ++i) {
        if (partNames_[i] == f && i < visibleValueIndices_.size()) {
            return values_->at(static_cast<int64_t>(visibleValueIndices_[i]));
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
        auto const limit = std::min<size_t>(partNames_.size(), visibleValueIndices_.size());
        for (size_t i = 0; i < limit; ++i) {
            auto valueNode = values_->at(static_cast<int64_t>(visibleValueIndices_[i]));
            appendTypedKeyValue(model(), partNames_[i], valueNode, [&](std::string_view keyName, auto&& v) {
                result.emplace_back(keyName, v);
            });
        }
    }

    return result;
}

}
