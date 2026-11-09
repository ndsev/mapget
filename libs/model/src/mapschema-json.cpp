#include "mapschema-json.h"
#include "mapschema-impl.h"

#include <picosha2.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace mapget::schema_detail
{
namespace
{
// The first column is the pre-v6 JSON name ID, not today's StringPool ID.
// New projections carry stable names: legacy array (15 << 16 | 512) is
// numerically identical to today's object, so layout autodetection is unsafe.
static constexpr std::pair<uint16_t, Kind> LEGACY_JSON_KINDS[]{
    {4, Kind::Unknown},
    {5, Kind::Any},
    {6, Kind::Never},
    {7, Kind::Value},
    {8, Kind::Null},
    {9, Kind::Bool},
    {10, Kind::Int},
    {11, Kind::Float},
    {12, Kind::String},
    {13, Kind::Bytes},
    {14, Kind::Object},
    {15, Kind::Array},
    {16, Kind::Union},
    {17, Kind::OneOf},
    {18, Kind::Intersection},
    {124, LayerSchema::FeatureKind},
    {125, LayerSchema::AttributeKind},
    {131, LayerSchema::BitmaskKind}};

static constexpr std::pair<simfil::ValueType, std::string_view> JSON_AFFINITIES[]{
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

}  // namespace

bool hasAffinity(Kind kind, simfil::ValueType type)
{
    return (simfil::SchemaType::affinities(kind) & simfil::valueTypeAffinity(type)) != 0;
}

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
            !hasAffinity(kind, simfil::ValueType::Array) &&
            simfil::SchemaType::affinities(kind) != 0;
    }
    return kind == requested;
}

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

nlohmann::json const* mapgetMetadata(nlohmann::json const& schema)
{
    auto it = schema.find("x-mapget");
    if (it == schema.end() || !it->is_object()) {
        return nullptr;
    }
    return &*it;
}

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

bool isAttributeScalarShorthandMetadataField(std::string_view fieldName)
{
    return fieldName.starts_with("$") || fieldName == "_sourceData" || fieldName == "conditions" ||
        fieldName == "properties" || fieldName == "references" || fieldName == "validity";
}

bool isBitmaskKind(Kind kind)
{
    auto bits = simfil::SchemaType::affinities(kind);
    auto mask = simfil::valueTypeAffinity(simfil::ValueType::Bitmask);
    return (bits & mask) && !(bits & ~(mask | simfil::valueTypeAffinity(simfil::ValueType::Null)));
}

std::optional<Kind> jsonKind(nlohmann::json const* metadata, simfil::StringPool const& strings)
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
        static constexpr simfil::ValueType LEGACY_TYPES[]{
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
        for (size_t i = 0; i < std::size(LEGACY_TYPES); ++i)
            if (legacyBits & (1u << (i + 1)))
                bits |= simfil::valueTypeAffinity(LEGACY_TYPES[i]);
        for (auto [legacyName, kind] : LEGACY_JSON_KINDS)
            if (legacyName == packed >> 16)
                return simfil::SchemaType::makeKind(simfil::SchemaType::kindNameId(kind), bits);
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
            JSON_AFFINITIES,
            affinity.get_ref<std::string const&>(),
            &std::pair<simfil::ValueType, std::string_view>::second);
        if (type == std::end(JSON_AFFINITIES))
            throw std::invalid_argument("Unknown schema affinity: " + affinity.get<std::string>());
        auto bit = simfil::valueTypeAffinity(type->first);
        if (bits & bit)
            throw std::invalid_argument("Duplicate schema affinity.");
        bits |= bit;
    }
    auto name = strings.get(value["name"].get_ref<std::string const&>());
    if (!name || name >= simfil::StringPool::FirstDynamicId)
        throw std::invalid_argument("Unknown schema kind: " + value["name"].get<std::string>());
    return simfil::SchemaType::makeKind(name, bits);
}

nlohmann::json exportKind(simfil::SchemaType const& type)
{
    auto kind = type.kind();
    auto affinities = nlohmann::json::array();
    auto bits = simfil::SchemaType::affinities(kind);
    for (auto [type, name] : JSON_AFFINITIES)
        if (bits & simfil::valueTypeAffinity(type)) {
            affinities.push_back(name);
            bits &= ~simfil::valueTypeAffinity(type);
        }
    if (bits)
        throw std::invalid_argument("Unsupported schema affinities for JSON projection.");
    auto name = type.pool()->strings()->resolve(simfil::SchemaType::kindNameId(kind));
    if (!name)
        throw std::invalid_argument("Schema kind has no registered name.");
    return {{"name", *name}, {"affinities", std::move(affinities)}};
}

void validateJsonSchema(nlohmann::json const& root)
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

nlohmann::json jsonSchemaAnnotations(nlohmann::json const& schema)
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
              "typeId",
              "kind",
              "schemaKey",
              "metaType",
              "typename",
              "zserioType",
              "attributeType",
              "attributeTypeCode",
              "requiredFields",
              "contextFields",
              "nullable",
              "keys",
              "attributeOwners",
              "enumBytes",
              "bitmaskValues",
              "domainDefinitions",
              "typeDefinitions",
              "edgeAlternatives",
              "unknownEdge",
              "unknownTarget",
              "fieldAliases",
              "baseType",
              "directFields",
              "nullableProjection"})
        {
            extra.erase(key);
        }
        if (!extra.empty()) {
            result["x-mapget"] = std::move(extra);
        }
    }
    return result;
}
}  // namespace mapget::schema_detail

