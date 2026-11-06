#include "featureid.h"
#include "stringpool.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <iterator>
#include <limits>
#include <set>
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
FeatureIdentity::Composition resolveComposition(
    PartitionFeatureModelLayerBase const& model,
    simfil::StringId typeId,
    uint8_t idCompositionIndex)
{
    auto typeName = model.strings()->resolve(typeId);
    if (!typeName) {
        return {};
    }

    auto typeInfo = model.layerInfo()->getTypeInfo(*typeName, false);
    if (!typeInfo || typeInfo.compositions().empty()) {
        return {};
    }

    auto const compositionIndex =
        std::min<size_t>(idCompositionIndex, typeInfo.compositions().size() - 1U);
    return typeInfo.compositions()[compositionIndex];
}

/** Build the externally visible id-part layout after removing an optional tile prefix. */
void resolveVisiblePartLayout(
    PartitionFeatureModelLayerBase const& model,
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

    auto composition = resolveComposition(model, data.typeId_, data.idCompositionIndex_);
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
                        else if constexpr (std::is_integral_v<T>) {
                            prefixFeatureIdParts.emplace_back(*keyStr, static_cast<int64_t>(v));
                        }
                    },
                    value->value());
            }
        }

        if (!prefixFeatureIdParts.empty()) {
            auto const matchEndIndex =
                composition.matchEndIndex(0, prefixFeatureIdParts, prefixFeatureIdParts.size());
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
        static_cast<uint32_t>(composition.parts().size()) -
            std::min<uint32_t>(localStartIndex, static_cast<uint32_t>(composition.parts().size())));

    for (uint32_t slot = 0; slot < maxSlots; ++slot) {
        auto const valueNode = values->at(static_cast<int64_t>(slot));
        if (!valueNode ||
            std::holds_alternative<std::monostate>(valueNode->value())) {
            continue;
        }

        auto sid = model.strings()->emplace(composition.parts()[localStartIndex + slot].name());
        if (!sid) {
            break;
        }
        partNames.push_back(*sid);
        visibleValueIndices.push_back(slot);
    }
}

