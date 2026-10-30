#pragma once
// Private transient import/compiler state. Discarded when MapSchema is finalized.
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include "mapget/model/layerschema.h"

namespace mapget::schema_detail
{
using Kind = simfil::Schema::Kind;
using Composition = simfil::Schema::Composition;

/** Interpret structural affinity without depending on a specialized kind's name. */
bool hasAffinity(Kind kind, simfil::ValueType type)
{
    return (simfil::Schema::affinities(kind) & simfil::valueTypeAffinity(type)) != 0;
}

/** Match legacy structural selectors without discarding precise scalar kinds. */
bool matchesKind(Kind kind, Kind requested)
{
    if (requested == Kind::Object) {
        return hasAffinity(kind, simfil::ValueType::Object);
    }
    if (requested == Kind::Array) {
        return hasAffinity(kind, simfil::ValueType::Array);
    }
    if (requested == Kind::Value) {
        return !hasAffinity(kind, simfil::ValueType::Object) &&
            !hasAffinity(kind, simfil::ValueType::Array) && simfil::Schema::affinities(kind) != 0;
    }
    return kind == requested;
}

constexpr size_t kMaxNormalizedAttributeScopes = 8;

/** Escape one JSON Pointer path token. */
std::string pointerToken(std::string_view token)
{
    std::string result;
    result.reserve(token.size());
    for (char c : token) {
        if (c == '~') {
            result += "~0";
        }
        else if (c == '/') {
            result += "~1";
        }
        else {
            result += c;
        }
    }
    return result;
}

/** Return true if a JSON Schema `type` accepts the requested scalar. */
bool hasType(nlohmann::json const& schema, std::string_view type)
{
    auto typeIt = schema.find("type");
    if (typeIt == schema.end()) {
        return false;
    }
    if (typeIt->is_string()) {
        return typeIt->get_ref<const std::string&>() == type;
    }
    if (typeIt->is_array()) {
        return std::any_of(
            typeIt->begin(),
            typeIt->end(),
            [&](auto const& item)
            { return item.is_string() && item.template get_ref<const std::string&>() == type; });
    }
    return false;
}

/** Return the x-mapget metadata object if present. */
nlohmann::json const* mapgetMetadata(nlohmann::json const& schema)
{
    auto it = schema.find("x-mapget");
    if (it == schema.end() || !it->is_object()) {
        return nullptr;
    }
    return &*it;
}

/** Read a string field from a metadata object. */
std::string metadataString(nlohmann::json const* metadata, std::string_view key)
{
    if (!metadata) {
        return {};
    }
    auto it = metadata->find(std::string(key));
    if (it == metadata->end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

/** Local traversal context used to derive stable x-mapget keys. */
struct BuildContext
{
    std::string featureType_;
    std::string attributeLayerName_;
};

/** Return the first JSON Schema combiner present on the object. */
std::string_view combinerKey(nlohmann::json const& schema)
{
    for (std::string_view key : {"oneOf", "anyOf", "allOf"}) {
        auto it = schema.find(std::string(key));
        if (it != schema.end() && it->is_array()) {
            return key;
        }
    }
    return {};
}

/** Return whether this schema branch should be treated as an object schema. */
bool isObjectSchema(nlohmann::json const& schema)
{
    // Type-specific JSON Schema keywords do not change an explicit non-object type.
    return schema.contains("type") ? hasType(schema, "object") : schema.contains("properties");
}

/** Return whether this schema branch should be treated as an array schema. */
bool isArraySchema(nlohmann::json const& schema)
{
    return schema.contains("type") ? hasType(schema, "array") : schema.contains("items");
}

/** Return whether this schema branch should be treated as a scalar value schema. */
bool isValueSchema(nlohmann::json const& schema)
{
    return schema.contains("const") || schema.contains("enum") || hasType(schema, "null") ||
        hasType(schema, "boolean") || hasType(schema, "integer") || hasType(schema, "number") ||
        hasType(schema, "string");
}

/** Return whether a root-level Attribute field is metadata, not the shorthand value payload. */
bool isAttributeScalarShorthandMetadataField(std::string_view fieldName)
{
    return fieldName.starts_with("$") || fieldName == "_sourceData" || fieldName == "conditions" ||
        fieldName == "properties" || fieldName == "references" || fieldName == "validity";
}

/** Stable suffix for memoizing the same JSON branch under different schema kinds. */
std::string_view kindMemoSuffix(std::optional<Kind> kind)
{
    if (!kind) {
        return "n";
    }

    switch (*kind) {
    case Kind::Object: return "o";
    case Kind::Array: return "a";
    case Kind::Value: return "v";
    default: return "n";
    }

    return "n";
}

/** Build a memo key that keeps x-mapget context-sensitive aliases distinct. */
std::string
contextMemoKey(std::string_view pointer, std::optional<Kind> kind, BuildContext const& context)
{
    auto appendSized = [](std::string& result, std::string_view value)
    {
        result += std::to_string(value.size());
        result += ':';
        result += value;
    };

    std::string result(pointer);
    result += '|';
    result += kindMemoSuffix(kind);
    result += '|';
    appendSized(result, context.featureType_);
    result += '|';
    appendSized(result, context.attributeLayerName_);
    return result;
}

/** Identify the JSON projection wrapper whose first branch is the native multimap value. */
bool isMapgetMultimap(nlohmann::json const& schema)
{
    auto it = schema.find("x-mapget-multimap");
    return it != schema.end() && it->is_boolean() && it->get<bool>();
}

/** Build a registry key from known x-mapget annotations. */
std::string
annotatedKey(nlohmann::json const& schema, std::string_view pointer, BuildContext const& context)
{
    auto const* metadata = mapgetMetadata(schema);
    auto explicitKey = metadataString(metadata, "schemaKey");
    if (!explicitKey.empty()) {
        return explicitKey;
    }

    auto metaType = metadataString(metadata, "metaType");
    auto featureType = metadataString(metadata, "featureType");
    if (featureType.empty()) {
        featureType = context.featureType_;
    }

    if (metaType == "Feature" && !featureType.empty()) {
        return "Feature:" + featureType;
    }
    if (metaType == "FeatureProperties" && !featureType.empty()) {
        return "FeatureProperties:" + featureType;
    }
    if (metaType == "AttributeLayerMap" && !featureType.empty()) {
        return "AttributeLayerMap:" + featureType;
    }
    if (metaType == "AttributeContainer" && !featureType.empty() &&
        !context.attributeLayerName_.empty())
    {
        return "AttributeContainer:" + featureType + ":" + context.attributeLayerName_;
    }
    if (metaType == "Attribute" && !featureType.empty() && !context.attributeLayerName_.empty()) {
        auto attributeTypeCode = metadataString(metadata, "attributeTypeCode");
        if (!attributeTypeCode.empty()) {
            return "Attribute:" + featureType + ":" + context.attributeLayerName_ + ":" +
                attributeTypeCode;
        }
    }

    return std::string(pointer);
}

/** Resolve local JSON Schema references of the form `#/...`. */
nlohmann::json const& resolveLocalRef(nlohmann::json const& root, std::string const& ref)
{
    if (ref.empty() || ref.front() != '#') {
        throw std::runtime_error("LayerSchema only supports local JSON Schema references.");
    }
    auto pointer = ref.substr(1);
    return root.at(nlohmann::json::json_pointer(pointer));
}

/** Convert a local JSON Schema reference into a canonical registry pointer. */
std::string refToPointer(std::string const& ref)
{
    if (ref.empty() || ref.front() != '#') {
        throw std::runtime_error("LayerSchema only supports local JSON Schema references.");
    }
    return ref.size() == 1 ? "#" : ref.substr(1);
}

/** Return the deterministic key used by feature schema annotations. */
std::string featureKey(std::string_view featureType)
{
    return "Feature:" + std::string(featureType);
}

/** Return the deterministic key used by Feature.properties annotations. */
std::string featurePropertiesKey(std::string_view featureType)
{
    return "FeatureProperties:" + std::string(featureType);
}

/** Return the deterministic key used by Feature.properties.layer annotations. */
std::string attributeLayerMapKey(std::string_view featureType)
{
    return "AttributeLayerMap:" + std::string(featureType);
}

struct SchemaBuilder
{
    /** Concrete mask affinity, allowing null but not a mixed raw-value union. */
    static bool isBitmaskKind(Kind kind)
    {
        auto bits = simfil::Schema::affinities(kind);
        auto mask = simfil::valueTypeAffinity(simfil::ValueType::Bitmask);
        return (bits & mask) &&
            !(bits & ~(mask | simfil::valueTypeAffinity(simfil::ValueType::Null)));
    }

    // The first column is the pre-v6 JSON name ID, not today's StringPool ID.
    // New projections carry stable names: legacy array (15 << 16 | 512) is
    // numerically identical to today's object, so layout autodetection is unsafe.
    static constexpr std::tuple<uint16_t, Kind, std::string_view> JsonKinds[]{
        {4, Kind::Unknown, "unknown"},
        {5, Kind::Any, "any"},
        {6, Kind::Never, "never"},
        {7, Kind::Value, "value"},
        {8, Kind::Null, "null"},
        {9, Kind::Bool, "boolean"},
        {10, Kind::Int, "integer"},
        {11, Kind::Float, "float"},
        {12, Kind::String, "string"},
        {13, Kind::Bytes, "bytes"},
        {14, Kind::Object, "object"},
        {15, Kind::Array, "array"},
        {16, Kind::Union, "union"},
        {17, Kind::OneOf, "oneOf"},
        {18, Kind::Intersection, "intersection"},
        {124, LayerSchema::FeatureKind, "Feature"},
        {125, LayerSchema::AttributeKind, "Attribute"},
        {131, LayerSchema::BitmaskKind, "bitmask"}};

    static constexpr std::pair<simfil::ValueType, std::string_view> JsonAffinities[]{
        {simfil::ValueType::Null, "null"},
        {simfil::ValueType::Bool, "boolean"},
        {simfil::ValueType::Int, "integer"},
        {simfil::ValueType::Float, "float"},
        {simfil::ValueType::String, "string"},
        {simfil::ValueType::Bytes, "bytes"},
        {simfil::ValueType::Bitmask, "bitmask"},
        {simfil::ValueType::TransientObject, "transientObject"},
        {simfil::ValueType::Object, "object"},
        {simfil::ValueType::Array, "array"}};

    /** A bare numeric kind is explicitly the historical v1 projection. Strings
     * such as LiveSource's "attribute"/"region" are annotations, not packed kinds. */
    static std::optional<Kind> jsonKind(nlohmann::json const* metadata)
    {
        if (!metadata)
            return {};
        auto found = metadata->find("kind");
        if (found == metadata->end() || found->is_string())
            return {};
        auto const& value = *found;
        if (value.is_number_integer()) {
            if ((!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
                value.get<uint64_t>() > UINT32_MAX)
                throw std::invalid_argument("Invalid legacy-v1 packed schema kind.");
            auto packed = value.get<uint32_t>();
            auto legacyBits = packed & 0xffff;
            // Legacy ValueType: null..bytes at 1..6, transient/object/array at 7..9.
            if (legacyBits & ~uint32_t(0x3fe))
                throw std::invalid_argument("Unknown legacy-v1 schema affinities.");
            static constexpr simfil::ValueType legacyTypes[]{
                simfil::ValueType::Null,
                simfil::ValueType::Bool,
                simfil::ValueType::Int,
                simfil::ValueType::Float,
                simfil::ValueType::String,
                simfil::ValueType::Bytes,
                simfil::ValueType::TransientObject,
                simfil::ValueType::Object,
                simfil::ValueType::Array};
            uint16_t bits = 0;
            for (size_t i = 0; i < std::size(legacyTypes); ++i)
                if (legacyBits & (1u << (i + 1)))
                    bits |= simfil::valueTypeAffinity(legacyTypes[i]);
            for (auto [legacyName, kind, _] : JsonKinds)
                if (legacyName == packed >> 16)
                    return simfil::Schema::makeKind(simfil::Schema::kindNameId(kind), bits);
            throw std::invalid_argument("Unknown legacy-v1 schema kind extension.");
        }
        if (!value.is_object() || value.size() != 2 || !value.contains("name") ||
            !value["name"].is_string() || !value.contains("affinities") ||
            !value["affinities"].is_array())
            throw std::invalid_argument("Schema kind requires a name and named affinities.");
        uint16_t bits = 0;
        for (auto const& affinity : value["affinities"]) {
            if (!affinity.is_string())
                throw std::invalid_argument("Schema affinities must be names.");
            auto type = std::ranges::find(
                JsonAffinities,
                affinity.get_ref<std::string const&>(),
                &std::pair<simfil::ValueType, std::string_view>::second);
            if (type == std::end(JsonAffinities))
                throw std::invalid_argument(
                    "Unknown schema affinity: " + affinity.get<std::string>());
            auto bit = simfil::valueTypeAffinity(type->first);
            if (bits & bit)
                throw std::invalid_argument("Duplicate schema affinity.");
            bits |= bit;
        }
        for (auto [_, kind, name] : JsonKinds)
            if (name == value["name"].get_ref<std::string const&>())
                return simfil::Schema::makeKind(simfil::Schema::kindNameId(kind), bits);
        throw std::invalid_argument("Unknown schema kind: " + value["name"].get<std::string>());
    }

    /** Textual projection stays stable across simfil/mapget enum and dictionary changes. */
    static nlohmann::json exportKind(Kind kind)
    {
        auto affinities = nlohmann::json::array();
        auto bits = simfil::Schema::affinities(kind);
        for (auto [type, name] : JsonAffinities)
            if (bits & simfil::valueTypeAffinity(type)) {
                affinities.push_back(name);
                bits &= ~simfil::valueTypeAffinity(type);
            }
        if (bits)
            throw std::invalid_argument("Unsupported schema affinities for JSON projection.");
        for (auto [_, known, name] : JsonKinds)
            if (simfil::Schema::kindNameId(kind) == simfil::Schema::kindNameId(known))
                return {{"name", name}, {"affinities", std::move(affinities)}};
        throw std::invalid_argument("Unsupported schema kind for JSON projection.");
    }

    /** Check before recursive JSON copying/dumping as well as before compilation. */
    static void validateInput(nlohmann::json const& root)
    {
        std::vector<std::pair<nlohmann::json const*, size_t>> pending{{&root, 0}};
        size_t nodes = 0, bytes = 0;
        while (!pending.empty()) {
            auto [value, depth] = pending.back();
            pending.pop_back();
            if (++nodes > 4 * 1024 * 1024 || depth > 256)
                throw std::length_error("JSON schema input exceeds its traversal budget.");
            if (value->is_string())
                bytes += value->get_ref<std::string const&>().size();
            if (value->is_structured()) {
                if (value->size() > 4 * 1024 * 1024 - pending.size())
                    throw std::length_error("JSON schema input exceeds its node budget.");
                for (auto const& item : value->items()) {
                    if (value->is_object())
                        bytes += item.key().size();
                    pending.emplace_back(&item.value(), depth + 1);
                }
            }
            if (bytes > 64 * 1024 * 1024)
                throw std::length_error("JSON schema input exceeds its text budget.");
        }
    }
    struct Entry
    {
        simfil::SchemaId id_ = 0;
        std::string key_, jsonPointer_, metaType_;
    };
    using AttributePathOwner = LayerSchema::AttributePathOwner;

    /** Logical object/array/value schema independent of any StringPool numbering. */
    struct LogicalSchema
    {
        simfil::SchemaId id_ = simfil::NoSchemaId;
        Kind kind_ = Kind::Object;
        Entry entry_;
        std::vector<std::string> directFields_;
        std::vector<std::string> directEnumSymbols_;
        std::map<std::string, std::vector<simfil::SchemaId>, std::less<>> childSchemas_;
        std::vector<simfil::SchemaId> elementSchemas_;
        std::vector<AttributePathOwner> attributeOwners_;
        std::string attributeTypeCode_;
        std::string attributeType_;
        std::string zserioType_;
        std::string typeName_;
        Composition composition_ = Composition::None;
        std::vector<simfil::SchemaId> alternatives_;
        std::vector<simfil::ScalarValueType> enumValues_;
        nlohmann::json annotations_;
        std::set<std::string, std::less<>> multimapFields_;
        std::map<std::string, bool, std::less<>> required_;
        std::optional<bool> nullable_;
        bool open_ = false;
        bool complete_ = false;
        bool queryRoot_ = false;
    };

    SchemaBuilder()
    {
        schemas_.push_back({});
        entriesById_.push_back({});
    }

    std::vector<LogicalSchema> schemas_;
    std::vector<Entry> entriesById_;
    std::map<std::string, simfil::SchemaId, std::less<>> idsByKey_;
    std::map<std::pair<simfil::SchemaId, simfil::SchemaId>, simfil::SchemaId> attributeQueryRoots_;

    /** Keep validation-only constraints and descriptive metadata, never a second schema graph. */
    static nlohmann::json annotations(nlohmann::json const& schema)
    {
        auto result = nlohmann::json::object();
        for (
            auto key :
            {"title",
             "description",
             "default",
             "examples",
             "deprecated",
             "readOnly",
             "writeOnly",
             "minimum",
             "maximum",
             "exclusiveMinimum",
             "exclusiveMaximum",
             "multipleOf",
             "minLength",
             "maxLength",
             "pattern",
             "format",
             "minItems",
             "maxItems",
             "uniqueItems",
             "minProperties",
             "maxProperties",
             "required",
             "if",
             "then",
             "else",
             "not",
             "contains",
             "additionalProperties",
             "additionalItems",
             "patternProperties",
             "dependencies",
             "propertyNames"})
        {
            if (auto it = schema.find(key); it != schema.end()) {
                result[key] = *it;
            }
        }
        if (schema.contains("const"))
            result["const"] = schema["const"];
        if (schema.contains("enum") &&
            (schema.contains("const") ||
             std::ranges::any_of(
                 schema["enum"],
                 [](auto const& value) { return value.is_structured(); })))
            result["enum"] = schema["enum"];
        if (auto metadata = mapgetMetadata(schema)) {
            auto extra = *metadata;
            for (auto key :
                 {"schemaId",
                  "kind",
                  "schemaKey",
                  "metaType",
                  "typename",
                  "zserioType",
                  "attributeType",
                  "attributeTypeCode",
                  "requiredFields",
                  "nullable",
                  "keys",
                  "attributeOwners",
                  "enumBytes",
                  "bitmaskValues",
                  "domainDefinitions",
                  "edgeAlternatives",
                  "unknownEdge"})
            {
                extra.erase(key);
            }
            if (!extra.empty()) {
                result["x-mapget"] = std::move(extra);
            }
        }
        return result;
    }

    /** Validation-only schemas are graph references, never independent retained
     * JSON schema trees. Literal values (const/enum) are deliberately not walked. */
    template <class F>
    static void validationSchemas(nlohmann::json& annotations, F&& visit)
    {
        for (auto key :
             {"if",
              "then",
              "else",
              "not",
              "contains",
              "additionalProperties",
              "additionalItems",
              "propertyNames"})
            if (auto it = annotations.find(key); it != annotations.end() && it->is_object())
                visit(*it, std::string("/") + key);
        for (auto key : {"patternProperties", "dependencies"})
            if (auto it = annotations.find(key); it != annotations.end() && it->is_object())
                for (auto& [name, schema] : it->items())
                    if (schema.is_object())
                        visit(schema, std::string("/") + key + "/" + pointerToken(name));
    }

    /** Only value alternatives/array elements describe emitted roots. Object fields
     * may reference other feature types but do not declare their production. */
    std::vector<simfil::SchemaId> featureRoots(simfil::SchemaId root) const
    {
        std::vector<simfil::SchemaId> result, pending{root};
        std::vector<bool> seen(schemas_.size());
        while (!pending.empty()) {
            auto id = pending.back();
            pending.pop_back();
            if (!valid(id) || seen[id])
                continue;
            seen[id] = true;
            auto const& domain = schemas_[id];
            if (domain.kind_ == LayerSchema::FeatureKind || domain.entry_.metaType_ == "Feature") {
                result.push_back(id);
                continue;
            }
            pending.insert(pending.end(), domain.alternatives_.begin(), domain.alternatives_.end());
            pending.insert(
                pending.end(),
                domain.elementSchemas_.begin(),
                domain.elementSchemas_.end());
        }
        std::ranges::sort(result);
        return result;
    }

    [[nodiscard]] bool valid(simfil::SchemaId id) const
    {
        return id != simfil::NoSchemaId && id < schemas_.size() && schemas_[id].id_ == id;
    }

    [[nodiscard]] Kind kind(simfil::SchemaId id) const
    {
        return valid(id) ? schemas_[id].kind_ : Kind::Unknown;
    }

    [[nodiscard]] std::string_view metaType(simfil::SchemaId id) const
    {
        return valid(id) ? entriesById_[id].metaType_ : std::string_view{};
    }

    [[nodiscard]] std::string_view attributeTypeCode(simfil::SchemaId id) const
    {
        return valid(id) ? schemas_[id].attributeTypeCode_ : std::string_view{};
    }

    /** Allocate one domain, preserving a reserved canonical transport identity when supplied. */
    simfil::SchemaId allocate(
        Kind kind,
        std::string key,
        std::string pointer,
        std::string metaType,
        simfil::SchemaId requested = simfil::NoSchemaId)
    {
        if (requested == simfil::NoSchemaId && schemas_.size() > simfil::MaxSchemaId) {
            throw std::runtime_error("LayerSchema exhausted the uint16 SchemaId domain.");
        }

        auto id = requested == simfil::NoSchemaId ?
            static_cast<simfil::SchemaId>(schemas_.size()) :
            requested;
        Entry entry{id, key, pointer, std::move(metaType)};
        LogicalSchema schema;
        schema.id_ = id;
        schema.kind_ = kind;
        if (kind == Kind::Object && entry.metaType_ == "Feature") {
            schema.kind_ = LayerSchema::FeatureKind;
        }
        else if (kind == Kind::Object && entry.metaType_ == "Attribute") {
            schema.kind_ = LayerSchema::AttributeKind;
        }
        schema.entry_ = entry;
        if (id >= schemas_.size()) {
            schemas_.resize(size_t(id) + 1);
            entriesById_.resize(size_t(id) + 1);
        }
        schemas_[id] = std::move(schema);
        entriesById_[id] = std::move(entry);
        if (requested == simfil::NoSchemaId) {
            // Canonical import restores exact aliases from metadata. A descriptive
            // key can occur on several context-specific domains without owning its lookup.
            registerKey(key, id);
            registerKey(pointer, id);
        }
        return id;
    }

    void registerAttributeOwner(simfil::SchemaId id, AttributePathOwner owner)
    {
        if (!valid(id) || owner.featureType_.empty() || owner.attributeLayerName_.empty() ||
            owner.attributeName_.empty())
        {
            return;
        }

        owner.attributeSchema_ = id;
        auto& owners = schemas_[id].attributeOwners_;
        auto duplicate = std::ranges::find_if(
            owners,
            [&](auto const& existing)
            {
                return existing.featureType_ == owner.featureType_ &&
                    existing.attributeLayerName_ == owner.attributeLayerName_ &&
                    existing.attributeName_ == owner.attributeName_;
            });
        if (duplicate == owners.end()) {
            owners.push_back(std::move(owner));
        }
    }

    /** Retain producer metadata independently of dictionary IDs and derived reachability indexes.
     */
    void registerSchemaMetadata(
        simfil::SchemaId id,
        nlohmann::json const& schema,
        BuildContext const& context,
        std::string_view metaType,
        std::string_view pointerPrefix)
    {
        if (!valid(id)) {
            return;
        }

        auto const* metadata = mapgetMetadata(schema);
        auto zserioType = metadataString(metadata, "zserioType");
        if (!zserioType.empty()) {
            schemas_[id].zserioType_ = std::move(zserioType);
        }

        auto& domain = schemas_[id];
        domain.annotations_ = annotations(schema);
        if (!schema.contains("type") && (schema.contains("properties") || schema.contains("items")))
            domain.annotations_["x-mapget"]["jsonTypeImplicit"] = true;
        domain.typeName_ = metadataString(metadata, "typename");
        if (domain.typeName_.empty()) {
            domain.typeName_ = domain.zserioType_;
        }
        if (domain.typeName_.empty() && metaType == "Feature") {
            domain.typeName_ = context.featureType_;
        }
        if (schema.contains("type")) {
            domain.nullable_ = hasType(schema, "null");
        }
        if (auto it = schema.find("nullable"); it != schema.end() && it->is_boolean()) {
            domain.nullable_ = it->get<bool>();
        }
        if (metadata) {
            if (auto kind = jsonKind(metadata))
                domain.kind_ = *kind;
            if (auto it = metadata->find("bitmaskValues"); it != metadata->end()) {
                addEnumSymbols(id, it->get<std::vector<std::string>>());
            }
        }
        if (hasAffinity(domain.kind_, simfil::ValueType::Object) &&
            domain.composition_ == Composition::None)
        {
            // JSON Schema permits additional properties unless explicitly closed.
            auto additional = schema.find("additionalProperties");
            domain.open_ = additional == schema.end() || !additional->is_boolean() ||
                additional->get<bool>();
            // A pattern-defined field namespace is not finitely enumerable even
            // if JSON validation forbids names outside those patterns.
            if (auto patterns = schema.find("patternProperties");
                patterns != schema.end() && !patterns->empty())
                domain.open_ = true;
        }
        if (auto properties = schema.find("properties"); properties != schema.end() &&
            properties->is_object() && hasAffinity(domain.kind_, simfil::ValueType::Object))
        {
            auto required = schema.find("required");
            for (auto const& [field, _] : properties->items()) {
                domain.required_[field] = required != schema.end() && required->is_array() &&
                    std::find(required->begin(), required->end(), field) != required->end();
            }
        }

        if (metadata) {
            if (auto it = metadata->find("nullable"); it != metadata->end()) {
                domain.nullable_ = it->is_null() ?
                    std::nullopt :
                    std::optional<bool>(it->get<bool>());
            }
            if (auto it = metadata->find("requiredFields"); it != metadata->end()) {
                domain.required_ = it->get<decltype(domain.required_)>();
            }
            if (auto it = metadata->find("keys"); it != metadata->end()) {
                for (auto const& alias : *it) {
                    auto key = alias.get<std::string>();
                    auto [found, inserted] = idsByKey_.emplace(key, id);
                    if (!inserted && found->second != id)
                        throw std::invalid_argument("Conflicting explicit schema alias: " + key);
                }
            }
            else if (auto key = metadataString(metadata, "schemaKey"); !key.empty()) {
                registerKey(key, id);
            }
            if (auto it = metadata->find("attributeOwners"); it != metadata->end()) {
                for (auto const& owner : *it) {
                    registerAttributeOwner(
                        id,
                        {owner.at("featureType"), owner.at("layer"), owner.at("name"), id});
                }
            }
            if (auto it = metadata->find("enumBytes"); it != metadata->end()) {
                for (auto const& hex : *it) {
                    auto bytes = simfil::ByteArray::fromHex(hex.get<std::string>());
                    if (!bytes) {
                        throw std::invalid_argument("Invalid schema byte literal.");
                    }
                    domain.enumValues_.push_back(std::move(*bytes));
                }
            }
        }

        if (metaType != "Attribute") {
            return;
        }

        auto attributeName = metadataString(metadata, "attributeTypeCode");
        schemas_[id].attributeType_ = metadataString(metadata, "attributeType");
        if (domain.typeName_.empty()) {
            domain.typeName_ = schemas_[id].attributeType_;
        }
        schemas_[id].attributeTypeCode_ = attributeName;
        if (!context.featureType_.empty() && !context.attributeLayerName_.empty() &&
            !attributeName.empty() && !(metadata && metadata->contains("keys")))
        {
            // A definition can be reused by several owners, while two physical
            // definitions can use the same attribute code. Neither is first-wins.
            auto pointer = std::string_view(domain.entry_.jsonPointer_);
            if (pointer.starts_with(pointerPrefix))
                pointer.remove_prefix(pointerPrefix.size());
            auto key = LayerSchema::attributeKey(
                context.featureType_,
                context.attributeLayerName_,
                attributeName,
                pointer.empty() ? "#" : pointer);
            auto [found, inserted] = idsByKey_.emplace(key, id);
            if (!inserted && found->second != id)
                throw std::invalid_argument("Conflicting qualified attribute schema key: " + key);
        }
        registerAttributeOwner(
            id,
            AttributePathOwner{
                context.featureType_,
                context.attributeLayerName_,
                std::move(attributeName),
                id});
    }

    [[nodiscard]] std::optional<AttributePathOwner>
    uniqueAttributeOwner(simfil::SchemaId id, std::string_view featureType) const
    {
        if (!valid(id)) {
            return std::nullopt;
        }

        std::optional<AttributePathOwner> result;
        for (auto const& owner : schemas_[id].attributeOwners_) {
            if (owner.featureType_ != featureType) {
                continue;
            }
            if (result) {
                return std::nullopt;
            }
            result = owner;
        }
        return result;
    }

    [[nodiscard]] std::vector<std::string>
    constantTypeNames(simfil::SchemaId id, std::string_view symbolName) const
    {
        std::vector<std::string> result;
        std::vector<simfil::SchemaId> visited;
        collectConstantTypeNames(id, symbolName, visited, result);
        std::ranges::sort(result);
        auto duplicates = std::ranges::unique(result);
        result.erase(duplicates.begin(), duplicates.end());
        return result;
    }

    void collectConstantTypeNames(
        simfil::SchemaId id,
        std::string_view symbolName,
        std::vector<simfil::SchemaId>& visited,
        std::vector<std::string>& result) const
    {
        if (!valid(id) || std::ranges::find(visited, id) != visited.end()) {
            return;
        }
        visited.push_back(id);

        auto const& schema = schemas_[id];
        if (schema.attributeTypeCode_ == symbolName && !schema.attributeType_.empty()) {
            result.push_back(schema.attributeType_);
        }
        auto const hasDirectEnumSymbol = std::ranges::find(schema.directEnumSymbols_, symbolName) !=
            schema.directEnumSymbols_.end();
        if (!schema.zserioType_.empty() && hasDirectEnumSymbol) {
            result.push_back(schema.zserioType_);
        }

        for (auto const& [_, children] : schema.childSchemas_) {
            for (auto child : children) {
                collectConstantTypeNames(child, symbolName, visited, result);
            }
        }
        for (auto child : schema.elementSchemas_) {
            collectConstantTypeNames(child, symbolName, visited, result);
        }
        for (auto child : schema.alternatives_) {
            collectConstantTypeNames(child, symbolName, visited, result);
        }
    }

    void registerKey(std::string const& key, simfil::SchemaId id)
    {
        if (!key.empty() && id != simfil::NoSchemaId) {
            idsByKey_.emplace(key, id);
        }
    }

    void addDirectField(simfil::SchemaId parent, std::string_view fieldName)
    {
        if (!valid(parent)) {
            return;
        }
        auto& fields = schemas_[parent].directFields_;
        if (std::ranges::find(fields, fieldName) == fields.end()) {
            fields.emplace_back(fieldName);
        }
    }

    void addEnumSymbol(simfil::SchemaId parent, std::string_view symbolName)
    {
        if (!valid(parent)) {
            return;
        }
        auto& symbols = schemas_[parent].directEnumSymbols_;
        if (std::ranges::find(symbols, symbolName) == symbols.end()) {
            symbols.emplace_back(symbolName);
        }
    }

    void addEnumSymbols(simfil::SchemaId parent, std::span<const std::string> symbolNames)
    {
        for (auto const& symbolName : symbolNames) {
            addEnumSymbol(parent, symbolName);
        }
    }

    void addChild(simfil::SchemaId parent, std::string_view fieldName, simfil::SchemaId child)
    {
        if (!valid(parent) || !valid(child)) {
            return;
        }
        auto& children = schemas_[parent].childSchemas_[std::string(fieldName)];
        if (std::ranges::find(children, child) == children.end()) {
            children.push_back(child);
        }
    }

    void addElementSchema(simfil::SchemaId parent, simfil::SchemaId child)
    {
        if (!valid(parent) || !valid(child)) {
            return;
        }
        auto& children = schemas_[parent].elementSchemas_;
        if (std::ranges::find(children, child) == children.end()) {
            children.push_back(child);
        }
    }

    /** Encode directly built domains as JSON Schema definitions with stable model IDs. */
    nlohmann::json exportDomains() const
    {
        // Use the dialect understood by our validator, including recursive definitions.
        auto result = nlohmann::json{
            {"$schema", "http://json-schema.org/draft-07/schema#"},
            {"definitions", nlohmann::json::object()}};
        std::map<simfil::SchemaId, std::vector<std::string_view>> aliases;
        for (auto const& [key, id] : idsByKey_) {
            aliases[id].push_back(key);
        }
        auto reference = [](simfil::SchemaId id)
        {
            return nlohmann::json{{"$ref", "#/definitions/" + std::to_string(id)}};
        };
        auto edges = [&](std::span<simfil::SchemaId const> ids)
        {
            if (ids.empty()) {
                return nlohmann::json{{"x-mapget", {{"unknownEdge", true}}}};
            }
            if (ids.size() == 1) {
                return reference(ids.front());
            }
            auto alternatives = nlohmann::json::array();
            for (auto id : ids) {
                alternatives.push_back(reference(id));
            }
            return nlohmann::json{
                {"anyOf", std::move(alternatives)},
                {"x-mapget", {{"edgeAlternatives", true}}}};
        };
        for (auto const& domain : schemas_) {
            if (domain.id_ == simfil::NoSchemaId || domain.queryRoot_) {
                continue;  // Completion overlays are derived, not binary model identities.
            }
            auto& out = result["definitions"][std::to_string(domain.id_)];
            out = domain.annotations_.is_null() ? nlohmann::json::object() : domain.annotations_;
            auto& meta = out["x-mapget"];
            if (meta.is_null()) {
                meta = nlohmann::json::object();
            }
            meta.update(
                {{"schemaId", domain.id_},
                 {"kind", exportKind(domain.kind_)},
                 {"schemaKey", domain.entry_.key_},
                 {"metaType", domain.entry_.metaType_},
                 {"typename", domain.typeName_},
                 {"zserioType", domain.zserioType_},
                 {"attributeType", domain.attributeType_},
                 {"attributeTypeCode", domain.attributeTypeCode_},
                 {"requiredFields", domain.required_},
                 {"nullable",
                  domain.nullable_ ? nlohmann::json(*domain.nullable_) : nlohmann::json(nullptr)}});
            // An explicit empty alias list prevents a different layer's same-named
            // descriptive schemaKey from claiming the selected layer's lookup key.
            meta["keys"] = nlohmann::json::array();
            for (auto key : aliases[domain.id_]) {
                meta["keys"].push_back(key);
            }
            for (auto const& owner : domain.attributeOwners_) {
                meta["attributeOwners"].push_back(
                    {{"featureType", owner.featureType_},
                     {"layer", owner.attributeLayerName_},
                     {"name", owner.attributeName_}});
            }
            if (domain.composition_ != Composition::None) {
                auto op = domain.composition_ == Composition::AllOf ?
                    "allOf" :
                    domain.composition_ == Composition::OneOf ?
                    "oneOf" :
                    "anyOf";
                out[op] = nlohmann::json::array();
                for (auto id : domain.alternatives_) {
                    out[op].push_back(reference(id));
                }
            }
            else if (
                hasAffinity(domain.kind_, simfil::ValueType::Object) &&
                simfil::Schema::kindNameId(domain.kind_) !=
                    simfil::Schema::kindNameId(Kind::Unknown) &&
                simfil::Schema::kindNameId(domain.kind_) != simfil::Schema::kindNameId(Kind::Any))
            {
                out["type"] = "object";
                if (!out.contains("additionalProperties"))
                    out["additionalProperties"] = domain.open_;
                out["properties"] = nlohmann::json::object();
                for (auto const& field : domain.directFields_) {
                    auto children = domain.childSchemas_.find(field);
                    out["properties"][field] = children == domain.childSchemas_.end() ?
                        edges({}) :
                        edges(children->second);
                    if (domain.multimapFields_.contains(field)) {
                        // Duplicate-key model objects become arrays only in the JSON projection.
                        auto value = std::move(out["properties"][field]);
                        out["properties"][field] = {
                            {"x-mapget-multimap", true},
                            {"anyOf",
                             nlohmann::json::array(
                                 {value, {{"type", "array"}, {"items", value}}})}};
                    }
                }
                for (auto const& [field, required] : domain.required_) {
                    if (required &&
                        (!out.contains("required") ||
                         std::find(out["required"].begin(), out["required"].end(), field) ==
                             out["required"].end()))
                    {
                        out["required"].push_back(field);
                    }
                }
                if (!domain.multimapFields_.empty()) {
                    out["properties"]["_multimap"] = {{"const", true}};
                }
            }
            else if (hasAffinity(domain.kind_, simfil::ValueType::Array)) {
                auto name = simfil::Schema::kindNameId(domain.kind_);
                if (name != simfil::Schema::kindNameId(Kind::Unknown) &&
                    name != simfil::Schema::kindNameId(Kind::Any))
                {
                    out["type"] = "array";
                    if (!domain.elementSchemas_.empty()) {
                        out["items"] = edges(domain.elementSchemas_);
                    }
                }
            }
            else {
                auto name = simfil::Schema::kindNameId(domain.kind_);
                for (auto [kind, type] :
                     {std::pair{Kind::String, "string"},
                      {Kind::Int, "integer"},
                      {Kind::Float, "number"},
                      {Kind::Bool, "boolean"},
                      {Kind::Null, "null"}})
                {
                    if (name == simfil::Schema::kindNameId(kind)) {
                        out["type"] = type;
                    }
                }
                if (name == simfil::Schema::kindNameId(Kind::Never)) {
                    out["not"] = nlohmann::json::object();
                }
                if (isBitmaskKind(domain.kind_)) {
                    // Native masks are lossless tagged scalars, not flag-label strings.
                    // Exact graph identity and encoding-width checks belong to bound import.
                    out["type"] = "object";
                    out["required"] = {"_bitmask", "bits", "schema", "domain"};
                    out["additionalProperties"] = false;
                    out["properties"] = {
                        {"_bitmask", {{"const", true}}},
                        {"bits", {{"type", "string"}, {"pattern", "^0x[0-9a-f]{16}$"}}},
                        {"schema", {{"type", "string"}, {"minLength", 1}}},
                        {"domain",
                         {{"type", "integer"}, {"minimum", 1}, {"maximum", simfil::MaxSchemaId}}}};
                }
                else if (name == simfil::Schema::kindNameId(LayerSchema::BitmaskKind)) {
                    // Individual flags are not an exhaustive enum of rendered combinations.
                    out["type"] = "string";
                }
                if (domain.kind_ == Kind::Bytes) {
                    // The generic ModelNode JSON codec projects binary scalars to this object.
                    out["type"] = "object";
                    out["required"] = {"_bytes", "hex", "number"};
                    out["properties"] = {
                        {"_bytes", {{"const", true}}},
                        {"hex", {{"type", "string"}}},
                        {"number", {{"type", {"integer", "null"}}}}};
                }
            }
            if (domain.nullable_.value_or(false) && out.contains("type") && out["type"] != "null") {
                out["type"] = nlohmann::json::array({out["type"], "null"});
            }
            if (meta.value("jsonTypeImplicit", false))
                out.erase("type");
            bool literalEnum = out.contains("enum") || out.contains("const");
            for (auto const& symbol : domain.directEnumSymbols_) {
                if (simfil::Schema::kindNameId(domain.kind_) ==
                    simfil::Schema::kindNameId(LayerSchema::BitmaskKind))
                {
                    meta["bitmaskValues"].push_back(symbol);
                }
                else if (!literalEnum) {
                    out["enum"].push_back(symbol);
                }
            }
            for (auto const& value : domain.enumValues_) {
                if (literalEnum)
                    break;
                std::visit(
                    [&](auto const& literal)
                    {
                        using T = std::decay_t<decltype(literal)>;
                        if constexpr (std::is_same_v<T, simfil::ByteArray>) {
                            meta["enumBytes"].push_back(literal.toHex());
                        }
                        else if constexpr (std::is_same_v<T, simfil::Bitmask>) {
                            throw std::invalid_argument(
                                "Bitmask constants must use named mask declarations.");
                        }
                        else if constexpr (std::is_same_v<T, std::monostate>) {
                            out["enum"].push_back(nullptr);
                        }
                        else {
                            out["enum"].push_back(literal);
                        }
                    },
                    value);
            }
        }
        // An envelope introduces no extra schema identity before the producer's IDs.
        result["x-mapget"] = {{"domainDefinitions", true}};
        for (auto const& domain : schemas_) {
            if (!domain.queryRoot_ && domain.entry_.metaType_ == "Feature") {
                result["anyOf"].push_back(reference(domain.id_));
            }
        }
        return result;
    }

    void forEachDirectField(
        simfil::SchemaId id,
        const std::function<void(std::string_view, std::span<const simfil::SchemaId>)>& fn) const
    {
        if (!valid(id)) {
            return;
        }

        auto const& schema = schemas_[id];
        for (auto const& fieldName : schema.directFields_) {
            auto childIt = schema.childSchemas_.find(fieldName);
            if (childIt == schema.childSchemas_.end()) {
                fn(fieldName, {});
            }
            else {
                fn(fieldName, childIt->second);
            }
        }
    }

    void
    forEachElementSchema(simfil::SchemaId id, const std::function<void(simfil::SchemaId)>& fn) const
    {
        if (!valid(id)) {
            return;
        }

        for (auto childSchemaId : schemas_[id].elementSchemas_) {
            fn(childSchemaId);
        }
    }

    [[nodiscard]] simfil::SchemaId childSchema(
        simfil::SchemaId parent,
        std::string_view fieldName,
        std::optional<Kind> preferredKind) const
    {
        std::vector<simfil::SchemaId> visited;
        return childSchema(parent, fieldName, preferredKind, visited);
    }

    [[nodiscard]] simfil::SchemaId childSchema(
        simfil::SchemaId parent,
        std::string_view fieldName,
        std::optional<Kind> preferredKind,
        std::vector<simfil::SchemaId>& visited) const
    {
        if (!valid(parent)) {
            return simfil::NoSchemaId;
        }
        if (std::ranges::find(visited, parent) != visited.end()) {
            return simfil::NoSchemaId;
        }
        visited.push_back(parent);

        auto const& schema = schemas_[parent];
        for (auto alternative : schema.alternatives_) {
            auto resolved = childSchema(alternative, fieldName, preferredKind, visited);
            if (resolved != simfil::NoSchemaId) {
                return resolved;
            }
        }
        if (hasAffinity(schema.kind_, simfil::ValueType::Array)) {
            for (auto child : schema.elementSchemas_) {
                auto resolved = childSchema(child, fieldName, preferredKind, visited);
                if (resolved != simfil::NoSchemaId) {
                    return resolved;
                }
            }
            return simfil::NoSchemaId;
        }

        auto fieldIt = schema.childSchemas_.find(fieldName);
        if (fieldIt == schema.childSchemas_.end()) {
            return simfil::NoSchemaId;
        }

        for (auto id : fieldIt->second) {
            if (!preferredKind || matchesKind(kind(id), *preferredKind)) {
                return id;
            }
        }
        return fieldIt->second.empty() ? simfil::NoSchemaId : fieldIt->second.front();
    }

    [[nodiscard]] std::optional<LayerSchema::NamedSchemaPath>
    firstScalarFieldPath(simfil::SchemaId id, bool skipRootAttributeMetadataFields = false) const
    {
        std::vector<simfil::SchemaId> visited;
        LayerSchema::NamedSchemaPath current;
        return firstScalarFieldPath(id, visited, current, skipRootAttributeMetadataFields, true);
    }

    [[nodiscard]] std::optional<LayerSchema::NamedSchemaPath> firstScalarFieldPath(
        simfil::SchemaId id,
        std::vector<simfil::SchemaId>& visited,
        LayerSchema::NamedSchemaPath& current,
        bool skipRootAttributeMetadataFields,
        bool isRoot) const
    {
        if (!valid(id) || std::ranges::find(visited, id) != visited.end()) {
            return std::nullopt;
        }

        auto const& schema = schemas_[id];
        if (matchesKind(schema.kind_, Kind::Value)) {
            return current;
        }

        visited.push_back(id);
        for (auto alternative : schema.alternatives_) {
            if (auto result = firstScalarFieldPath(
                    alternative,
                    visited,
                    current,
                    skipRootAttributeMetadataFields,
                    isRoot))
            {
                visited.pop_back();
                return result;
            }
        }
        if (hasAffinity(schema.kind_, simfil::ValueType::Object)) {
            for (auto const& fieldName : schema.directFields_) {
                if (isRoot && skipRootAttributeMetadataFields &&
                    isAttributeScalarShorthandMetadataField(fieldName))
                {
                    continue;
                }
                current.push_back({simfil::SchemaPathSegment::Kind::Field, fieldName});
                auto childIt = schema.childSchemas_.find(fieldName);
                if (childIt == schema.childSchemas_.end() || childIt->second.empty()) {
                    // Missing metadata is unknown, not proof of a scalar payload.
                    current.pop_back();
                    continue;
                }
                for (auto child : childIt->second) {
                    if (auto result = firstScalarFieldPath(
                            child,
                            visited,
                            current,
                            skipRootAttributeMetadataFields,
                            false))
                    {
                        current.pop_back();
                        visited.pop_back();
                        return result;
                    }
                }
                current.pop_back();
            }
        }
        else {
            for (auto child : schema.elementSchemas_) {
                current.push_back({simfil::SchemaPathSegment::Kind::ArrayElement, {}});
                if (auto result = firstScalarFieldPath(
                        child,
                        visited,
                        current,
                        skipRootAttributeMetadataFields,
                        false))
                {
                    current.pop_back();
                    visited.pop_back();
                    return result;
                }
                current.pop_back();
            }
        }

        visited.pop_back();
        return std::nullopt;
    }

    [[nodiscard]] std::vector<LayerSchema::NamedSchemaPath>
    scalarFieldPathsForAttribute(simfil::SchemaId rootSchema, std::string_view attributeTypeCode)
        const
    {
        std::vector<LayerSchema::NamedSchemaPath> paths;
        std::vector<simfil::SchemaId> visited;
        LayerSchema::NamedSchemaPath current;
        collectScalarFieldPathsForAttribute(rootSchema, attributeTypeCode, visited, current, paths);
        std::ranges::sort(paths);
        auto duplicates = std::ranges::unique(paths);
        paths.erase(duplicates.begin(), duplicates.end());
        return paths;
    }

    void collectScalarFieldPathsForAttribute(
        simfil::SchemaId id,
        std::string_view attributeTypeCode,
        std::vector<simfil::SchemaId>& visited,
        LayerSchema::NamedSchemaPath& current,
        std::vector<LayerSchema::NamedSchemaPath>& paths) const
    {
        if (!valid(id) || std::ranges::find(visited, id) != visited.end()) {
            return;
        }

        auto const& schema = schemas_[id];
        if (schema.attributeTypeCode_ == attributeTypeCode) {
            if (auto suffix = firstScalarFieldPath(id, true)) {
                auto path = current;
                path.insert(path.end(), suffix->begin(), suffix->end());
                paths.push_back(std::move(path));
            }
            return;
        }

        visited.push_back(id);
        for (auto alternative : schema.alternatives_) {
            collectScalarFieldPathsForAttribute(
                alternative,
                attributeTypeCode,
                visited,
                current,
                paths);
        }
        if (hasAffinity(schema.kind_, simfil::ValueType::Object)) {
            for (auto const& fieldName : schema.directFields_) {
                if (schema.entry_.metaType_ == "Feature" && fieldName == "attributes") {
                    // Do not let the feature-root alias duplicate canonical
                    // `properties...` paths generated for shorthand rewrites.
                    continue;
                }
                auto childIt = schema.childSchemas_.find(fieldName);
                if (childIt == schema.childSchemas_.end()) {
                    continue;
                }
                current.push_back({simfil::SchemaPathSegment::Kind::Field, fieldName});
                for (auto child : childIt->second) {
                    collectScalarFieldPathsForAttribute(
                        child,
                        attributeTypeCode,
                        visited,
                        current,
                        paths);
                }
                current.pop_back();
            }
        }
        else if (hasAffinity(schema.kind_, simfil::ValueType::Array)) {
            current.push_back({simfil::SchemaPathSegment::Kind::ArrayElement, {}});
            for (auto child : schema.elementSchemas_) {
                collectScalarFieldPathsForAttribute(
                    child,
                    attributeTypeCode,
                    visited,
                    current,
                    paths);
            }
            current.pop_back();
        }
        visited.pop_back();
    }
};

/** Recursive JSON Schema compiler used by LayerSchema construction. */
class LayerSchemaCompiler
{
public:
    LayerSchemaCompiler(SchemaBuilder& registry, nlohmann::json const& root)
        : registry_(registry), root_(root)
    {
    }

    /** Reserve canonical IDs before following references; ordinary input uses deterministic
     * traversal. */
    simfil::SchemaId buildAll()
    {
        auto metadata = mapgetMetadata(root_);
        if (metadata && metadata->value("domainDefinitions", false)) {
            auto const* keyword = root_.contains("definitions") ? "definitions" : "$defs";
            auto const& definitions = root_.at(keyword);
            auto prefix = "/" + std::string(keyword) + "/";
            size_t highest = 0;
            for (auto const& [name, definition] : definitions.items()) {
                auto const& encodedId = definition.at("x-mapget").at("schemaId");
                if (!encodedId.is_number_integer() ||
                    (!encodedId.is_number_unsigned() && encodedId.get<int64_t>() < 0))
                    throw std::invalid_argument("Invalid transported schema ID.");
                auto id = encodedId.get<uint64_t>();
                if (id == 0 || id > simfil::MaxSchemaId || !reservedIds_.insert(id).second) {
                    throw std::invalid_argument("Invalid or duplicate transported schema ID.");
                }
                highest = std::max(highest, size_t(id));
                transportedIds_[prefix + pointerToken(name)] = static_cast<simfil::SchemaId>(id);
            }
            registry_.schemas_.resize(highest + 1);
            registry_.entriesById_.resize(highest + 1);
            for (auto const& [name, definition] : definitions.items()) {
                build(definition, prefix + pointerToken(name), {}, std::nullopt);
            }
        }
        // The envelope's actual root is significant, including non-feature roots.
        // Canonical definitions alone must not imply that every feature is produced.
        return build(root_, "#", {}, std::nullopt);
    }

    /** Append an input fragment without confusing its local references with another fragment. */
    simfil::SchemaId append()
    {
        pointerPrefix_ = "#/import/" + std::to_string(registry_.schemas_.size());
        return build(root_, pointerPrefix_, {}, std::nullopt);
    }

private:
    void compileValidation(
        simfil::SchemaId id,
        nlohmann::json const& schema,
        std::string const& pointer,
        BuildContext const& context)
    {
        auto annotations = registry_.schemas_[id].annotations_;
        if (registry_.schemas_[id].kind_ == Kind::Never &&
            annotations.value("not", nlohmann::json(nullptr)) == nlohmann::json::object())
        {
            // The JSON projection of Never is not an additional native domain.
            annotations.erase("not");
        }
        SchemaBuilder::validationSchemas(
            annotations,
            [&](nlohmann::json& value, std::string const& suffix)
            {
                auto child = build(value, pointer + suffix, context, std::nullopt);
                value = {{"$ref", "#/definitions/" + std::to_string(child)}};
            });
        registry_.schemas_[id].annotations_ = std::move(annotations);
    }

    /** Honor stable IDs only in the canonical transport envelope, never arbitrary annotations. */
    simfil::SchemaId allocate(Kind kind, std::string key, std::string pointer, std::string metaType)
    {
        auto found = transportedIds_.find(pointer);
        auto requested = found == transportedIds_.end() ? simfil::NoSchemaId : found->second;
        return registry_
            .allocate(kind, std::move(key), std::move(pointer), std::move(metaType), requested);
    }

    /** A plural field edge is not an extra union definition in the producer's namespace. */
    std::vector<simfil::SchemaId> buildEdges(
        nlohmann::json const& schema,
        std::string const& pointer,
        BuildContext const& context)
    {
        if (isMapgetMultimap(schema)) {
            // Unwrap before reading plural edges; the native value can itself be an array.
            auto combiner = std::string(combinerKey(schema));
            return buildEdges(schema.at(combiner).at(0), pointer + "/" + combiner + "/0", context);
        }
        auto metadata = mapgetMetadata(schema);
        if (metadata && metadata->value("unknownEdge", false)) {
            return {};
        }
        if (metadata && metadata->value("edgeAlternatives", false)) {
            std::vector<simfil::SchemaId> result;
            auto const& branches = schema.at("anyOf");
            for (size_t i = 0; i < branches.size(); ++i) {
                result.push_back(build(
                    branches[i],
                    pointer + "/anyOf/" + std::to_string(i),
                    context,
                    std::nullopt));
            }
            return result;
        }
        return {build(schema, pointer, context, std::nullopt)};
    }

    /** Compile domains without flattening logical alternatives into a selected branch. */
    simfil::SchemaId build(
        nlohmann::json const& schema,
        std::string pointer,
        BuildContext context,
        std::optional<Kind> preferredKind)
    {
        if (depth_ >= 256 || ++work_ > 4 * 1024 * 1024)
            throw std::length_error("JSON schema import traversal budget exceeded.");
        ++depth_;
        auto exitDepth = [](size_t* depth)
        {
            --*depth;
        };
        std::unique_ptr<size_t, decltype(exitDepth)> depthGuard(&depth_, exitDepth);
        auto const* metadata = mapgetMetadata(schema);
        auto declaredKind = SchemaBuilder::jsonKind(metadata);
        auto metaType = metadataString(metadata, "metaType");
        if (auto feature = metadataString(metadata, "featureType"); !feature.empty()) {
            context.featureType_ = std::move(feature);
        }
        auto key = reservedIds_.empty() ?
            annotatedKey(schema, pointer, context) :
            metadataString(metadata, "schemaKey");
        auto memoKey = contextMemoKey(pointer, preferredKind, context);
        if (auto found = transportedIds_.find(pointer);
            found != transportedIds_.end() && registry_.valid(found->second))
        {
            return found->second;
        }
        if (auto found = memo_.find(memoKey); found != memo_.end()) {
            registry_.registerKey(key, found->second);
            return found->second;
        }
        if (!schema.is_object()) {
            auto kind = schema.is_boolean() ?
                (schema.get<bool>() ? Kind::Any : Kind::Never) :
                Kind::Unknown;
            auto id = allocate(kind, key, pointer, metaType);
            memo_[memoKey] = id;
            return id;
        }

        // The duplicate-key JSON representation is not an array-valued runtime domain.
        if (isMapgetMultimap(schema)) {
            auto combiner = std::string(combinerKey(schema));
            auto id = build(
                schema.at(combiner).at(0),
                pointer + "/" + combiner + "/0",
                context,
                std::nullopt);
            memo_[memoKey] = id;
            registry_.registerKey(key, id);
            return id;
        }

        auto constraints = schema;
        constraints.erase("x-mapget");
        constraints.erase("$defs");
        constraints.erase("definitions");
        std::vector<std::pair<std::string, nlohmann::json>> terms;
        for (std::string const name : {"$ref", "anyOf", "oneOf", "allOf"}) {
            if (auto it = schema.find(name); it != schema.end()) {
                terms.emplace_back(name, *it);
                constraints.erase(name);
            }
        }
        auto hasBase = isObjectSchema(constraints) || isArraySchema(constraints) ||
            isValueSchema(constraints);
        if (!terms.empty()) {
            if (terms.size() == 1 && !hasBase && terms.front().first == "$ref") {
                auto ref = terms.front().second.get<std::string>();
                if (!pointerPrefix_.empty()) {
                    // Converter fragments may refer to shared domains already in this graph.
                    if (auto found = registry_.idsByKey_.find(ref);
                        found != registry_.idsByKey_.end())
                    {
                        return found->second;
                    }
                }
                // Pure reference cycles have no concrete domain to expand.
                if (!activeRefs_.insert(memoKey).second) {
                    auto id = allocate(Kind::Unknown, key, pointer, metaType);
                    memo_[memoKey] = id;
                    return id;
                }
                auto id = build(
                    resolveLocalRef(root_, ref),
                    pointerPrefix_ + refToPointer(ref),
                    context,
                    preferredKind);
                activeRefs_.erase(memoKey);
                memo_[memoKey] = id;
                registry_.registerKey(key, id);
                return id;
            }
            auto oneCombiner = terms.size() == 1 && !hasBase;
            auto op = oneCombiner ? terms.front().first : std::string("allOf");
            auto kind = op == "allOf" ?
                Kind::Intersection :
                op == "oneOf" ?
                Kind::OneOf :
                Kind::Union;
            auto id = allocate(kind, key, pointer, metaType);
            memo_[memoKey] = id;  // Publish before children so recursive references terminate.
            registry_.schemas_[id].composition_ = op == "allOf" ?
                Composition::AllOf :
                op == "oneOf" ?
                Composition::OneOf :
                Composition::AnyOf;
            registry_.registerSchemaMetadata(id, schema, context, metaType, pointerPrefix_);
            compileValidation(id, schema, pointer, context);
            if (oneCombiner) {
                if (!terms.front().second.is_array()) {
                    throw std::invalid_argument("LayerSchema combiner must be an array.");
                }
                auto const& branches = terms.front().second;
                for (size_t i = 0; i < branches.size(); ++i) {
                    auto child = build(
                        branches[i],
                        pointer + "/" + op + "/" + std::to_string(i),
                        context,
                        std::nullopt);
                    registry_.schemas_[id].alternatives_.push_back(child);
                }
            }
            else {
                for (auto const& [name, term] : terms) {
                    auto child = build(
                        nlohmann::json{{name, term}},
                        pointer + "/@" + name,
                        context,
                        std::nullopt);
                    registry_.schemas_[id].alternatives_.push_back(child);
                }
                if (hasBase) {
                    auto child = build(constraints, pointer + "/@base", context, std::nullopt);
                    registry_.schemas_[id].alternatives_.push_back(child);
                }
            }
            return id;
        }
        if (auto types = schema.find("type");
            types != schema.end() && types->is_array() && !declaredKind)
        {
            auto branches = nlohmann::json::array();
            for (auto const& type : *types) {
                auto branch = schema;
                branch["type"] = type;
                branch.erase("x-mapget");
                branches.push_back(std::move(branch));
            }
            auto combined = nlohmann::json{{"anyOf", std::move(branches)}};
            if (metadata) {
                combined["x-mapget"] = *metadata;
            }
            auto id = build(combined, pointer, context, preferredKind);
            registry_.schemas_[id].nullable_ = hasType(schema, "null");
            return id;
        }
        if (declaredKind &&
            (simfil::Schema::kindNameId(*declaredKind) == simfil::Schema::kindNameId(Kind::Bytes) ||
             SchemaBuilder::isBitmaskKind(*declaredKind)))
        {
            // Tagged byte/mask projections are scalars, not object fields in the native graph.
            return buildValue(
                schema,
                std::move(pointer),
                std::move(context),
                std::move(key),
                std::move(metaType),
                memoKey);
        }
        if (isObjectSchema(schema)) {
            return buildObject(
                schema,
                std::move(pointer),
                std::move(context),
                std::move(key),
                std::move(metaType),
                memoKey);
        }
        if (isArraySchema(schema)) {
            return buildArray(
                schema,
                std::move(pointer),
                std::move(context),
                std::move(key),
                std::move(metaType),
                memoKey);
        }
        return buildValue(
            schema,
            std::move(pointer),
            std::move(context),
            std::move(key),
            std::move(metaType),
            memoKey);
    }

    simfil::SchemaId buildObject(
        nlohmann::json const& schema,
        std::string pointer,
        BuildContext context,
        std::string key,
        std::string metaType,
        std::string const& memoKey)
    {
        auto id = allocate(Kind::Object, std::move(key), pointer, metaType);
        registry_.registerSchemaMetadata(id, schema, context, metaType, pointerPrefix_);
        memo_[memoKey] = id;
        compileValidation(id, schema, pointer, context);

        auto propertiesIt = schema.find("properties");
        if (propertiesIt == schema.end() || !propertiesIt->is_object()) {
            return id;
        }

        auto const shouldAddFeatureAttributesAlias = reservedIds_.empty() &&
            metaType == "Feature" && propertiesIt->contains("properties") &&
            !propertiesIt->contains("attributes");

        for (auto const& [fieldName, childSchemaJson] : propertiesIt->items()) {
            if (!reservedIds_.empty() && fieldName == "_multimap" &&
                childSchemaJson == nlohmann::json{{"const", true}})
            {
                continue;  // JSON-only projection marker, never a native field.
            }
            registry_.addDirectField(id, fieldName);
            if (isMapgetMultimap(childSchemaJson)) {
                registry_.schemas_[id].multimapFields_.insert(fieldName);
            }

            auto childContext = context;
            if (metaType == "AttributeLayerMap") {
                childContext.attributeLayerName_ = fieldName;
            }

            auto const childPointer = pointer + "/properties/" + pointerToken(fieldName);
            for (auto childId : buildEdges(childSchemaJson, childPointer, childContext)) {
                registry_.addChild(id, fieldName, childId);
                if (shouldAddFeatureAttributesAlias && fieldName == "properties") {
                    registry_.addChild(id, "attributes", childId);
                }
            }
        }
        return id;
    }

    simfil::SchemaId buildArray(
        nlohmann::json const& schema,
        std::string pointer,
        BuildContext context,
        std::string key,
        std::string metaType,
        std::string const& memoKey)
    {
        auto id = allocate(Kind::Array, std::move(key), pointer, metaType);
        registry_.registerSchemaMetadata(id, schema, context, metaType, pointerPrefix_);
        memo_[memoKey] = id;
        compileValidation(id, schema, pointer, context);

        auto itemsIt = schema.find("items");
        if (itemsIt == schema.end()) {
            return id;
        }

        for (auto childId : buildEdges(*itemsIt, pointer + "/items", context)) {
            registry_.addElementSchema(id, childId);
        }
        return id;
    }

    simfil::SchemaId buildValue(
        nlohmann::json const& schema,
        std::string pointer,
        BuildContext context,
        std::string key,
        std::string metaType,
        std::string const& memoKey)
    {
        auto kind = hasType(schema, "string") ? Kind::String :
            hasType(schema, "integer")        ? Kind::Int :
            hasType(schema, "number")         ? Kind::Float :
            hasType(schema, "boolean")        ? Kind::Bool :
            hasType(schema, "null")           ? Kind::Null :
                                                Kind::Unknown;
        if (kind == Kind::Float) {
            kind = simfil::Schema::makeKind(
                simfil::Schema::kindNameId(kind),
                simfil::valueTypeAffinity(simfil::ValueType::Float) |
                    simfil::valueTypeAffinity(simfil::ValueType::Int));
        }
        auto literals = nlohmann::json::array();
        if (auto it = schema.find("enum"); it != schema.end() && it->is_array()) {
            literals = *it;
        }
        if (auto it = schema.find("const"); it != schema.end()) {
            if (schema.contains("enum")) {
                // Both constraints apply: this is an intersection, not a union
                // (or a duplicated literal when the constant is already listed).
                auto value = std::ranges::find(literals, *it);
                literals = value == literals.end() ?
                    nlohmann::json::array() :
                    nlohmann::json::array({*it});
                if (literals.empty())
                    kind = Kind::Never;
            }
            else
                literals.push_back(*it);
        }
        uint16_t literalTypes = 0;
        std::vector<simfil::ScalarValueType> values;
        std::vector<std::string> symbols;
        for (auto const& value : literals) {
            if (value.is_string()) {
                // Keep declaration order for stable native transport roundtrips.
                symbols.push_back(value.get<std::string>());
                literalTypes |= simfil::valueTypeAffinity(simfil::ValueType::String);
            }
            else if (value.is_boolean()) {
                values.emplace_back(value.get<bool>());
                literalTypes |= simfil::valueTypeAffinity(simfil::ValueType::Bool);
            }
            else if (value.is_number_integer()) {
                if (value.is_number_unsigned() && value.get<uint64_t>() > uint64_t(INT64_MAX)) {
                    throw std::invalid_argument(
                        "Schema integer literal exceeds the query value domain.");
                }
                values.emplace_back(value.get<int64_t>());
                literalTypes |= simfil::valueTypeAffinity(simfil::ValueType::Int);
            }
            else if (value.is_number_float()) {
                values.emplace_back(value.get<double>());
                literalTypes |= simfil::valueTypeAffinity(simfil::ValueType::Float);
            }
            else if (value.is_null()) {
                values.emplace_back(std::monostate{});
                literalTypes |= simfil::valueTypeAffinity(simfil::ValueType::Null);
            }
            else {
                // Compound constants are not modeled as scalar enums; do not prove absence.
                literalTypes |= simfil::Schema::AllAffinities;
            }
        }
        if (kind == Kind::Unknown && literalTypes) {
            kind = Kind::Value;
            for (auto scalar : {Kind::String, Kind::Int, Kind::Float, Kind::Bool, Kind::Null}) {
                if (simfil::Schema::affinities(scalar) == literalTypes) {
                    kind = scalar;
                }
            }
            if (literalTypes &
                (simfil::valueTypeAffinity(simfil::ValueType::Object) |
                 simfil::valueTypeAffinity(simfil::ValueType::Array)))
            {
                kind = Kind::Unknown;
            }
            else {
                kind = simfil::Schema::makeKind(simfil::Schema::kindNameId(kind), literalTypes);
            }
        }
        auto id = allocate(kind, std::move(key), pointer, metaType);
        registry_.schemas_[id].enumValues_ = std::move(values);
        registry_.registerSchemaMetadata(id, schema, context, metaType, pointerPrefix_);
        memo_[memoKey] = id;
        compileValidation(id, schema, pointer, context);

        registry_.addEnumSymbols(id, symbols);
        return id;
    }

    SchemaBuilder& registry_;
    nlohmann::json const& root_;
    std::map<std::string, simfil::SchemaId> memo_;
    std::set<std::string> activeRefs_;
    std::set<simfil::SchemaId> reservedIds_;
    std::map<std::string, simfil::SchemaId> transportedIds_;
    std::string pointerPrefix_;
    size_t depth_ = 0, work_ = 0;
};

}  // namespace mapget::schema_detail