namespace mapget
{
namespace
{
using Json = nlohmann::json;
using schema_detail::mapgetMetadata;
using schema_detail::metadataString;
using schema_detail::pointerToken;

/** Test explicit JSON type membership without treating validation keywords as types. */
bool hasJsonType(Json const& schema, std::string_view type)
{
    auto found = schema.find("type");
    if (found == schema.end())
        return false;
    if (found->is_string())
        return found->get_ref<std::string const&>() == type;
    return found->is_array() &&
        std::ranges::any_of(
               *found,
               [&](auto const& value) {
                   return value.is_string() && value.template get_ref<std::string const&>() == type;
               });
}

/** JSON properties/items imply structure only in the absence of an explicit type. */
bool hasJsonStructure(Json const& schema, std::string_view type, std::string_view keyword)
{
    return schema.contains("type") ? hasJsonType(schema, type) : schema.contains(keyword);
}

/** References are strictly local to the supplied document, never fetched over the network. */
std::string referencePointer(std::string const& reference)
{
    if (reference.empty() || reference.front() != '#')
        throw std::invalid_argument("LayerSchema only supports local JSON Schema references.");
    return reference.size() == 1 ? "#" : reference.substr(1);
}

/** Preserve conjunction between enum and const instead of concatenating their literals. */
Json scalarLiterals(Json const& schema)
{
    Json values = schema.value("enum", Json::array());
    if (!values.is_array())
        throw std::invalid_argument("Schema enum must be an array.");
    if (auto constant = schema.find("const"); constant != schema.end()) {
        if (!schema.contains("enum") || std::ranges::find(values, *constant) != values.end())
            values = Json::array({*constant});
        else
            values = Json::array();
    }
    return values;
}

/** Infer affinity without converting compound constants into fictitious scalar values. */
Kind scalarKind(Json const& schema)
{
    Kind kind = hasJsonType(schema, "string") ? Kind::String :
        hasJsonType(schema, "integer")        ? Kind::Int :
        hasJsonType(schema, "number")         ? Kind::Float :
        hasJsonType(schema, "boolean")        ? Kind::Bool :
        hasJsonType(schema, "null")           ? Kind::Null :
                                                Kind::Unknown;
    if (kind == Kind::Float)
        kind = simfil::SchemaType::makeKind(
            simfil::SchemaType::kindNameId(kind),
            simfil::valueTypeAffinity(simfil::ValueType::Float) |
                simfil::valueTypeAffinity(simfil::ValueType::Int));
    auto values = scalarLiterals(schema);
    if (schema.contains("const") && schema.contains("enum") && values.empty())
        return Kind::Never;
    uint16_t affinities = 0;
    for (auto const& value : values) {
        if (value.is_string())
            affinities |= simfil::valueTypeAffinity(simfil::ValueType::String);
        else if (value.is_boolean())
            affinities |= simfil::valueTypeAffinity(simfil::ValueType::Bool);
        else if (value.is_number_integer())
            affinities |= simfil::valueTypeAffinity(simfil::ValueType::Int);
        else if (value.is_number_float())
            affinities |= simfil::valueTypeAffinity(simfil::ValueType::Float);
        else if (value.is_null())
            affinities |= simfil::valueTypeAffinity(simfil::ValueType::Null);
        else
            affinities |= simfil::SchemaType::AllAffinities;
    }
    if (kind != Kind::Unknown || !affinities ||
        (affinities &
         (simfil::valueTypeAffinity(simfil::ValueType::Object) |
          simfil::valueTypeAffinity(simfil::ValueType::Array))))
        return kind;
    kind = Kind::Value;
    for (auto scalar : {Kind::String, Kind::Int, Kind::Float, Kind::Bool, Kind::Null})
        if (simfil::SchemaType::affinities(scalar) == affinities)
            kind = scalar;
    return simfil::SchemaType::makeKind(simfil::SchemaType::kindNameId(kind), affinities);
}
}  // namespace

/** Compile boundary JSON directly into an unpublished canonical pool.
 * Only traversal state and external-reference reuse IDs are temporary. There is
 * no second object/field graph to pack, reinterpret, or retain after import. */
class MapSchema::Impl::JsonCompiler
{
public:
    /** Borrow the input and optional destination; the caller owns the staging pool. */
    JsonCompiler(
        MapSchema& staged,
        Json const& input,
        LayerSchema::Ptr layer = {},
        MapSchema const* existing = nullptr,
        size_t existingLayer = GlobalLayer)
        : staged_(staged),
          graph_(*staged.schema_),
          input_(input),
          layer_(std::move(layer)),
          existing_(existing),
          existingLayer_(existingLayer)
    {
        if (existing_) {
            // Registered application kinds belong to the owner's static dictionary, not JSON.
            for (simfil::StringId id = 1; id < simfil::StringPool::FirstDynamicId; ++id) {
                auto name = existing_->strings()->resolve(id);
                if (!name)
                    continue;
                auto current = staged_.strings()->resolve(id);
                if (current && current != name)
                    throw std::invalid_argument("Schema kind vocabularies disagree.");
                if (!current)
                    staged_.strings()->addStaticKey(id, std::string(*name));
            }
        }
    }

    /** Preallocate transported declarations by source ID, then populate canonical edges. */
    TypeId compile(bool fragment = false)
    {
        schema_detail::validateJsonSchema(input_);
        fragment_ = fragment;
        if (fragment)
            pointerPrefix_ = "#/import/" +
                std::to_string(existing_ ? existing_->typeCount() + 1 : 1);
        auto metadata = mapgetMetadata(input_);
        if (!fragment && metadata &&
            metadata->value("typeDefinitions", metadata->value("domainDefinitions", false)))
        {
            auto keyword = input_.contains("definitions") ? "definitions" : "$defs";
            auto const& definitions = input_.at(keyword);
            std::map<uint64_t, std::pair<std::string, Json const*>> ordered;
            for (auto const& [name, definition] : definitions.items()) {
                auto const& meta = definition.at("x-mapget");
                auto const& value = meta.at(meta.contains("typeId") ? "typeId" : "schemaId");
                if (!value.is_number_integer() ||
                    (!value.is_number_unsigned() && value.get<int64_t>() < 0))
                    throw std::invalid_argument("Invalid transported schema ID.");
                auto id = value.get<uint64_t>();
                auto pointer = "/" + std::string(keyword) + "/" + pointerToken(name);
                if (!id || id > simfil::MaxTypeId ||
                    !ordered.emplace(id, std::pair{pointer, &definition}).second)
                    throw std::invalid_argument("Invalid or duplicate transported schema ID.");
            }
            canonical_ = true;
            for (auto const& [_, declaration] : ordered) {
                auto const& [pointer, definition] = declaration;
                auto kind = declaredKind(*definition);
                auto id = staged_.addSchema(
                    kind,
                    {},
                    metadataString(mapgetMetadata(*definition), "metaType"),
                    pointer);
                transported_.emplace(pointer, id);
            }
            for (auto const& [_, declaration] : ordered)
                build(*declaration.second, declaration.first, {});
        }
        return build(input_, fragment ? pointerPrefix_ : "#", {});
    }

    /** Map staging placeholders to exact destination IDs without copying existing graphs. */
    std::span<TypeId const> reuse()
    {
        reuse_.resize(staged_.typeCount() + 1);
        return reuse_;
    }

private:
    /** Context qualifies use-site aliases; it never owns structural type declarations. */
    struct Context
    {
        std::string feature, layer, attribute;
    };

    /** Inspect the complete branch before allocation; canonical kinds cannot change afterward. */
    Kind declaredKind(Json const& input) const
    {
        if (!input.is_object())
            return input.is_boolean() ?
                (input.get<bool>() ? Kind::Any : Kind::Never) :
                Kind::Unknown;
        auto metadata = mapgetMetadata(input);
        if (auto kind = schema_detail::jsonKind(metadata, *staged_.strings()))
            return *kind;
        size_t terms = input.contains("$ref") + input.contains("anyOf") + input.contains("oneOf") +
            input.contains("allOf");
        bool base = input.contains("type") || input.contains("properties") ||
            input.contains("items") || input.contains("enum") || input.contains("const");
        Kind kind;
        if (terms && (terms != 1 || base || !input.contains("$ref")))
            kind = terms > 1 || base || input.contains("allOf") ?
                Kind::Intersection :
                input.contains("oneOf") ?
                Kind::OneOf :
                Kind::Union;
        else if (auto types = input.find("type"); types != input.end() && types->is_array())
            kind = Kind::Union;
        else if (hasJsonStructure(input, "object", "properties"))
            kind = Kind::Object;
        else if (hasJsonStructure(input, "array", "items"))
            kind = Kind::Array;
        else
            kind = scalarKind(input);
        auto name = metadataString(metadata, "metaType");
        if (name == "Attribute")
            kind = simfil::SchemaType::makeKind(
                simfil::SchemaType::kindNameId(LayerSchema::AttributeKind),
                simfil::SchemaType::affinities(kind));
        else if (name == "Feature" && kind == Kind::Object)
            kind = LayerSchema::FeatureKind;
        return kind;
    }

    /** Length-prefix context components so punctuation cannot alias unrelated use sites. */
    static std::string memoKey(std::string_view pointer, Context const& context)
    {
        std::string key(pointer);
        for (auto const& value : {context.feature, context.layer, context.attribute})
            key += "|" + std::to_string(value.size()) + ":" + value;
        return key;
    }

    /** Derive public lookup keys separately from the physical JSON reference. */
    static std::string
    schemaKey(Json const& schema, std::string const& pointer, Context const& context)
    {
        auto metadata = mapgetMetadata(schema);
        if (auto key = metadataString(metadata, "schemaKey"); !key.empty())
            return key;
        auto metaType = metadataString(metadata, "metaType");
        auto feature = metadataString(metadata, "featureType");
        if (feature.empty())
            feature = context.feature;
        if (metaType == "Feature" && !feature.empty())
            return LayerSchema::featureKey(feature);
        if (metaType == "FeatureProperties" && !feature.empty())
            return LayerSchema::featurePropertiesKey(feature);
        if (metaType == "AttributeLayerMap" && !feature.empty())
            return LayerSchema::attributeLayerMapKey(feature);
        if (metaType == "AttributeContainer" && !feature.empty() && !context.layer.empty())
            return "AttributeContainer:" + feature + ":" + context.layer;
        if (metaType == "Attribute" && !feature.empty() && !context.layer.empty()) {
            auto code = metadataString(metadata, "attributeTypeCode");
            if (!code.empty())
                return LayerSchema::attributeKey(feature, context.layer, code);
        }
        return pointer;
    }

