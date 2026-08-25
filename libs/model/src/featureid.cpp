#include "featureid.h"
#include "stringpool.h"

#include <algorithm>
#include <charconv>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "mapget/log.h"

namespace mapget
{

namespace
{
/** Resolve the concrete id composition used by a stored feature-id node. */
std::vector<IdPart> const* resolveComposition(
    TileFeatureModelLayerBase const& model,
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

/** Build the externally visible id-part layout after removing an optional tile prefix. */
void resolveVisiblePartLayout(
    TileFeatureModelLayerBase const& model,
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
                // A stored prefix that no longer matches the schema would make the
                // visible id parts misleading, so we expose no parts instead.
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
/** Forward a typed id-part value while rejecting node types that cannot appear in ids. */
void appendTypedKeyValue(
    TileFeatureModelLayerBase const& model,
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

/** Escape separators and escape markers inside string-valued id parts. */
[[nodiscard]] std::string escapeFeatureIdPart(std::string_view input)
{
    return escapeIdentifierComponent(input, ".");
}

/** Split a canonical feature-id string into type and id-part tokens. */
[[nodiscard]] std::vector<std::string_view> splitFeatureIdTokens(std::string_view input)
{
    std::vector<std::string_view> tokens;
    size_t start = 0;
    for (size_t i = 0; i <= input.size(); ++i) {
        if (i == input.size() || input[i] == '.') {
            tokens.push_back(input.substr(start, i - start));
            start = i + 1;
        }
    }
    return tokens;
}

struct CompositionParseState
{
    KeyValuePairs values;
};

/** Try to parse one id composition, including optional parts that may consume no token. */
[[nodiscard]] bool tryParseCompositionRecursive(
    std::vector<IdPart> const& composition,
    std::vector<std::string_view> const& tokens,
    size_t partIndex,
    size_t tokenIndex,
    CompositionParseState const& current,
    std::vector<CompositionParseState>& results,
    std::string* error)
{
    if (partIndex == composition.size()) {
        if (tokenIndex == tokens.size()) {
            results.push_back(current);
            return true;
        }
        return false;
    }

    auto const& part = composition[partIndex];
    auto matched = false;

    if (part.isOptional_) {
        // Optional parts are explored both as present and as omitted so we can
        // disambiguate compositions solely from the canonical string form.
        matched = tryParseCompositionRecursive(
            composition,
            tokens,
            partIndex + 1,
            tokenIndex,
            current,
            results,
            error) || matched;
    }

    if (tokenIndex >= tokens.size()) {
        return matched;
    }

    std::string decoded;
    if (!unescapeIdentifierComponent(tokens[tokenIndex], decoded, error)) {
        return false;
    }

    std::variant<int64_t, std::string> parsedValue = decoded;
    std::string localError;
    if (!part.validate(parsedValue, &localError)) {
        // Datatype mismatches do not fail the whole search immediately because a
        // different composition may still accept the same token sequence.
        return matched;
    }

    auto next = current;
    next.values.emplace_back(part.idPartLabel_, std::move(parsedValue));
    return tryParseCompositionRecursive(
               composition,
               tokens,
               partIndex + 1,
               tokenIndex + 1,
               next,
               results,
               error) || matched;
}

/** Append one node value to the canonical dot-separated feature-id string. */
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
                else if constexpr (std::is_same_v<T, std::string_view> || std::is_same_v<T, std::string>) {
                    // String-valued parts must escape canonical separators before joining.
                    fmt::format_to(std::back_inserter(out), FMT_STRING(".{}"), escapeFeatureIdPart(v));
                }
                else {
                    fmt::format_to(std::back_inserter(out), FMT_STRING(".{}"), v);
                }
            }
        },
        node->value());
}

/** Materialize one exported reference field for external-map feature ids. */
[[nodiscard]] simfil::ModelNode::Ptr referenceFieldNode(
    FeatureId const& featureId,
    simfil::StringId field)
{
    auto owner = featureId.model().shared_from_this();
    if (field == StringPool::IdStr) {
        return model_ptr<simfil::ValueNode>::make(featureId.toString(), owner);
    }
    if (field == StringPool::MapIdStr) {
        if (auto mapId = featureId.externalMapId()) {
            return model_ptr<simfil::ValueNode>::make(std::string(*mapId), owner);
        }
    }
    return {};
}
}  // namespace