template <typename Fn>
/** Forward a typed id-part value while rejecting node types that cannot appear in ids. */
void appendTypedKeyValue(
    PartitionFeatureModelLayerBase const& model,
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
            if constexpr (
                std::is_same_v<T, simfil::ByteArray> || std::is_same_v<T, simfil::Bitmask>) {
                raiseFmt("FeatureId part '{}' must be an integer or string.", *keyStr);
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
    FeatureIdentity::Composition composition,
    std::vector<std::string_view> const& tokens,
    size_t partIndex,
    size_t tokenIndex,
    CompositionParseState const& current,
    std::vector<CompositionParseState>& results,
    std::string* error)
{
    if (partIndex == composition.parts().size()) {
        if (tokenIndex == tokens.size()) {
            results.push_back(current);
            return true;
        }
        return false;
    }

    auto const& part = composition.parts()[partIndex];
    auto matched = false;

    if (part.optional()) {
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
    if (!part.validateValue(parsedValue, &localError)) {
        // Datatype mismatches do not fail the whole search immediately because a
        // different composition may still accept the same token sequence.
        return matched;
    }

    auto next = current;
    next.values.emplace_back(part.name(), std::move(parsedValue));
    return tryParseCompositionRecursive(
               composition,
               tokens,
               partIndex + 1,
               tokenIndex + 1,
               next,
               results,
               error) || matched;
}

/** Materialize one exported reference field for external-map feature ids. */
[[nodiscard]] simfil::ModelNode::Ptr referenceFieldNode(
    FeatureId const& featureId,
    simfil::StringId field)
{
    auto owner = featureId.model().shared_from_this();
    if (field == StringPool::TypeIdStr)
        return model_ptr<simfil::ValueNode>::make(std::string(featureId.typeId()), owner);
    if (field == StringPool::MapIdStr) {
        if (auto mapId = featureId.externalMapId()) {
            return model_ptr<simfil::ValueNode>::make(std::string(*mapId), owner);
        }
    }
    return {};
}
}  // namespace

FeatureId::FeatureId(
    FeatureId::Data& data,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : FeatureId(static_cast<FeatureId::Data const&>(data), std::move(l), a, key)
{
}

FeatureId::FeatureId(
    FeatureId::Data const& data,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>(l, a, key), data_(data)
{
    if (data_.idParts_.column() == simfil::ModelPool::ColumnId::Arrays) {
        if (data_.extMapId_ != simfil::StringPool::Empty)
            raise("A foreign FeatureId must store named parts, not schema-backed array values.");
        values_ = model().resolve<simfil::Array>(data_.idParts_);
        (void)values_->size();
    }
    else if (data_.idParts_.column() == simfil::ModelPool::ColumnId::Objects) {
        if (data_.extMapId_ == simfil::StringPool::Empty || data_.useCommonTilePrefix_ ||
            data_.idCompositionIndex_ != 0)
            raise("Named FeatureId parts require an explicit foreign map without a local prefix.");
        auto parts = model().resolve<simfil::Object>(data_.idParts_);
        auto const map = externalMapId();
        auto const type = typeId();
        if (!map || map->empty() || *map == model().mapId() || map->size() > 1024 || type.empty() ||
            type.size() > 1024 || parts->size() == 0 || parts->size() > 255)
            raise("Invalid named foreign FeatureId storage.");
        validateIdentifierName("map id", *map, {}, "/");
        validateIdentifierName("feature type name", type, ".");
        std::set<simfil::StringId> names;
        for (auto const& [key, value] : parts->fields()) {
            auto const name = model().strings()->resolve(key);
            if (!name || name->empty() || name->size() > 1024 || !names.insert(key).second ||
                !value)
                raise("Invalid named foreign FeatureId part.");
            validateIdentifierName("id-part label", *name, ".");
            auto scalar = value->value();
            if (!std::holds_alternative<int64_t>(scalar) &&
                !std::holds_alternative<std::string>(scalar) &&
                !std::holds_alternative<std::string_view>(scalar))
                raise("Foreign FeatureId parts must be integer or string scalars.");
        }
    }
    else {
        raise("FeatureId parts must refer to an Array or Object.");
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
    raiseFmt(
        "Feature type string ID {} is missing from dictionary '{}' for map '{}' layer '{}'.",
        data_.typeId_,
        model().stringPoolId(),
        model().mapId(),
        model().layerInfo()->layerId_);
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
    if (externalMapId())
        raise(
            "Foreign FeatureId has no canonical string before target-map resolution; use "
            "toJson().");
    return formatFeatureIdString(
        typeId(),
        castToKeyValue(keyValuePairs()),
        model().layerInfo().get());
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
    case 0: return model().resolve<simfil::Object>(data_.idParts_);
    case 1:
        return referenceFieldNode(*this, StringPool::MapIdStr);
    case 2: return referenceFieldNode(*this, StringPool::TypeIdStr);
    default:
        return {};
    }
}

uint32_t FeatureId::size() const
{
    return externalMapId() ? 3U : 0U;
}

simfil::ModelNode::Ptr FeatureId::get(const simfil::StringId& f) const
{
    if (!externalMapId()) {
        return {};
    }
    if (f == StringPool::IdStr)
        return model().resolve<simfil::Object>(data_.idParts_);
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
    case 2: return StringPool::TypeIdStr;
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
    KeyValueViewPairs result;
    if (data_.idParts_.column() == simfil::ModelPool::ColumnId::Objects) {
        auto parts = model().resolve<simfil::Object>(data_.idParts_);
        result.reserve(parts->size());
        for (auto const& [key, value] : parts->fields())
            appendTypedKeyValue(
                model(),
                key,
                value,
                [&](std::string_view name, auto&& scalar) { result.emplace_back(name, scalar); });
        return result;
    }
    ensureVisiblePartLayout();

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

KeyValuePairs parseFeatureIdPartsJson(nlohmann::json const& parts)
{
    if (!parts.is_object() || parts.empty() || parts.size() > 255)
        raise("Named feature ID parts must be an object with 1..255 entries.");
    KeyValuePairs result;
    result.reserve(parts.size());
    for (auto const& [name, value] : parts.items()) {
        if (name.empty() || name.size() > 1024)
            raise("Feature ID part names must contain 1..1024 bytes.");
        validateIdentifierName("id-part label", name, ".");
        if (value.is_string())
            result.emplace_back(name, value.get<std::string>());
        else if (
            value.is_number_integer() &&
            (!value.is_number_unsigned() ||
             value.get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
            result.emplace_back(name, value.get<int64_t>());
        else
            raise("Named feature ID parts must be int64 integers or strings.");
    }
    return result;
}

bool normalizeFeatureIdParts(
    std::string_view typeId,
    KeyValuePairs const& parts,
    LayerInfo const& layerInfo,
    ParsedFeatureId& result)
{
    result = {};
    auto type = layerInfo.getTypeInfo(typeId, false);
    if (!type)
        return false;
    std::set<std::string_view> names;
    for (auto const& [name, value] : parts)
        if (!names.insert(name).second)
            return false;
    for (size_t index = 0;
         index < type.compositions().size() && index <= std::numeric_limits<uint8_t>::max();
         ++index)
    {
        KeyValuePairs ordered;
        bool matches = true;
        for (auto definition : type.compositions()[index].parts()) {
            auto part = std::find_if(
                parts.begin(),
                parts.end(),
                [&](auto const& item) { return item.first == definition.name(); });
            if (part == parts.end()) {
                if (!definition.optional()) {
                    matches = false;
                    break;
                }
                continue;
            }
            auto value = part->second;
            if (!definition.validateValue(value)) {
                matches = false;
                break;
            }
            ordered.emplace_back(part->first, std::move(value));
        }
        // Unknown parts must not disappear while selecting a composition.
        if (matches && ordered.size() == parts.size()) {
            result = {std::string(typeId), std::move(ordered), static_cast<uint8_t>(index)};
            return true;
        }
    }
    return false;
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
    auto typeInfo = layerInfo.getTypeInfo(typeId, false);
    if (!typeInfo) {
        if (error) {
            *error = fmt::format("Could not find feature type {}", typeId);
        }
        return false;
    }

    std::vector<std::pair<uint8_t, CompositionParseState>> matches;
    std::string localError;
    for (uint32_t compositionIndex = 0; compositionIndex < typeInfo.compositions().size();
         ++compositionIndex)
    {
        auto const& composition = typeInfo.compositions()[compositionIndex];
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
    KeyValuePairs const& featureIdParts,
    LayerInfo const* layerInfo)
{
    FeatureIdentity::Composition composition;
    if (layerInfo) {
        auto index = layerInfo->matchingFeatureIdCompositionIndex(
            typeId,
            castToKeyValueView(featureIdParts),
            false);
        if (index)
            composition = layerInfo->getTypeInfo(typeId).compositions()[*index];
    }
    std::string result(typeId);
    // Older AppleClang cannot capture structured bindings in the nested lambdas below.
    for (auto const& idPart : featureIdParts) {
        std::visit(
            [&](auto&& part)
            {
                using T = std::decay_t<decltype(part)>;
                if constexpr (std::is_same_v<T, std::string_view> ||
                              std::is_same_v<T, std::string>) {
                    fmt::format_to(
                        std::back_inserter(result),
                        FMT_STRING(".{}"),
                        escapeFeatureIdPart(part));
                }
                else {
                    bool unsignedPart = composition &&
                        std::ranges::any_of(composition.parts(),
                                            [&](auto const& item) {
                                                return item.name() == idPart.first &&
                                                    item.datatype() == IdPartDataType::U64;
                                            });
                    if (unsignedPart)
                        fmt::format_to(
                            std::back_inserter(result),
                            ".{}",
                            std::bit_cast<uint64_t>(part));
                    else
                        fmt::format_to(std::back_inserter(result), ".{}", part);
                }
            },
            idPart.second);
    }
    return result;
}

}