    /** Derived keys retain their first physical binding; explicit aliases must agree exactly. */
    void alias(std::string const& key, TypeId id, bool strict = false)
    {
        if (key.empty() || !id)
            return;
        auto existing = layer_ ? layer_->schemaId(key) : staged_.findSchema(key);
        if (existing) {
            if (strict && existing != id)
                throw std::invalid_argument("Conflicting explicit schema alias: " + key);
            return;
        }
        if (layer_)
            layer_->registerSchemaKey(key, id);
        else
            staged_.registerSchemaKey(key, id);
    }

    /** Reuse one preallocated transport node or allocate an ordinary canonical declaration. */
    TypeId allocate(Json const& schema, std::string const& key, std::string const& pointer)
    {
        auto metaType = metadataString(mapgetMetadata(schema), "metaType");
        auto found = transported_.find(pointer);
        auto id = found != transported_.end() ?
            found->second :
            staged_.addSchema(declaredKind(schema), {}, metaType, {});
        auto& data = graph_.data(id);
        data.key = graph_.text(key);
        data.pointer = graph_.text(pointer);
        data.metaType = graph_.text(metaType);
        if (!canonical_) {
            alias(key, id);
            alias(pointer, id);
        }
        return id;
    }

    TypeId build(Json const& schema, std::string pointer, Context context);
    std::vector<TypeId>
    edges(Json const& schema, std::string const& pointer, Context const& context);
    TypeId contextualAttribute(TypeId source, Context const& context);
    void
    metadata(TypeId id, Json const& schema, Context const& context, std::string const& pointer);
    TypeId externalReference(std::string const& key);