FeatureId::FeatureId(FeatureId::Data& data,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(l, a, key),
      data_(data)
{
    if (data_.idPartValues_ != simfil::InvalidArrayIndex) {
        values_ = model().resolve<simfil::Array>(
            simfil::ModelNodeAddress{
                simfil::ModelPool::ColumnId::Arrays,
                static_cast<uint32_t>(data_.idPartValues_)});
    }
}

FeatureId::FeatureId(FeatureId::Data const& data,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(l, a, key),
      data_(data)
{
    if (data_.idPartValues_ != simfil::InvalidArrayIndex) {
        values_ = model().resolve<simfil::Array>(
            simfil::ModelNodeAddress{
                simfil::ModelPool::ColumnId::Arrays,
                static_cast<uint32_t>(data_.idPartValues_)});
    }
}

void FeatureId::ensureVisiblePartLayout() const
{
    if (visiblePartLayoutResolved_) {
        return;
    }
    resolveVisiblePartLayout(
        model(),
        data_,
        values_,
        partNames_,
        visibleValueIndices_);
    visiblePartLayoutResolved_ = true;
}

std::string_view FeatureId::typeId() const
{
    if (auto s = model().strings()->resolve(data_.typeId_)) {
        return *s;
    }
    return "err-unresolved-typename";
}

std::string FeatureId::mapId() const
{
    if (auto mapId = externalMapId()) {
        return std::string(*mapId);
    }
    return model().mapId();
}

std::optional<std::string_view> FeatureId::externalMapId() const
{
    if (data_.extMapId_ == simfil::StringPool::Empty) {
        return std::nullopt;
    }

    if (auto resolved = model().strings()->resolve(data_.extMapId_)) {
        return *resolved;
    }

    raise("FeatureId external map id is not known to string pool.");
    return std::nullopt;
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
        for (uint32_t index = 0U; index < values_->size(); ++index) {
            appendNodeValueToString(result, values_->at(index));
        }
    }

    return result;
}

simfil::ValueType FeatureId::type() const
{
    // Only external references need structural object access; all other ids
    // stay canonical strings to preserve the historical feature-id surface.
    return externalMapId().has_value() ? simfil::ValueType::Object : simfil::ValueType::String;
}

simfil::ScalarValueType FeatureId::value() const
{
    if (externalMapId()) {
        return {};
    }
    return toString();
}

simfil::ModelNode::Ptr FeatureId::at(int64_t i) const
{
    if (!externalMapId()) {
        return {};
    }
    switch (i) {
    case 0:
        return referenceFieldNode(*this, StringPool::IdStr);
    case 1:
        return referenceFieldNode(*this, StringPool::MapIdStr);
    default:
        return {};
    }
}

uint32_t FeatureId::size() const
{
    return externalMapId() ? 2U : 0U;
}

simfil::ModelNode::Ptr FeatureId::get(const simfil::StringId& f) const
{
    if (!externalMapId()) {
        return {};
    }
    return referenceFieldNode(*this, f);
}

simfil::StringId FeatureId::keyAt(int64_t i) const
{
    if (!externalMapId()) {
        return {};
    }
    switch (i) {
    case 0:
        return StringPool::IdStr;
    case 1:
        return StringPool::MapIdStr;
    default:
        return {};
    }
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
    ensureVisiblePartLayout();
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

bool parseFeatureIdString(
    std::string_view featureId,
    LayerInfo const& layerInfo,
    ParsedFeatureId& result,
    std::string* error)
{
    result = {};

    auto const tokens = splitFeatureIdTokens(featureId);
    if (tokens.empty() || tokens.front().empty()) {
        if (error) {
            *error = "Feature id must start with a non-empty type id.";
        }
        return false;
    }

    auto const typeId = std::string(tokens.front());
    auto const* typeInfo = layerInfo.getTypeInfo(typeId, false);
    if (!typeInfo) {
        if (error) {
            *error = fmt::format("Could not find feature type {}", typeId);
        }
        return false;
    }

    std::vector<std::pair<uint8_t, CompositionParseState>> matches;
    std::string localError;
    for (uint32_t compositionIndex = 0;
         compositionIndex < typeInfo->uniqueIdCompositions_.size();
         ++compositionIndex) {
        auto const& composition = typeInfo->uniqueIdCompositions_[compositionIndex];
        std::vector<CompositionParseState> parsedStates;
        CompositionParseState emptyState{};
        if (!tryParseCompositionRecursive(
                composition,
                std::vector<std::string_view>(tokens.begin() + 1, tokens.end()),
                0,
                0,
                emptyState,
                parsedStates,
                &localError)) {
            continue;
        }
        for (auto& parsed : parsedStates) {
            // The parser keeps all valid matches so ambiguity can be reported explicitly.
            matches.emplace_back(static_cast<uint8_t>(std::min<uint32_t>(compositionIndex, 255U)), std::move(parsed));
        }
    }

    if (matches.empty()) {
        if (error) {
            *error = localError.empty()
                ? fmt::format("Could not parse feature id '{}' for type '{}'.", featureId, typeId)
                : localError;
        }
        return false;
    }

    auto selectedMatch = matches.begin();
    if (matches.size() > 1) {
        auto const primaryMatch = std::find_if(
            matches.begin(), matches.end(), [](auto const& match) {
                return match.first == 0;
            });
        auto const secondPrimaryMatch = primaryMatch == matches.end()
            ? matches.end()
            : std::find_if(
                  std::next(primaryMatch), matches.end(), [](auto const& match) {
                      return match.first == 0;
                  });

        // Stored features always use composition zero. Prefer its one valid
        // interpretation when a secondary locate composition serializes to
        // the same unlabeled token shape.
        if (primaryMatch != matches.end() && secondPrimaryMatch == matches.end()) {
            selectedMatch = primaryMatch;
        }
        else {
            if (error) {
                *error = fmt::format(
                    "Feature id '{}' matches multiple id compositions of type '{}'.",
                    featureId,
                    typeId);
            }
            return false;
        }
    }

    result.typeId_ = typeId;
    result.idCompositionIndex_ = selectedMatch->first;
    result.keyValuePairs_ = std::move(selectedMatch->second.values);
    return true;
}

std::string formatFeatureIdString(
    std::string_view typeId,
    KeyValuePairs const& featureIdParts)
{
    std::string result(typeId);
    for (auto const& [_, value] : featureIdParts) {
        std::visit(
            [&](auto&& part) {
                using T = std::decay_t<decltype(part)>;
                if constexpr (std::is_same_v<T, std::string_view> ||
                              std::is_same_v<T, std::string>) {
                    fmt::format_to(
                        std::back_inserter(result),
                        FMT_STRING(".{}"),
                        escapeFeatureIdPart(part));
                }
                else {
                    fmt::format_to(
                        std::back_inserter(result),
                        FMT_STRING(".{}"),
                        part);
                }
            },
            value);
    }
    return result;
}

}
