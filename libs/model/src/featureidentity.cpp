#include "mapget/model/featureidentity.h"
#include <fmt/format.h>
#include <bit>
#include <charconv>
#include <limits>
#include "mapget/model/info.h"
#include "mapschema-impl.h"

namespace mapget
{
namespace
{
/** Share exact value validation between input DTOs and canonical borrowed parts. */
template <class Integer>
std::optional<Integer> parseInteger(std::string_view text)
{
    Integer value;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        return {};
    return value;
}

bool validatePart(
    IdPartDataType datatype,
    std::string_view label,
    std::variant<int64_t, std::string_view>& val,
    std::string* error)
{
    // U64 nodes deliberately project through simfil's signed scalar storage.
    // Decimal transport still parses the full unsigned domain without overflow.
    if (datatype == IdPartDataType::U64) {
        if (auto text = std::get_if<std::string_view>(&val)) {
            auto parsed = parseInteger<uint64_t>(*text);
            if (!parsed) {
                if (error)
                    *error = "Expected an unsigned 64-bit decimal ID.";
                return false;
            }
            val = std::bit_cast<int64_t>(*parsed);
        }
        return true;
    }
    std::optional<int64_t> intVal;
    if (std::holds_alternative<std::string_view>(val)) {
        intVal = parseInteger<int64_t>(std::get<std::string_view>(val));
    }
    else {
        intVal = std::get<int64_t>(val);
    }

    auto expectInteger =
        [label, &error, &intVal, &val](auto const minVal, auto const maxVal) -> bool
    {
        if (!intVal) {
            if (error)
                *error = fmt::format(
                    "Value '{}' for {} is not an integer!",
                    std::get<std::string_view>(val),
                    label);
            return false;
        }
        if (intVal < minVal) {
            if (error)
                *error = fmt::format(
                    "Value {} for {} is smaller than allowed ({}).",
                    *intVal,
                    label,
                    minVal);
            return false;
        }
        if (intVal > maxVal) {
            if (error)
                *error = fmt::format(
                    "Value {} for {} is larger than allowed ({}).",
                    *intVal,
                    label,
                    maxVal);
            return false;
        }
        val = *intVal;
        return true;
    };

    auto expectString = [label, &error, &val](auto const& validator) -> bool
    {
        if (!std::holds_alternative<std::string_view>(val)) {
            if (error)
                *error = fmt::format("Value for {} must be a string!", label);
            return false;
        }
        return validator(std::get<std::string_view>(val), error);
    };

    auto expectUuid128 = [&expectString]() -> bool
    {
        return expectString(
            [](auto const& strVal, auto error)
            {
                // UUID128 ids are stored as the mapget-specific 16-character token,
                // not a hyphenated RFC 4122 string.
                if (strVal.size() != 16) {
                    if (error)
                        *error = fmt::format("Value for {} must have 16 characters!", strVal);
                    return false;
                }
                return true;
            });
    };

    switch (datatype) {
    case IdPartDataType::I32: return expectInteger(INT32_MIN, INT32_MAX);
    case IdPartDataType::U32: return expectInteger(0, UINT32_MAX);
    case IdPartDataType::U64: return expectInteger(0, UINT64_MAX);
    case IdPartDataType::I64: return expectInteger(INT64_MIN, INT64_MAX);
    case IdPartDataType::UUID128: return expectUuid128();
    case IdPartDataType::STR: return expectString([](auto s, auto e) { return true; });
    }

    if (error)
        *error = fmt::format(
            "Part datatype {} is not supported.",
            static_cast<std::underlying_type_t<IdPartDataType>>(datatype));
    return false;
}

/** Normalize an owning value using the same borrowed validation path. */
bool validateOwnedPart(
    IdPartDataType datatype,
    std::string_view label,
    std::variant<int64_t, std::string>& value,
    std::string* error)
{
    auto borrowed = std::holds_alternative<std::string>(value) ?
        std::variant<int64_t, std::string_view>{std::get<std::string>(value)} :
        std::variant<int64_t, std::string_view>{std::get<int64_t>(value)};
    bool result = validatePart(datatype, label, borrowed, error);
    if (auto integer = std::get_if<int64_t>(&borrowed))
        value = *integer;
    return result;
}
}  // namespace

bool IdPart::validate(std::variant<int64_t, std::string_view>& value, std::string* error) const
{
    return validatePart(datatype_, idPartLabel_, value, error);
}

bool IdPart::validate(std::variant<int64_t, std::string>& value, std::string* error) const
{
    return validateOwnedPart(datatype_, idPartLabel_, value, error);
}

std::string_view FeatureIdentity::name() const
{
    return owner_ ?
        owner_->schema_->name(owner_->schema_->identities[index_].name) :
        std::string_view{};
}

size_t FeatureIdentity::compositionCount() const
{
    return owner_ ?
        owner_->schema_->identityCompositions
            .size(owner_->schema_->identities[index_].compositions) :
        0;
}

FeatureIdentity::Composition FeatureIdentity::compositionAt(size_t index) const
{
    auto handle = owner_->schema_->identities[index_].compositions;
    return {owner_, owner_->schema_->identityCompositions.range(handle)[index]};
}

size_t FeatureIdentity::Composition::partCount() const
{
    return owner_ ? owner_->schema_->identityParts.size(parts_) : 0;
}

FeatureIdentity::Part FeatureIdentity::Composition::partAt(size_t index) const
{
    return {owner_, parts_, index};
}

std::string_view FeatureIdentity::Part::name() const
{
    return owner_ ?
        owner_->schema_->name(owner_->schema_->identityParts.range(parts_)[index_].name) :
        std::string_view{};
}

IdPartDataType FeatureIdentity::Part::datatype() const
{
    if (!owner_)
        throw std::logic_error("An absent identity part has no datatype.");
    return IdPartDataType(owner_->schema_->identityParts.range(parts_)[index_].datatype);
}

bool FeatureIdentity::Part::optional() const
{
    return owner_ && (owner_->schema_->identityParts.range(parts_)[index_].flags & 2);
}

bool FeatureIdentity::Part::synthetic() const
{
    return owner_ && (owner_->schema_->identityParts.range(parts_)[index_].flags & 1);
}

std::string_view FeatureIdentity::Part::description() const
{
    return owner_ ?
        owner_->schema_->text(owner_->schema_->identityParts.range(parts_)[index_].description) :
        std::string_view{};
}

nlohmann::json FeatureIdentity::Part::toJson() const
{
    return {
        {"partId", name()},
        {"description", description()},
        {"datatype", datatype()},
        {"isSynthetic", synthetic()},
        {"isOptional", optional()}};
}

bool FeatureIdentity::Part::validateValue(
    std::variant<int64_t, std::string_view>& value,
    std::string* error) const
{
    return owner_ && validatePart(datatype(), name(), value, error);
}

bool FeatureIdentity::Part::validateValue(
    std::variant<int64_t, std::string>& value,
    std::string* error) const
{
    return owner_ && validateOwnedPart(datatype(), name(), value, error);
}

FeatureIdentity FeatureIdentity::retain() const
{
    auto result = *this;
    if (owner_ && !retained_)
        result.retained_ = std::static_pointer_cast<MapSchema const>(owner_->shared_from_this());
    return result;
}

FeatureIdentity::Composition FeatureIdentity::Composition::retain() const
{
    auto result = *this;
    if (owner_ && !retained_)
        result.retained_ = std::static_pointer_cast<MapSchema const>(owner_->shared_from_this());
    return result;
}

FeatureIdentity::Part FeatureIdentity::Part::retain() const
{
    auto result = *this;
    if (owner_ && !retained_)
        result.retained_ = std::static_pointer_cast<MapSchema const>(owner_->shared_from_this());
    return result;
}

bool FeatureIdentity::Composition::operator==(Composition const& other) const noexcept
{
    return owner_ == other.owner_ && parts_ == other.parts_;
}

bool FeatureIdentity::Part::operator==(Part const& other) const noexcept
{
    return owner_ == other.owner_ && parts_ == other.parts_ && index_ == other.index_;
}

std::optional<uint32_t> FeatureIdentity::Composition::matchEndIndex(
    uint32_t start,
    KeyValueViewPairs const& values,
    size_t length,
    std::string* error) const
{
    if (!owner_ || start > partCount())
        return {};
    auto definitions = parts();
    auto value = values.begin();
    while (length && start < definitions.size()) {
        if (value == values.end())
            return {};
        auto part = definitions[start];
        if (part.name() != value->first) {
            if (part.optional()) {
                ++start;
                continue;
            }
            return {};
        }
        auto normalized = value->second;
        if (!part.validateValue(normalized, error))
            return {};
        ++value;
        ++start;
        --length;
    }
    return length ? std::nullopt : std::optional<uint32_t>{start};
}

bool FeatureIdentity::Composition::matches(
    uint32_t start,
    KeyValueViewPairs const& values,
    size_t length,
    bool requireEnd,
    std::string* error) const
{
    auto end = matchEndIndex(start, values, length, error);
    if (!end)
        return false;
    if (requireEnd) {
        auto definitions = parts();
        for (size_t i = *end; i < definitions.size(); ++i)
            if (!definitions[i].optional())
                return false;
    }
    return true;
}

std::optional<uint8_t> FeatureIdentity::matchingCompositionIndex(
    KeyValueViewPairs const& values,
    MatchMode mode,
    std::string* error) const
{
    if (!owner_)
        return {};
    auto definitions = compositions();
    auto count = mode == MatchMode::PrimaryOnly ?
        std::min(size_t{1}, definitions.size()) :
        definitions.size();
    for (size_t i = 0; i < count; ++i)
        if (definitions[i].matches(0, values, values.size(), true, error))
            return uint8_t(i);
    return {};
}

bool FeatureIdentity::sameIdentity(FeatureIdentity const& other) const
{
    if (!*this || !other)
        return *this == other;
    auto left = compositions(), right = other.compositions();
    if (name() != other.name() || left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i) {
        auto a = left[i].parts(), b = right[i].parts();
        if (a.size() != b.size())
            return false;
        for (size_t j = 0; j < a.size(); ++j)
            if (a[j].name() != b[j].name() || a[j].datatype() != b[j].datatype() ||
                a[j].optional() != b[j].optional() || a[j].synthetic() != b[j].synthetic())
                return false;
    }
    return true;
}

bool FeatureIdentity::sameIdentity(FeatureTypeInfo const& declaration) const
{
    if (!*this || name() != declaration.name_)
        return false;
    auto definitions = compositions();
    if (definitions.size() != declaration.uniqueIdCompositions_.size())
        return false;
    for (size_t i = 0; i < definitions.size(); ++i) {
        auto parts = definitions[i].parts();
        auto const& expected = declaration.uniqueIdCompositions_[i];
        if (parts.size() != expected.size())
            return false;
        for (size_t j = 0; j < parts.size(); ++j)
            if (parts[j].name() != expected[j].idPartLabel_ ||
                parts[j].datatype() != expected[j].datatype_ ||
                parts[j].optional() != expected[j].isOptional_ ||
                parts[j].synthetic() != expected[j].isSynthetic_)
                return false;
    }
    return true;
}

FeatureTypeInfo FeatureIdentity::toDeclaration() const
{
    if (!*this)
        throw std::logic_error("Cannot materialize an absent feature identity.");
    FeatureTypeInfo result;
    result.name_ = name();
    for (auto composition : compositions()) {
        auto& parts = result.uniqueIdCompositions_.emplace_back();
        for (auto part : composition.parts())
            parts.push_back(
                {std::string(part.name()),
                 std::string(part.description()),
                 part.datatype(),
                 part.synthetic(),
                 part.optional()});
    }
    return result;
}

nlohmann::json FeatureIdentity::toJson() const
{
    auto definitions = nlohmann::json::array();
    for (auto composition : compositions()) {
        auto parts = nlohmann::json::array();
        for (auto part : composition.parts())
            parts.push_back(part.toJson());
        definitions.push_back(std::move(parts));
    }
    return {{"name", name()}, {"uniqueIdCompositions", std::move(definitions)}};
}
}  // namespace mapget