    MapSchema& staged_;
    Impl& graph_;
    Json const& input_;
    LayerSchema::Ptr layer_;
    MapSchema const* existing_;
    size_t existingLayer_;
    bool fragment_ = false, canonical_ = false;
    std::string pointerPrefix_;
    std::map<std::string, TypeId> transported_, memo_, external_;
    std::set<TypeId> entered_;
    std::set<std::string> activeReferences_;
    std::vector<TypeId> reuse_;
    size_t depth_ = 0, work_ = 0;
};

/** Resolve exact existing keys through a placeholder, leaving destination types untouched. */
TypeId MapSchema::Impl::JsonCompiler::externalReference(std::string const& key)
{
    if (!existing_)
        return 0;
    if (auto found = external_.find(key); found != external_.end())
        return found->second;
    TypeId destination = 0;
    if (existingLayer_ != GlobalLayer)
        destination = existing_->schema_->findAlias(existingLayer_, key);
    if (!destination)
        destination = existing_->schema_->findAlias(GlobalLayer, key);
    if (!destination)
        return 0;
    auto id = staged_.addSchema(Kind::Unknown);
    reuse_.resize(staged_.typeCount() + 1);
    reuse_[id] = destination;
    external_.emplace(key, id);
    return id;
}

/** Restore plural member edges without allocating an accidental union type. */
std::vector<TypeId> MapSchema::Impl::JsonCompiler::edges(
    Json const& schema,
    std::string const& pointer,
    Context const& context)
{
    if (schema.is_object() && schema.value("x-mapget-multimap", false)) {
        for (auto key : {"oneOf", "anyOf", "allOf"}) {
            if (auto branches = schema.find(key); branches != schema.end() && branches->is_array())
                return edges(branches->at(0), pointer + "/" + key + "/0", context);
        }
        throw std::invalid_argument("Malformed multimap schema projection.");
    }
    auto metadata = mapgetMetadata(schema);
    if (metadata && metadata->value("unknownEdge", false))
        return metadata->value("unknownTarget", false) ?
            std::vector<TypeId>{simfil::NoTypeId} :
            std::vector<TypeId>{};
    if (metadata && metadata->value("edgeAlternatives", false)) {
        std::vector<TypeId> result;
        auto const& branches = schema.at("anyOf");
        for (size_t i = 0; i < branches.size(); ++i) {
            auto meta = mapgetMetadata(branches[i]);
            result.push_back(
                meta && meta->value("unknownEdge", false) ?
                    simfil::NoTypeId :
                    build(branches[i], pointer + "/anyOf/" + std::to_string(i), context));
        }
        return result;
    }
    return {build(schema, pointer, context)};
}

/** Reserve each node before descending so recursive references keep their exact sharing. */
TypeId
MapSchema::Impl::JsonCompiler::build(Json const& schema, std::string pointer, Context context)
{
    if (depth_ >= 256 || ++work_ > 4 * 1024 * 1024)
        throw std::length_error("JSON schema import traversal budget exceeded.");
    ++depth_;
    auto leave = [](size_t* depth)
    {
        --*depth;
    };
    std::unique_ptr<size_t, decltype(leave)> depthGuard(&depth_, leave);
    auto meta = mapgetMetadata(schema);
    if (meta && meta->value("nullableProjection", false)) {
        auto unwrapped = schema.at("anyOf").at(0);
        unwrapped["x-mapget"] = *meta;
        unwrapped["x-mapget"].erase("nullableProjection");
        return build(unwrapped, std::move(pointer), std::move(context));
    }
    auto metaType = metadataString(meta, "metaType");
    if (auto feature = metadataString(meta, "featureType"); !feature.empty())
        context.feature = feature;
    if (metaType == "Attribute")
        if (auto name = metadataString(meta, "attributeTypeCode"); !name.empty())
            context.attribute = name;
    auto key = canonical_ ? metadataString(meta, "schemaKey") : schemaKey(schema, pointer, context);
    auto memo = memoKey(pointer, context);
    if (auto reserved = transported_.find(pointer);
        reserved != transported_.end() && entered_.contains(reserved->second))
        return reserved->second;
    if (auto found = memo_.find(memo); found != memo_.end()) {
        alias(key, found->second);
        return found->second;
    }
    if (!schema.is_object()) {
        auto id = allocate(schema, key, pointer);
        memo_[memo] = id;
        entered_.insert(id);
        return id;
    }
    if (schema.value("x-mapget-multimap", false)) {
        auto values = edges(schema, pointer, context);
        if (values.size() != 1)
            throw std::invalid_argument("A root multimap must have one logical value type.");
        memo_[memo] = values.front();
        alias(key, values.front());
        return values.front();
    }

    Json constraints = schema;
    constraints.erase("x-mapget");
    constraints.erase("definitions");
    constraints.erase("$defs");
    std::vector<std::pair<std::string, Json>> terms;
    for (auto name : {"$ref", "anyOf", "oneOf", "allOf"}) {
        if (auto term = schema.find(name); term != schema.end()) {
            terms.emplace_back(name, *term);
            constraints.erase(name);
        }
    }
    bool base = hasJsonStructure(constraints, "object", "properties") ||
        hasJsonStructure(constraints, "array", "items") || constraints.contains("const") ||
        constraints.contains("enum") || hasJsonType(constraints, "null") ||
        hasJsonType(constraints, "boolean") || hasJsonType(constraints, "integer") ||
        hasJsonType(constraints, "number") || hasJsonType(constraints, "string");

    if (terms.size() == 1 && !base && terms.front().first == "$ref") {
        auto reference = terms.front().second.get<std::string>();
        if (fragment_) {
            auto id = layer_ ? layer_->schemaId(reference) : staged_.findSchema(reference);
            if (!id)
                id = externalReference(reference);
            if (id)
                return contextualAttribute(id, context);
        }
        if (!activeReferences_.insert(memo).second) {
            auto id = allocate(Json{}, key, pointer);
            memo_[memo] = id;
            return id;
        }
        auto physical = context;
        physical.attribute.clear();
        auto target = referencePointer(reference);
        auto const& input = target == "#" ? input_ : input_.at(Json::json_pointer(target));
        auto id = build(input, pointerPrefix_ + target, physical);
        id = contextualAttribute(id, context);
        activeReferences_.erase(memo);
        memo_[memo] = id;
        alias(key, id);
        return id;
    }

    auto id = allocate(schema, key, pointer);
    auto type = staged_.schemaType(id);
    memo_[memo] = id;
    entered_.insert(id);
    if (!terms.empty()) {
        bool single = terms.size() == 1 && !base;
        auto operation = single ? terms.front().first : std::string("allOf");
        type.setComposition(
            operation == "allOf"     ? Composition::AllOf :
                operation == "oneOf" ? Composition::OneOf :
                                       Composition::AnyOf);
        if (single) {
            auto const& branches = terms.front().second;
            if (!branches.is_array())
                throw std::invalid_argument("LayerSchema combiner must be an array.");
            for (size_t i = 0; i < branches.size(); ++i)
                type.asComposition().addAlternative(staged_.schemaType(build(
                    branches[i],
                    pointer + "/" + operation + "/" + std::to_string(i),
                    context)));
        }
        else {
            for (auto const& [name, term] : terms)
                type.asComposition().addAlternative(
                    staged_.schemaType(build(Json{{name, term}}, pointer + "/@" + name, context)));
            if (base)
                type.asComposition().addAlternative(
                    staged_.schemaType(build(constraints, pointer + "/@base", context)));
        }
    }
    else if (auto types = schema.find("type"); types != schema.end() && types->is_array() &&
             !schema_detail::jsonKind(meta, *staged_.strings()))
    {
        type.setComposition(Composition::AnyOf);
        for (size_t index = 0; index < types->size(); ++index) {
            auto branch = schema;
            branch["type"] = types->at(index);
            branch.erase("x-mapget");
            type.asComposition().addAlternative(staged_.schemaType(
                build(branch, pointer + "/type/" + std::to_string(index), context)));
        }
    }
    else {
        bool taggedScalar = schema_detail::isBitmaskKind(type.kind()) ||
            simfil::SchemaType::kindNameId(type.kind()) ==
                simfil::SchemaType::kindNameId(Kind::Bytes);
        bool object = !taggedScalar && hasJsonStructure(schema, "object", "properties");
        bool array = !taggedScalar && hasJsonStructure(schema, "array", "items");
        if (object || (array && schema_detail::jsonKind(meta, *staged_.strings()))) {
            if (auto properties = schema.find("properties");
                properties != schema.end() && properties->is_object())
            {
                auto addField = [&](std::string const& name, Json const& child)
                {
                    if (canonical_ && name == "_multimap" && child == Json{{"const", true}})
                        return;
                    auto childContext = context;
                    if (metaType == "AttributeLayerMap")
                        childContext.layer = name;
                    if (metaType == "AttributeContainer")
                        childContext.attribute = name;
                    else if (metaType == "Attribute")
                        childContext.attribute.clear();
                    auto children =
                        edges(child, pointer + "/properties/" + pointerToken(name), childContext);
                    // Empty declarations differ from a single unknown target.
                    auto field = array ?
                        type.asArray().declareField(name) :
                        type.asObject().declareField(name);
                    field.setMultimap(child.is_object() && child.value("x-mapget-multimap", false));
                    for (auto target : children)
                        field.addType(staged_.schemaType(target));
                };
                if (canonical_ && meta && meta->contains("directFields")) {
                    // JSON properties are sorted, but native declaration order is not.
                    // Other projected properties belong to the base and must not be copied.
                    auto const& direct = meta->at("directFields");
                    if (!direct.is_array())
                        throw std::invalid_argument("Canonical directFields must be an array.");
                    std::set<std::string_view> seen;
                    for (auto const& field : direct) {
                        if (!field.is_string())
                            throw std::invalid_argument(
                                "Canonical directFields must contain names.");
                        auto const& name = field.get_ref<std::string const&>();
                        auto child = properties->find(name);
                        if (child == properties->end() || !seen.insert(name).second)
                            throw std::invalid_argument(
                                "Missing or duplicate canonical direct field: " + name);
                        addField(name, *child);
                    }
                }
                else
                    for (auto const& [name, child] : properties->items())
                        addField(name, child);
                if (!canonical_ && object && metaType == "Feature" &&
                    properties->contains("properties") && !properties->contains("attributes"))
                    type.asObject().addAlias("attributes", "properties");
            }
        }
        if (array) {
            if (auto items = schema.find("items"); items != schema.end())
                for (auto child : edges(*items, pointer + "/items", context))
                    type.asArray().addElement(staged_.schemaType(child));
        }
        else if (!object) {
            for (auto const& literal : scalarLiterals(schema)) {
                if (literal.is_string())
                    type.asEnum().addSymbol(literal.get<std::string>());
                else if (literal.is_boolean())
                    type.asEnum().addValue(literal.get<bool>());
                else if (literal.is_number_integer()) {
                    if (literal.is_number_unsigned() &&
                        literal.get<uint64_t>() > uint64_t(INT64_MAX))
                        throw std::invalid_argument(
                            "Schema integer literal exceeds the query value domain.");
                    type.asEnum().addValue(literal.get<int64_t>());
                }
                else if (literal.is_number_float())
                    type.asEnum().addValue(literal.get<double>());
                else if (literal.is_null())
                    type.asEnum().addValue(std::monostate{});
                // Compound literals remain validation annotations, not flattened model types.
            }
        }
    }
    metadata(id, schema, context, pointer);
    return id;
}

/** Qualify only an Attribute root; its shared payload remains in the same type graph. */
TypeId MapSchema::Impl::JsonCompiler::contextualAttribute(TypeId source, Context const& context)
{
    bool external = source < reuse_.size() && reuse_[source];
    auto const& owner = external ? *existing_ : staged_;
    auto physical = external ? reuse_[source] : source;
    auto const& sourceGraph = *owner.schema_;
    auto row = sourceGraph.data(physical);
    if (canonical_ || sourceGraph.text(row.metaType) != "Attribute" ||
        !sourceGraph.text(row.attributeCode).empty() || context.feature.empty() ||
        context.layer.empty() || context.attribute.empty())
        return source;
    auto pointer = sourceGraph.text(row.pointer);
    if (pointer.starts_with(pointerPrefix_))
        pointer.remove_prefix(pointerPrefix_.size());
    auto key = LayerSchema::attributeKey(
        context.feature,
        context.layer,
        context.attribute,
        pointer.empty() ? "#" : pointer);
    if (auto found = layer_ ? layer_->schemaId(key) : staged_.findSchema(key))
        return found;

    TypeId id;
    if (external) {
        // A one-branch named view preserves a foreign root without copying its graph.
        auto kind = simfil::SchemaType::makeKind(
            simfil::SchemaType::kindNameId(LayerSchema::AttributeKind),
            simfil::SchemaType::affinities(Kind::Union));
        id = staged_.addSchema(kind, {}, "Attribute");
        staged_.schemaType(id).setComposition(Composition::AnyOf);
        staged_.schemaType(id).asComposition().addAlternative(staged_.schemaType(source));
        staged_.schemaType(id).setTypeName(std::string(owner.schemaType(physical).typeName()));
        staged_.setAttributeType(id, std::string(sourceGraph.text(row.attributeType)));
        staged_.setZserioType(id, std::string(sourceGraph.text(row.zserioType)));
    }
    else {
        id = staged_.cloneType(staged_.schemaType(source)).id();
        // All metadata addresses are already owned here. The use-site inventory is not shared.
        row.owners = 0;
        graph_.data(id) = row;
    }
    graph_.data(id).key = graph_.text(key);
    staged_.setAttributeMetadata(
        id,
        {context.feature, context.layer, context.attribute, id},
        std::string(sourceGraph.text(row.attributeType)),
        std::string(sourceGraph.text(row.zserioType)));
    alias(key, id, true);
    return id;
}

/** Attach source metadata and validation references to the already allocated canonical node. */
void MapSchema::Impl::JsonCompiler::metadata(
    TypeId id,
    Json const& schema,
    Context const& context,
    std::string const& pointer)
{
    auto meta = mapgetMetadata(schema);
    auto metaType = metadataString(meta, "metaType");
    auto zserio = metadataString(meta, "zserioType");
    auto attributeType = metadataString(meta, "attributeType");
    auto attributeCode = metadataString(meta, "attributeTypeCode");
    auto typeName = metadataString(meta, "typename");
    if (typeName.empty())
        typeName = zserio;
    if (typeName.empty())
        typeName = metadataString(meta, "sourceType");
    if (typeName.empty() && metaType == "Feature")
        typeName = context.feature;
    if (typeName.empty() && metaType == "Attribute")
        typeName = attributeType;
    auto type = staged_.schemaType(id);
    type.setTypeName(typeName);
    graph_.data(id).zserioType = graph_.text(zserio);
    graph_.data(id).attributeType = graph_.text(attributeType);
    if (schema.contains("type"))
        type.setNullable(hasJsonType(schema, "null"));
    if (auto value = schema.find("nullable"); value != schema.end() && value->is_boolean())
        type.setNullable(value->get<bool>());
    if (type.hasAffinity(simfil::ValueType::Object) && type.composition() == Composition::None) {
        auto additional = schema.find("additionalProperties");
        bool open = additional == schema.end() || !additional->is_boolean() ||
            additional->get<bool>();
        if (auto patterns = schema.find("patternProperties");
            patterns != schema.end() && !patterns->empty())
            open = true;
        type.asObject().setOpen(open);
        if (auto properties = schema.find("properties");
            properties != schema.end() && properties->is_object())
        {
            auto required = schema.find("required");
            for (auto const& [name, _] : properties->items())
                if (type.field(name))
                    type.field(name).setRequired(
                        required != schema.end() && required->is_array() &&
                        std::find(required->begin(), required->end(), name) != required->end());
        }
    }
    if (meta) {
        if (auto value = meta->find("nullable"); value != meta->end())
            type.setNullable(
                value->is_null() ? std::nullopt : std::optional<bool>(value->get<bool>()));
        if (auto fields = meta->find("requiredFields"); fields != meta->end()) {
            // Canonical omission means unknown presence, not JSON's default optional field.
            type.forEachDirectField(
                [](auto const& field)
                {
                    auto writable = field;
                    writable.setRequired(std::nullopt);
                });
            for (auto const& [name, value] : fields->items()) {
                auto field = type.hasAffinity(simfil::ValueType::Object) ?
                    type.asObject().declareField(name) :
                    type.asArray().declareField(name);
                field.setRequired(
                    value.is_null() ? std::nullopt : std::optional<bool>(value.get<bool>()));
            }
        }
        if (auto fields = meta->find("contextFields"); fields != meta->end())
            for (auto const& name : *fields) {
                auto spelling = name.get<std::string>();
                auto field = type.hasAffinity(simfil::ValueType::Object) ?
                    type.asObject().declareField(spelling) :
                    type.asArray().declareField(spelling);
                field.setRole(simfil::SchemaType::FieldRole::Context);
            }
        if (auto keys = meta->find("keys"); keys != meta->end())
            for (auto const& key : *keys)
                alias(key.get<std::string>(), id, true);
        else if (auto key = metadataString(meta, "schemaKey"); !key.empty())
            alias(key, id);
        if (auto names = meta->find("bitmaskValues"); names != meta->end())
            for (auto const& name : *names)
                type.asEnum().addSymbol(name.get<std::string>());
        if (auto bytes = meta->find("enumBytes"); bytes != meta->end())
            for (auto const& hex : *bytes) {
                auto value = simfil::ByteArray::fromHex(hex.get<std::string>());
                if (!value)
                    throw std::invalid_argument("Invalid schema byte literal.");
                type.asEnum().addValue(std::move(*value));
            }
        if (auto owners = meta->find("attributeOwners"); owners != meta->end())
            for (auto const& owner : *owners)
                staged_.setAttributeMetadata(
                    id,
                    {owner.at("featureType"),
                     owner.at("layer"),
                     owner.at("name"),
                     id,
                     owner.value("featureLayer", std::string{})},
                    attributeType,
                    zserio);
        if (auto aliases = meta->find("fieldAliases"); aliases != meta->end())
            for (auto const& [name, target] : aliases->items())
                type.asObject().addAlias(name, target.get<std::string>());
        if (auto base = meta->find("baseType"); base != meta->end()) {
            auto target = build(*base, pointer + "/x-mapget/baseType", context);
            type.asObject().setBase(staged_.schemaType(target).asObject());
        }
    }
    if (metaType == "Attribute") {
        if (!context.feature.empty() && !context.layer.empty() && !attributeCode.empty() &&
            !(meta && meta->contains("keys")))
        {
            auto local = std::string_view(pointer);
            if (local.starts_with(pointerPrefix_))
                local.remove_prefix(pointerPrefix_.size());
            alias(
                LayerSchema::attributeKey(
                    context.feature,
                    context.layer,
                    attributeCode,
                    local.empty() ? "#" : local),
                id,
                true);
        }
        if (!meta || !meta->contains("attributeOwners"))
            staged_.setAttributeMetadata(
                id,
                {context.feature, context.layer, attributeCode, id},
                attributeType,
                zserio);
    }
    // An explicit empty discriminator is meaningful for a physical type with several names.
    graph_.data(id).attributeCode = graph_.text(attributeCode);
    auto extra = schema_detail::jsonSchemaAnnotations(schema);
    if (!schema.contains("type") && (schema.contains("properties") || schema.contains("items")))
        extra["x-mapget"]["jsonTypeImplicit"] = true;
    if (type.kind() == Kind::Never && extra.contains("not") && extra["not"] == Json::object())
        extra.erase("not");
    schema_detail::forEachValidationSchema(
        extra,
        [&](auto& constraint, auto const& suffix)
        {
            auto child = build(constraint, pointer + suffix, context);
            constraint = {{"$ref", "#/definitions/" + std::to_string(child)}};
        });
    graph_.compileScalarMetadata(id, extra);
}

/** Compile one standalone layer before touching the destination's type or alias namespace. */
LayerSchema::Ptr MapSchema::importLayer(
    std::string_view name,
    LayerType type,
    Json const& json,
    std::span<FeatureTypeInfo const> features)
{
    requireMutable();
    schema_detail::validateJsonSchema(json);
    auto staged = std::make_shared<MapSchema>(format());
    auto local = staged->newLayer(name, type);
    auto inputs = Json::array();
    for (auto const& info : features) {
        // This also detects conflicts within the supplied list, before destination mutation.
        staged->addFeatureType(info);
        if (auto existing = getTypeInfo(info.name_, false);
            existing && !existing.sameIdentity(info))
            throw std::invalid_argument("Conflicting map-wide feature identity: " + info.name_);
        inputs.push_back(info.toJson());
    }
    auto fingerprint = picosha2::hash256_hex_string(json.dump() + inputs.dump());
    if (auto found = layer(name)) {
        auto const& old = schema_->layerAt(found->addr().index());
        if (old.type == uint8_t(type) &&
            schema_->layerBuild.at(found->addr().index()).fingerprint == fingerprint)
            return found;
        throw std::invalid_argument(
            "Conflicting declarations for schema layer '" + std::string(name) + "'.");
    }
    TypeId root = json.is_null() ? 0 : Impl::JsonCompiler(*staged, json, local, this).compile();
    local->setRootSchema(root);
    auto emitted = staged->schema_->producedRoots(staged->schema_->layerAt(0));
    for (auto const& info : features) {
        auto feature = local->schemaId(LayerSchema::featureKey(info.name_));
        bool produced = emitted.empty() || std::ranges::find(emitted, feature) != emitted.end();
        if (feature) {
            auto annotations =
                staged->schema_->annotation(staged->schema_->data(feature).annotations);
            auto meta = mapgetMetadata(annotations);
            if (meta && meta->contains("produced"))
                produced = produced && meta->at("produced").get<bool>();
        }
        if (produced)
            local->addFeatureType(info, feature);
    }
    if (!root) {
        root = local->addSchema(Kind::Object);
        staged->schemaType(root).asObject().setOpen(true);
        local->setRootSchema(root);
    }
    // Only native generic storage is sealed here; query overlays are derived once on publication.
    staged->simfil::SchemaPool::finalize();
    LayerSchema::Ptr result;
    appendExtension(
        [&]
        {
            schema_->appendMetadata(
                [&]
                {
                    auto added = newLayer(name, type);
                    auto index = added->addr().index();
                    // Native import remaps produced memberships inside the same transaction.
                    auto imported = schema_->import(*staged, index, root);
                    schema_->layerBuild.at(index).fingerprint = std::move(fingerprint);
                    schema_->layerAt(index).root = imported;
                    result = std::move(added);
                });
        });
    return result;
}

/** Append a JSON fragment using placeholders for previously declared canonical types. */
TypeId MapSchema::addJsonSchema(Json const& json)
{
    requireMutable();
    auto staged = std::make_shared<MapSchema>(format());
    Impl::JsonCompiler compiler(*staged, json, {}, this);
    auto root = compiler.compile(true);
    auto reuse = compiler.reuse();
    staged->simfil::SchemaPool::finalize();
    return schema_->import(*staged, Impl::GlobalLayer, root, reuse);
}

/** Resolve layer-local aliases first without copying them into a second logical graph. */
TypeId LayerSchema::addJsonSchema(Json const& json)
{
    auto& owner = mutableOwner();
    auto staged = std::make_shared<MapSchema>(owner.format());
    auto const& layer = owner.schema_->layerAt(addr_.index());
    auto local = staged->newLayer(owner.schema_->name(layer.name), LayerType(layer.type));
    MapSchema::Impl::JsonCompiler compiler(*staged, json, local, &owner, addr_.index());
    auto root = compiler.compile(true);
    auto reuse = compiler.reuse();
    staged->simfil::SchemaPool::finalize();
    return owner.schema_->import(*staged, addr_.index(), root, reuse);
}

/** Legacy JSON remains an input boundary; the returned layer owns only native canonical storage. */
LayerSchema::Ptr LayerSchema::fromJsonSchema(Json json)
{
    if (json.is_null())
        return {};
    auto owner = std::make_shared<MapSchema>();
    auto result = owner->importLayer("legacy", LayerType::Features, json);
    owner->finalize();
    return result;
}

/** Consume scalar semantics once, retaining only residual validation/descriptive CBOR. */
void MapSchema::Impl::compileScalarMetadata(
    TypeId id,
    Json& extra,
    std::function<void()> const& extension)
{
    auto type = pool.schemaType(id);
    auto kindName = simfil::SchemaType::kindNameId(type.kind());
    auto metadata = mapgetMetadata(extra);
    if (type.composition() == Composition::None &&
        (kindName == simfil::SchemaType::kindNameId(Kind::Int) ||
         kindName == simfil::SchemaType::kindNameId(Kind::Float)) &&
        (!metadata || !metadata->contains("numericDomain")))
    {
        auto inferred = Json::object();
        for (auto key : {"minimum", "maximum"})
            if (auto bound = extra.find(key); bound != extra.end() && bound->is_number())
                inferred[key] = *bound;
        if (!inferred.empty())
            extra["x-mapget"]["numericDomain"] = std::move(inferred);
    }
    // Borrowed scalar strings must survive removal of their JSON transport keys below.
    auto declarations = extra.value("x-mapget", Json::object());
    if (!declarations.is_object())
        throw std::invalid_argument("x-mapget must be an object.");
    auto numericValue = [](Json const& value) -> simfil::SchemaType::NumericValue
    {
        if (value.is_number_unsigned())
            return value.get<uint64_t>();
        if (value.is_number_integer())
            return value.get<int64_t>();
        if (value.is_number_float())
            return value.get<double>();
        throw std::invalid_argument("Numeric domain metadata must be numeric.");
    };
    std::optional<simfil::SchemaType::NumericDomain> numeric;
    std::vector<simfil::SchemaType::NumericRange> ranges;
    if (auto input = declarations.find("numericDomain"); input != declarations.end()) {
        if (!input->is_object())
            throw std::invalid_argument("numericDomain must be an object.");
        numeric.emplace();
        if (auto value = input->find("encoding"); value != input->end())
            numeric->encoding = value->get_ref<std::string const&>();
        if (auto value = input->find("unit"); value != input->end())
            numeric->unit = value->get_ref<std::string const&>();
        if (auto value = input->find("minimum"); value != input->end())
            numeric->minimum = numericValue(*value);
        if (auto value = input->find("maximum"); value != input->end())
            numeric->maximum = numericValue(*value);
        numeric->scale = input->value("scale", 1.);
        numeric->offset = input->value("offset", 0.);
        numeric->complete = input->value("complete", false);
        if (auto values = input->find("ordinaryRanges"); values != input->end()) {
            if (!values->is_array())
                throw std::invalid_argument("ordinaryRanges must be an array.");
            for (auto const& range : *values)
                ranges.push_back(
                    {numericValue(range.at("minimum")), numericValue(range.at("maximum"))});
        }
        numeric->ordinaryRanges = simfil::SchemaType::NumericRanges(std::span(ranges));
    }
    std::optional<uint8_t> width;
    if (auto value = declarations.find("bitWidth"); value != declarations.end()) {
        if (!type.isBitmask())
            throw std::invalid_argument("bitWidth requires a concrete raw-bitmask type.");
        if (!value->is_number_integer() ||
            (!value->is_number_unsigned() && value->get<int64_t>() < 1) ||
            value->get<uint64_t>() < 1 || value->get<uint64_t>() > 64)
            throw std::invalid_argument("Bitmask width must be 1..64.");
        width = uint8_t(value->get<uint64_t>());
    }
    std::vector<simfil::SchemaType::NamedValue> named;
    if (auto input = declarations.find("namedValues"); input != declarations.end()) {
        if (!input->is_array())
            throw std::invalid_argument("namedValues must be an array.");
        std::set<std::string_view> seen;
        for (auto const& value : *input) {
            simfil::SchemaType::NamedValue item;
            item.name = value.at("name").get_ref<std::string const&>();
            if (item.name.empty() || !seen.insert(item.name).second)
                throw std::invalid_argument("Duplicate or empty named-value name.");
            auto const& scalar = value.at("value");
            if (scalar.is_boolean())
                item.value = scalar.get<bool>();
            else if (scalar.is_string())
                item.value = std::string_view(scalar.get_ref<std::string const&>());
            else
                std::visit([&](auto number) { item.value = number; }, numericValue(scalar));
            static constexpr std::string_view ROLES[]{"constant", "special", "flag"};
            static constexpr std::string_view
                MEANINGS[]{"ordinary", "unknown", "variable", "unbounded", "saturated"};
            auto role = std::ranges::find(ROLES, value.value("role", "constant"));
            auto meaning = std::ranges::find(MEANINGS, value.value("meaning", "ordinary"));
            if (role == std::end(ROLES) || meaning == std::end(MEANINGS))
                throw std::invalid_argument("Invalid named-value role or meaning.");
            item.role = simfil::SchemaType::NamedValue::Role(role - std::begin(ROLES));
            item.meaning = simfil::SchemaType::NamedValue::Meaning(meaning - std::begin(MEANINGS));
            item.deprecated = value.value("deprecated", false);
            item.removed = value.value("removed", false);
            if (auto raw = value.find("rawValue"); raw != value.end()) {
                if (raw->is_number_unsigned())
                    item.rawValue = raw->get<uint64_t>();
                else if (raw->is_number_integer())
                    item.rawValue = raw->get<int64_t>();
                else
                    throw std::invalid_argument("Named-value rawValue must be an integer.");
            }
            if (auto previous = type.namedValue(item.name)) {
                if (previous->value != item.value || previous->role != item.role ||
                    previous->meaning != item.meaning || previous->deprecated != item.deprecated ||
                    previous->removed != item.removed || previous->rawValue != item.rawValue)
                    throw std::invalid_argument("Conflicting named-value declaration.");
                continue;  // Reapplying equal boundary metadata is idempotent, not another symbol.
            }
            named.push_back(item);
        }
    }
    if (auto meta = extra.find("x-mapget"); meta != extra.end()) {
        for (auto key : {"numericDomain", "bitWidth", "namedValues"})
            meta->erase(key);
        if (auto relations = meta->find("relationDeclarations"); relations != meta->end()) {
            if (!relations->is_array())
                throw std::invalid_argument("relationDeclarations must be an array.");
            for (auto& relation : *relations) {
                if (!relation.contains("complete") && relation.contains("coverage"))
                    relation["complete"] = relation.at("coverage") == "complete";
                relation.erase("coverage");
                relation.erase("incoming");
            }
        }
        if (meta->empty())
            extra.erase("x-mapget");
    }
    auto publish = [&]
    {
        appendMetadata(
            [&]
            {
                if (extension)
                    extension();
                auto stored = annotation(extra);
                data(id).annotations =
                    stored;  // Last nonthrowing publication; both owners roll back first.
            });
    };
    // Sealing a native shape protects its structure, not descriptive map metadata.
    // Scalar changes still pass through the generic row's normal mutation guard.
    if (!numeric && !width && named.empty())
        pool.appendExtension(publish);
    else
        pool.applyScalarAnnotations(id, numeric, width, named, publish);
}

/** Atomically replace boundary annotations without replacing an existing type's structure. */
void MapSchema::setJsonSchemaAnnotations(TypeId id, Json annotations)
{
    requireMutable();
    schema_->requireType(id);
    schema_detail::validateJsonSchema(annotations);
    if (!annotations.is_object() ||
        schema_detail::jsonSchemaAnnotations(annotations) != annotations)
        throw std::invalid_argument(
            "Schema annotations cannot replace structural type declarations.");
    schema_->compileScalarMetadata(
        id,
        annotations,
        [&]
        {
            schema_detail::forEachValidationSchema(
                annotations,
                [&](auto& constraint, auto const&)
                {
                    auto child = addJsonSchema(constraint);
                    constraint = {{"$ref", "#/definitions/" + std::to_string(child)}};
                });
        });
}

/** Project reachable canonical records directly; no builder-shaped graph survives or is rebuilt. */
Json LayerSchema::toJsonSchema() const
{
    auto owner = mapSchema();
    auto const& graph = *owner->schema_;
    auto const& layer = graph.layerAt(addr_.index());
    std::map<std::string_view, TypeId, std::less<>> aliases;
    graph.forEachAlias(MapSchema::Impl::GlobalLayer, [&](auto key, auto id) { aliases[key] = id; });
    graph.forEachAlias(addr_.index(), [&](auto key, auto id) { aliases[key] = id; });
    std::vector<TypeId> pending;
    for (auto const& feature : graph.featureRoots(layer))
        pending.push_back(feature.root);
    if (layer.root)
        pending.push_back(layer.root);
    graph.forEachAlias(addr_.index(), [&](auto, auto id) { pending.push_back(id); });
    std::vector<bool> reachable(owner->typeCount() + 1, pending.empty());
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (!id || !graph.valid(id) || reachable[id])
            continue;
        reachable[id] = true;
        auto type = owner->schemaType(id);
        type.forEachDirectField(
            [&](auto const& field)
            {
                for (auto child : field.types())
                    pending.push_back(child);
            });
        for (auto child : type.elements())
            pending.push_back(child);
        for (auto child : type.alternatives())
            pending.push_back(child);
        if (auto base = type.baseType())
            pending.push_back(base.id());
        auto extra = graph.annotation(graph.data(id).annotations);
        schema_detail::forEachValidationSchema(
            extra,
            [&](auto const& value, auto const&)
            {
                auto reference = value.at("$ref").template get<std::string>();
                constexpr std::string_view PREFIX = "#/definitions/";
                if (!reference.starts_with(PREFIX))
                    throw std::invalid_argument("Invalid validation type reference.");
                auto digits = std::string_view(reference).substr(PREFIX.size());
                uint32_t target = 0;
                auto [end, error] =
                    std::from_chars(digits.data(), digits.data() + digits.size(), target);
                if (error != std::errc{} || end != digits.data() + digits.size() ||
                    target > simfil::MaxTypeId || !graph.valid(TypeId(target)))
                    throw std::invalid_argument("Dangling validation type reference.");
                pending.push_back(TypeId(target));
            });
    }
    auto reference = [](TypeId id)
    {
        return Json{{"$ref", "#/definitions/" + std::to_string(id)}};
    };
    auto edges = [&](auto ids)
    {
        if (ids.empty())
            return Json{{"x-mapget", {{"unknownEdge", true}}}};
        if (ids.size() == 1 && !ids.front())
            return Json{{"x-mapget", {{"unknownEdge", true}, {"unknownTarget", true}}}};
        if (ids.size() == 1)
            return reference(ids.front());
        auto branches = Json::array();
        for (auto id : ids)
            branches.push_back(id ? reference(id) : Json{{"x-mapget", {{"unknownEdge", true}}}});
        return Json{{"anyOf", std::move(branches)}, {"x-mapget", {{"edgeAlternatives", true}}}};
    };
    auto scalarJson = [](auto const& value)
    {
        return std::visit([](auto v) { return Json(v); }, value);
    };
    Json result{
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"definitions", Json::object()},
        {"x-mapget", {{"typeDefinitions", true}}}};
    std::map<TypeId, std::vector<std::string_view>> names;
    for (auto const& [key, id] : aliases)
        names[id].push_back(key);
    for (size_t index = 1; index < reachable.size(); ++index) {
        auto id = TypeId(index);
        auto const& row = graph.data(id);
        if (!reachable[id] || row.flags & MapSchema::Impl::QueryRootFlag)
            continue;
        auto type = owner->schemaType(id);
        auto kindName = simfil::SchemaType::kindNameId(type.kind());
        auto& out = result["definitions"][std::to_string(id)];
        out = graph.annotation(row.annotations);
        auto& meta = out["x-mapget"];
        if (meta.is_null())
            meta = Json::object();
        meta.update(
            {{"typeId", id},
             {"kind", schema_detail::exportKind(type)},
             {"schemaKey", graph.text(row.key)},
             {"metaType", graph.text(row.metaType)},
             {"typename", type.typeName()},
             {"zserioType", graph.text(row.zserioType)},
             {"attributeType", graph.text(row.attributeType)},
             {"attributeTypeCode", graph.text(row.attributeCode)},
             {"requiredFields", Json::object()},
             {"nullable", type.nullable() ? Json(*type.nullable()) : Json(nullptr)},
             {"keys", names[id]}});
        for (auto const& site : graph.ownerSites(row.owners)) {
            if (site.featureLayer && site.featureLayer != layer.name)
                continue;
            meta["attributeOwners"].push_back(
                {{"featureType", graph.name(site.feature)},
                 {"featureLayer", graph.name(site.featureLayer)},
                 {"layer", graph.name(site.layer)},
                 {"name", graph.name(site.name)}});
        }
        type.forEachFieldAlias([&](auto from, auto to)
                               { meta["fieldAliases"][graph.name(from)] = graph.name(to); });
        if (auto base = type.baseType())
            meta["baseType"] = reference(base.id());
        // JSON property maps sort keys. Preserve declaration order even without
        // inheritance, including named context lookups on native point arrays.
        if (type.baseType() || !type.directFields().empty()) {
            meta["directFields"] = Json::array();
            type.forEachDirectField(
                [&](auto const& field) { meta["directFields"].push_back(field.name()); });
        }
        auto field = [&](auto const& item)
        {
            auto name = std::string(item.name());
            auto value = edges(item.types());
            if (item.multimap()) {
                out["properties"][name] = {
                    {"x-mapget-multimap", true},
                    {"anyOf", Json::array({value, {{"type", "array"}, {"items", value}}})}};
                out["properties"]["_multimap"] = {{"const", true}};
            }
            else
                out["properties"][name] = std::move(value);
            if (auto required = item.required()) {
                if (item.parentType().id() == id)
                    meta["requiredFields"][name] = *required;
                if (*required &&
                    (!out.contains("required") ||
                     std::find(out["required"].begin(), out["required"].end(), name) ==
                         out["required"].end()))
                    out["required"].push_back(name);
            }
            if (item.parentType().id() == id &&
                item.role() == simfil::SchemaType::FieldRole::Context)
                meta["contextFields"].push_back(name);
        };
        if (type.composition() != Composition::None) {
            auto op = type.composition() == Composition::AllOf ?
                "allOf" :
                type.composition() == Composition::OneOf ?
                "oneOf" :
                "anyOf";
            out[op] = Json::array();
            for (auto child : type.alternatives())
                out[op].push_back(reference(child));
        }
        else if (
            kindName != simfil::SchemaType::kindNameId(Kind::Unknown) &&
            kindName != simfil::SchemaType::kindNameId(Kind::Any) &&
            type.hasAffinity(simfil::ValueType::Object))
        {
            out["type"] = "object";
            if (!out.contains("additionalProperties"))
                out["additionalProperties"] = type.open();
            out["properties"] = Json::object();
            // JSON validators need effective fields; directFields restores native inheritance.
            type.forEachField(field);
        }
        else if (
            kindName != simfil::SchemaType::kindNameId(Kind::Unknown) &&
            kindName != simfil::SchemaType::kindNameId(Kind::Any) &&
            type.hasAffinity(simfil::ValueType::Array))
        {
            out["type"] = "array";
            if (!type.elements().empty())
                out["items"] = edges(type.elements());
            type.forEachDirectField(field);
        }
        else {
            for (auto [kind, name] :
                 {std::pair{Kind::String, "string"},
                  {Kind::Int, "integer"},
                  {Kind::Float, "number"},
                  {Kind::Bool, "boolean"},
                  {Kind::Null, "null"}})
                if (kindName == simfil::SchemaType::kindNameId(kind))
                    out["type"] = name;
            if (kindName == simfil::SchemaType::kindNameId(Kind::Never))
                out["not"] = Json::object();
            if (type.isBitmask()) {
                out["type"] = "object";
                out["required"] = {"_bitmask", "bits", "schema", "typeId"};
                out["additionalProperties"] = false;
                out["properties"] = {
                    {"_bitmask", {{"const", true}}},
                    {"bits", {{"type", "string"}, {"pattern", "^0x[0-9a-f]{16}$"}}},
                    {"schema", {{"type", "string"}, {"minLength", 1}}},
                    {"typeId",
                     {{"type", "integer"}, {"minimum", 1}, {"maximum", simfil::MaxTypeId}}}};
                meta["bitWidth"] = type.bitWidth();
            }
            else if (kindName == simfil::SchemaType::kindNameId(BitmaskKind))
                out["type"] = "string";
            if (kindName == simfil::SchemaType::kindNameId(Kind::Bytes)) {
                out["type"] = "object";
                out["required"] = {"_bytes", "hex", "number"};
                out["properties"] = {
                    {"_bytes", {{"const", true}}},
                    {"hex", {{"type", "string"}}},
                    {"number", {{"type", {"integer", "null"}}}}};
            }
        }
        if (meta.value("jsonTypeImplicit", false))
            out.erase("type");
        bool literalEnum = out.contains("enum") || out.contains("const");
        type.forEachEnumSymbol(
            [&](auto symbol)
            {
                if (kindName == simfil::SchemaType::kindNameId(BitmaskKind))
                    meta["bitmaskValues"].push_back(graph.name(symbol));
                else if (!literalEnum)
                    out["enum"].push_back(graph.name(symbol));
            });
        type.forEachEnumValue(
            [&](auto const& value)
            {
                if (auto bytes = std::get_if<simfil::ByteArray>(&value))
                    meta["enumBytes"].push_back(bytes->toHex());
                if (literalEnum)
                    return;
                std::visit(
                    [&](auto const& literal)
                    {
                        using T = std::decay_t<decltype(literal)>;
                        if constexpr (std::is_same_v<T, simfil::ByteArray>) {
                            auto number = literal.decodeBigEndianI64();
                            out["enum"].push_back(
                                {{"_bytes", true},
                                 {"hex", literal.toHex(false)},
                                 {"number", number ? Json(*number) : Json(nullptr)}});
                        }
                        else if constexpr (std::is_same_v<T, std::monostate>)
                            out["enum"].push_back(nullptr);
                        else
                            out["enum"].push_back(literal);
                    },
                    value);
            });
        if (auto numeric = type.numericDomain()) {
            auto& value = meta["numericDomain"];
            value = {
                {"encoding", numeric->encoding},
                {"unit", numeric->unit},
                {"scale", numeric->scale},
                {"offset", numeric->offset},
                {"complete", numeric->complete}};
            if (numeric->minimum)
                value["minimum"] = scalarJson(*numeric->minimum);
            if (numeric->maximum)
                value["maximum"] = scalarJson(*numeric->maximum);
            for (auto const& range : numeric->ordinaryRanges)
                value["ordinaryRanges"].push_back(
                    {{"minimum", scalarJson(range.min)}, {"maximum", scalarJson(range.max)}});
        }
        type.forEachNamedValue(
            [&](auto const& value)
            {
                static constexpr std::string_view ROLES[]{"constant", "special", "flag"};
                static constexpr std::string_view
                    MEANINGS[]{"ordinary", "unknown", "variable", "unbounded", "saturated"};
                Json named{
                    {"name", value.name},
                    {"value", scalarJson(value.value)},
                    {"role", ROLES[unsigned(value.role)]},
                    {"meaning", MEANINGS[unsigned(value.meaning)]},
                    {"deprecated", value.deprecated},
                    {"removed", value.removed}};
                if (value.rawValue)
                    named["rawValue"] = scalarJson(*value.rawValue);
                meta["namedValues"].push_back(std::move(named));
            });
        if (type.nullable().value_or(false) &&
            kindName != simfil::SchemaType::kindNameId(Kind::Null)) {
            // Null must bypass enum/const, conditionals and OneOf, not merely widen `type`.
            auto metadata = std::move(meta);
            out.erase("x-mapget");
            metadata["nullableProjection"] = true;
            out = Json{
                {"x-mapget", std::move(metadata)},
                {"anyOf", Json::array({std::move(out), {{"type", "null"}}})}};
        }
    }
    if (graph.relationsDeclared(layer)) {
        for (auto const& [name, root] : graph.featureRoots(layer)) {
            auto& metadata = result["definitions"][std::to_string(root)]["x-mapget"];
            metadata["relationDeclarations"] = Json::array();
            metadata["relationDeclarationsKnown"] = graph.relationsKnown(layer);
            for (auto const& relation : graph.relationsOf(layer)) {
                if (relation.owner != name)
                    continue;
                static constexpr std::string_view LOCALITIES[]{
                    "unknown",
                    "same-container",
                    "may-cross-container"};
                auto targets = Json::array();
                for (auto target : graph.relationTargets.range(relation.targets))
                    targets.push_back(graph.name(target));
                metadata["relationDeclarations"].push_back(
                    {{"ownerType", graph.name(relation.owner)},
                     {"name", graph.name(relation.name)},
                     {"targetTypes", std::move(targets)},
                     {"complete", graph.relationComplete(relation)},
                     {"locality", LOCALITIES[relation.locality]}});
            }
        }
    }
    if (layer.root)
        result["$ref"] = "#/definitions/" + std::to_string(layer.root);
    else
        for (auto root : graph.producedRoots(layer))
            result["anyOf"].push_back(reference(root));
    return result;
}
}  // namespace mapget
