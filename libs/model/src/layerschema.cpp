#include "mapget/model/layerschema.h"
#include "mapget/model/simfilutil.h"
#include "mapget/model/stringpool.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "simfil/simfil.h"

namespace mapget
{

namespace
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
        return std::any_of(typeIt->begin(), typeIt->end(), [&](auto const& item) {
            return item.is_string() && item.template get_ref<const std::string&>() == type;
        });
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
    return schema.contains("const")
        || schema.contains("enum")
        || hasType(schema, "null")
        || hasType(schema, "boolean")
        || hasType(schema, "integer")
        || hasType(schema, "number")
        || hasType(schema, "string");
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
    case Kind::Object:
        return "o";
    case Kind::Array:
        return "a";
    case Kind::Value:
        return "v";
    default: return "n";
    }

    return "n";
}

/** Build a memo key that keeps x-mapget context-sensitive aliases distinct. */
std::string contextMemoKey(
    std::string_view pointer,
    std::optional<Kind> kind,
    BuildContext const& context)
{
    auto appendSized = [](std::string& result, std::string_view value) {
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
std::string annotatedKey(
    nlohmann::json const& schema,
    std::string_view pointer,
    BuildContext const& context)
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
    if (metaType == "AttributeContainer" && !featureType.empty() && !context.attributeLayerName_.empty()) {
        return "AttributeContainer:" + featureType + ":" + context.attributeLayerName_;
    }
    if (metaType == "Attribute" && !featureType.empty() && !context.attributeLayerName_.empty()) {
        auto attributeTypeCode = metadataString(metadata, "attributeTypeCode");
        if (!attributeTypeCode.empty()) {
            return "Attribute:" + featureType + ":" + context.attributeLayerName_ + ":" + attributeTypeCode;
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

/** Quote one string as a SIMFIL string literal. */
std::string simfilStringLiteral(std::string_view value)
{
    return nlohmann::json(std::string(value)).dump();
}

/** Trim whitespace around a query fragment without changing the expression. */
std::string trimQuery(std::string_view value)
{
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

/** Return whether a path segment can be emitted as dotted SIMFIL field syntax. */
bool isIdentifier(std::string_view value)
{
    if (value.empty()) {
        return false;
    }
    auto first = static_cast<unsigned char>(value.front());
    if (!std::isalpha(first) && value.front() != '_') {
        return false;
    }
    return std::ranges::all_of(value.begin() + 1, value.end(), [](char ch) {
        auto c = static_cast<unsigned char>(ch);
        return std::isalnum(c) || ch == '_';
    });
}

/** Attribute guard that is valid from an attribute-root overlay context. */
std::string attributeScopeGuard(LayerSchema::AttributePathOwner const& scope)
{
    return fmt::format(
        "$feature.typeId == {} and $layer == {} and $name == {}",
        simfilStringLiteral(scope.featureType_),
        simfilStringLiteral(scope.attributeLayerName_),
        simfilStringLiteral(scope.attributeName_));
}

/** Wrap a branch in parentheses to avoid precedence surprises in generated ORs. */
std::string parenthesized(std::string expression)
{
    return "(" + std::move(expression) + ")";
}

/** Join normalized attribute-scope branch predicates with OR. */
std::string joinOr(std::vector<std::string> branches)
{
    if (branches.empty()) {
        return {};
    }
    auto result = std::move(branches.front());
    for (size_t i = 1; i < branches.size(); ++i) {
        result = parenthesized(std::move(result)) + " or " + parenthesized(std::move(branches[i]));
    }
    return result;
}

/** One AST-derived schema path reference owned by an attribute branch. */
struct AttributeQueryReference
{
    LayerSchema::AttributePathOwner owner;
    std::vector<std::string> fieldPath;
    std::vector<std::string> expressionPath;
    simfil::SourceLocation location;
    bool viaWildcard = false;
    std::optional<std::string> equalsStringLiteral;
};

/** Schema-aware compile result used by query normalization. */
struct FeatureQueryAnalysis
{
    std::string astDebug;
    std::vector<AttributeQueryReference> attributeReferences;
    bool hasFeatureOwnedReference = false;
    bool hasDynamicReference = false;
};

bool sourceRangeCoversWholeQuery(std::string_view query, simfil::SourceLocation location);

/** Convert one compile-local SchemaPath into field names. Array markers are ignored for source-level path rewrites. */
std::optional<std::vector<std::string>> schemaPathFieldNames(
    simfil::Environment& env,
    simfil::SchemaPath const& path)
{
    std::vector<std::string> result;
    for (auto const& segment : path) {
        if (segment.kind != simfil::SchemaPathSegment::Kind::Field) {
            continue;
        }
        auto fieldName = env.strings()->resolve(segment.field);
        if (!fieldName) {
            return std::nullopt;
        }
        result.emplace_back(*fieldName);
    }
    return result;
}

/** Convert one compile-local SchemaPath into SIMFIL path segments, preserving array wildcards. */
std::optional<std::vector<std::string>> schemaPathExpressionSegments(
    simfil::Environment& env,
    simfil::SchemaPath const& path)
{
    std::vector<std::string> result;
    for (auto const& segment : path) {
        if (segment.kind == simfil::SchemaPathSegment::Kind::ArrayElement) {
            result.emplace_back("*");
            continue;
        }
        auto fieldName = env.strings()->resolve(segment.field);
        if (!fieldName) {
            return std::nullopt;
        }
        result.emplace_back(*fieldName);
    }
    return result;
}

/** Stable identity for one layer-local attribute owner. */
std::string attributeOwnerKey(LayerSchema::AttributePathOwner const& owner)
{
    return owner.featureType_ + "\n" + owner.attributeLayerName_ + "\n" + owner.attributeName_;
}

/** Return whether two attribute owner records address the same layer-local attribute context. */
bool sameAttributeOwner(
    LayerSchema::AttributePathOwner const& lhs,
    LayerSchema::AttributePathOwner const& rhs)
{
    return lhs.featureType_ == rhs.featureType_
        && lhs.attributeLayerName_ == rhs.attributeLayerName_
        && lhs.attributeName_ == rhs.attributeName_;
}

/** Return the attribute-root suffix of a feature-root path owned by the supplied attribute. */
std::optional<std::string> attributeRootPathForFeaturePath(
    std::vector<std::string> const& fieldPath,
    LayerSchema::AttributePathOwner const& owner)
{
    auto attrIt = std::ranges::find(fieldPath, owner.attributeName_);
    if (attrIt == fieldPath.end()) {
        return std::nullopt;
    }
    auto suffixBegin = attrIt + 1;
    if (suffixBegin == fieldPath.end()) {
        return std::string("true");
    }

    std::string result;
    for (auto it = suffixBegin; it != fieldPath.end(); ++it) {
        if (*it == "*") {
            result += result.empty() ? "*" : ".*";
        }
        else if (isIdentifier(*it)) {
            if (!result.empty()) {
                result += ".";
            }
            result += *it;
        }
        else {
            result += "[" + simfilStringLiteral(*it) + "]";
        }
    }
    return result.empty() ? std::optional<std::string>{"true"} : std::optional<std::string>{result};
}

/** Return one unguarded attribute-root predicate for a schema-generated enum comparison. */
std::optional<std::string> generatedAttributeRootPredicate(
    AttributeQueryReference const& reference,
    LayerSchema::AttributePathOwner const& owner,
    std::string_view query)
{
    if (!reference.equalsStringLiteral
        || (!reference.viaWildcard
            && reference.location.size != 0
            && !sourceRangeCoversWholeQuery(query, reference.location))) {
        return std::nullopt;
    }
    auto replacement = attributeRootPathForFeaturePath(reference.expressionPath, owner);
    if (!replacement || *replacement == "true") {
        return std::nullopt;
    }
    return *replacement + " == " + simfilStringLiteral(*reference.equalsStringLiteral);
}

/** Build a compact generic attribute-root body when specific scope guards would be too broad. */
std::string genericAttributeRootQuery(
    std::string_view query,
    std::vector<AttributeQueryReference> const& references)
{
    std::vector<std::string> predicates;
    std::set<std::string> seen;
    for (auto const& reference : references) {
        auto predicate = generatedAttributeRootPredicate(reference, reference.owner, query);
        if (predicate && seen.insert(*predicate).second) {
            predicates.push_back(std::move(*predicate));
        }
    }
    return joinOr(std::move(predicates));
}

/** One source-location rewrite derived from a schema-aware AST reference. */
struct SourceRewrite
{
    uint32_t offset = 0;
    uint32_t size = 0;
    std::string replacement;
};

/** Apply non-overlapping source rewrites in reverse order. */
std::string applySourceRewrites(std::string query, std::vector<SourceRewrite> rewrites)
{
    std::ranges::sort(rewrites, {}, [](auto const& rewrite) {
        return rewrite.offset;
    });

    std::vector<SourceRewrite> nonOverlapping;
    uint32_t previousEnd = 0;
    for (auto const& rewrite : rewrites) {
        if (rewrite.size == 0 || rewrite.offset < previousEnd || rewrite.offset + rewrite.size > query.size()) {
            continue;
        }
        previousEnd = rewrite.offset + rewrite.size;
        nonOverlapping.push_back(rewrite);
    }

    for (auto it = nonOverlapping.rbegin(); it != nonOverlapping.rend(); ++it) {
        query.replace(it->offset, it->size, it->replacement);
    }
    return query;
}

/** Return whether one AST source range covers the whole normalized query. */
bool sourceRangeCoversWholeQuery(std::string_view query, simfil::SourceLocation location)
{
    if (location.size == 0 || location.offset + location.size > query.size()) {
        return false;
    }
    return location.offset == 0 && location.size == query.size();
}

/** Return whether a source range sits inside a function that changes feature-level cardinality. */
bool sourceRangeIsInsideAggregateCall(std::string_view query, simfil::SourceLocation location)
{
    if (location.size == 0 || location.offset + location.size > query.size()) {
        return false;
    }

    std::vector<std::string> enclosingCalls;
    auto inString = false;
    auto escaped = false;
    for (size_t i = 0; i < location.offset; ++i) {
        auto const c = query[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            }
            else if (c == '\\') {
                escaped = true;
            }
            else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
            continue;
        }
        if (c == '(') {
            auto nameEnd = i;
            while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(query[nameEnd - 1]))) {
                --nameEnd;
            }
            auto nameBegin = nameEnd;
            while (nameBegin > 0) {
                auto const nameChar = query[nameBegin - 1];
                if (!std::isalnum(static_cast<unsigned char>(nameChar)) && nameChar != '_') {
                    break;
                }
                --nameBegin;
            }
            auto name = std::string(query.substr(nameBegin, nameEnd - nameBegin));
            std::ranges::transform(name, name.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            enclosingCalls.push_back(std::move(name));
            continue;
        }
        if (c == ')' && !enclosingCalls.empty()) {
            enclosingCalls.pop_back();
        }
    }

    static constexpr std::array aggregateFunctions = {
        std::string_view("avg"),
        std::string_view("count"),
        std::string_view("exists"),
        std::string_view("max"),
        std::string_view("mean"),
        std::string_view("min"),
        std::string_view("sum")
    };
    return std::ranges::any_of(enclosingCalls, [](std::string const& name) {
        return std::ranges::find(aggregateFunctions, name) != aggregateFunctions.end();
    });
}

/** Attribute scope/style inference must ignore references inside feature-level aggregate calls. */
bool referenceIsInferenceEligible(std::string_view query, simfil::SourceLocation location)
{
    return !sourceRangeIsInsideAggregateCall(query, location);
}

/** Return all attribute contexts whose mapget type-code matches one exact AST symbol. */
std::vector<LayerSchema::AttributePathOwner> attributeScopesForStandaloneSymbol(
    LayerSchema const& registry,
    std::string_view symbol)
{
    std::vector<LayerSchema::AttributePathOwner> result;
    std::set<std::string> seenScopes;
    for (auto const& scope : registry.attributeScopes()) {
        auto const typeCode = registry.attributeTypeCode(scope.attributeSchema_);
        if (scope.attributeName_ != symbol && typeCode != symbol) {
            continue;
        }
        if (seenScopes.insert(attributeOwnerKey(scope)).second) {
            result.push_back(scope);
        }
    }
    return result;
}

/** Compile once with the real feature root and classify exact schema references by owner. */
tl::expected<FeatureQueryAnalysis, simfil::Error> analyzeFeatureQueryAst(
    LayerSchema const& registry,
    std::string_view query,
    std::string_view featureType)
{
    auto strings = std::make_shared<StringPool>("SearchQueryNormalization");
    auto env = makeEnvironment(strings);
    auto registryPtr = std::shared_ptr<LayerSchema const>(&registry, [](LayerSchema const*) {});
    installCompletionLayerSchema(*env, std::move(registryPtr), strings);

    auto const featureSchemaId = registry.featureSchema(featureType);
    auto ast = simfil::compile(
        *env,
        query,
        simfil::CompileOptions{
            .any = false,
            .rewriteMode = simfil::RewriteMode::Schema,
            .rootSchema = featureSchemaId});
    if (!ast) {
        return tl::unexpected(ast.error());
    }

    FeatureQueryAnalysis result;
    result.astDebug = (*ast)->expr().toString();
    auto references = simfil::referencedSchemaPaths(*env, **ast, featureSchemaId);
    if (!references) {
        return tl::unexpected(references.error());
    }
    result.hasDynamicReference = references->hasBroadWildcardAccess
        || references->hasDynamicAccess
        || references->hasUnresolvedAccess;

    std::set<std::tuple<std::string, std::string, std::string, uint32_t, uint32_t, std::optional<std::string>>> seenReferences;
    for (auto const& reference : references->paths) {
        auto fieldPath = schemaPathFieldNames(*env, reference.path);
        auto expressionPath = schemaPathExpressionSegments(*env, reference.path);
        if (!fieldPath || !expressionPath) {
            result.hasDynamicReference = true;
            continue;
        }
        auto owner = registry.ownerForPath(featureType, featureSchemaId, *fieldPath);
        if (owner.kind_ == LayerSchema::PathOwnerKind::Feature) {
            result.hasFeatureOwnedReference = true;
            continue;
        }
        if (owner.kind_ != LayerSchema::PathOwnerKind::Attribute) {
            result.hasDynamicReference = true;
            continue;
        }
        if (!referenceIsInferenceEligible(query, reference.location)) {
            continue;
        }
        auto key = std::make_tuple(
            owner.attribute_.featureType_,
            owner.attribute_.attributeLayerName_,
            owner.attribute_.attributeName_,
            reference.location.offset,
            reference.location.size,
            reference.equalsStringLiteral);
        if (seenReferences.insert(std::move(key)).second) {
            result.attributeReferences.push_back({
                owner.attribute_,
                std::move(*fieldPath),
                std::move(*expressionPath),
                reference.location,
                reference.viaWildcard,
                reference.equalsStringLiteral});
        }
    }
    return result;
}

} // namespace

struct LayerSchema::Impl
{
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
        std::vector<std::string> flatFields_;
        std::vector<std::string> flatEnumSymbols_;
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
        bool finalized_ = false;
    };

    Impl()
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
        for (auto key :
             {"title",        "description",      "default",          "examples",
              "deprecated",   "readOnly",         "writeOnly",        "minimum",
              "maximum",      "exclusiveMinimum", "exclusiveMaximum", "multipleOf",
              "minLength",    "maxLength",        "pattern",          "format",
              "minItems",     "maxItems",         "uniqueItems",      "minProperties",
              "maxProperties"})
        {
            if (auto it = schema.find(key); it != schema.end()) {
                result[key] = *it;
            }
        }
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
        registerKey(key, id);
        if (requested == simfil::NoSchemaId) {
            // Canonical import restores producer aliases, not incidental transport paths.
            registerKey(pointer, id);
        }
        return id;
    }

    void registerAttributeOwner(simfil::SchemaId id, AttributePathOwner owner)
    {
        if (!valid(id) ||
            owner.featureType_.empty() ||
            owner.attributeLayerName_.empty() ||
            owner.attributeName_.empty()) {
            return;
        }

        owner.attributeSchema_ = id;
        auto& owners = schemas_[id].attributeOwners_;
        auto duplicate = std::ranges::find_if(owners, [&](auto const& existing) {
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
        std::string_view metaType)
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
            if (auto it = metadata->find("kind"); it != metadata->end()) {
                auto packed = it->get<uint32_t>();
                auto name = packed >> 16;
                if (name == 0 || name >= simfil::StringPool::FirstDynamicId ||
                    (packed & 0xffff) >= (1u << unsigned(simfil::ValueType::LAST_)))
                {
                    throw std::invalid_argument("Invalid packed schema kind.");
                }
                domain.kind_ = Kind(packed);
            }
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
        }
        if (auto properties = schema.find("properties");
            properties != schema.end() && properties->is_object())
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
                    registerKey(alias.get<std::string>(), id);
                }
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
        registerAttributeOwner(
            id,
            AttributePathOwner{
                context.featureType_,
                context.attributeLayerName_,
                std::move(attributeName),
                id});
    }

    [[nodiscard]] std::optional<AttributePathOwner> uniqueAttributeOwner(
        simfil::SchemaId id,
        std::string_view featureType) const
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

    [[nodiscard]] std::vector<std::string> constantTypeNames(
        simfil::SchemaId id,
        std::string_view symbolName) const
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
        auto const hasDirectEnumSymbol =
            std::ranges::find(schema.directEnumSymbols_, symbolName) != schema.directEnumSymbols_.end();
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
                 {"kind", uint32_t(domain.kind_)},
                 {"schemaKey", domain.entry_.key_},
                 {"metaType", domain.entry_.metaType_},
                 {"typename", domain.typeName_},
                 {"zserioType", domain.zserioType_},
                 {"attributeType", domain.attributeType_},
                 {"attributeTypeCode", domain.attributeTypeCode_},
                 {"requiredFields", domain.required_},
                 {"nullable",
                  domain.nullable_ ? nlohmann::json(*domain.nullable_) : nlohmann::json(nullptr)}});
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
                    if (required) {
                        out["required"].push_back(field);
                    }
                }
                if (!domain.multimapFields_.empty()) {
                    out["properties"]["_multimap"] = {{"const", true}};
                }
            }
            else if (hasAffinity(domain.kind_, simfil::ValueType::Array)) {
                if (domain.kind_ != Kind::Unknown && domain.kind_ != Kind::Any) {
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
                if (domain.kind_ == LayerSchema::BitmaskKind) {
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
            for (auto const& symbol : domain.directEnumSymbols_) {
                if (domain.kind_ == LayerSchema::BitmaskKind) {
                    meta["bitmaskValues"].push_back(symbol);
                }
                else {
                    out["enum"].push_back(symbol);
                }
            }
            for (auto const& value : domain.enumValues_) {
                std::visit(
                    [&](auto const& literal)
                    {
                        using T = std::decay_t<decltype(literal)>;
                        if constexpr (std::is_same_v<T, simfil::ByteArray>) {
                            meta["enumBytes"].push_back(literal.toHex());
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

    /** Prepare overlay roots and refresh all derived indexes before immutable publication. */
    void finalizeAll()
    {
        prepareAttributeQueryRoots();
        for (auto& schema : schemas_) {
            schema.flatFields_.clear();
            schema.flatEnumSymbols_.clear();
            schema.finalized_ = false;
            schema.complete_ = false;
        }
        for (size_t id = 1; id < schemas_.size(); ++id) {
            finalize(static_cast<simfil::SchemaId>(id));
        }
    }

    /** Prepare scope-specific overlay edges without allocating a second schema registry. */
    void prepareAttributeQueryRoots()
    {
        std::set<std::pair<simfil::SchemaId, simfil::SchemaId>> contexts;
        for (auto const& schema : schemas_) {
            if (schema.queryRoot_) {
                continue;
            }
            for (auto const& owner : schema.attributeOwners_) {
                auto feature = idsByKey_.find(LayerSchema::featureKey(owner.featureType_));
                if (feature != idsByKey_.end()) {
                    contexts.emplace(feature->second, schema.id_);
                }
            }
        }
        if (contexts.empty()) {
            return;
        }
        auto scalar = [&](Kind kind, std::string const& key)
        {
            if (auto found = idsByKey_.find(key); found != idsByKey_.end()) {
                return found->second;
            }
            auto id = allocate(kind, key, {}, {});
            schemas_[id].nullable_ = false;
            schemas_[id].queryRoot_ = true;
            return id;
        };
        auto string = scalar(Kind::String, "@query/string");
        auto integer = scalar(Kind::Int, "@query/integer");
        auto boolean = scalar(Kind::Bool, "@query/boolean");
        for (auto [feature, attribute] : contexts) {
            auto copy = schemas_[attribute];
            auto [found, inserted] = attributeQueryRoots_.try_emplace({feature, attribute});
            if (inserted) {
                found->second = allocate(LayerSchema::AttributeKind, {}, {}, "Attribute");
            }
            auto root = found->second;
            copy.id_ = root;
            copy.entry_ = entriesById_[root];
            copy.queryRoot_ = true;
            schemas_[root] = std::move(copy);
            auto overlay = [&](std::string_view name, simfil::SchemaId child)
            {
                addDirectField(root, name);
                // Runtime OverlayNode::set shadows an identically named payload field.
                schemas_[root].childSchemas_[std::string(name)] = {child};
                schemas_[root].required_[std::string(name)] = true;
            };
            overlay("$name", string);
            overlay("$layer", string);
            overlay("$feature", feature);
            overlay("$attributeIndex", integer);
            overlay("$validityIndex", integer);
            overlay("$validityCount", integer);
            overlay("$hasValidity", boolean);
        }
    }

    /** Prove completeness once during preparation, never during a feature-query hot path. */
    [[nodiscard]] bool computeComplete(simfil::SchemaId root) const
    {
        std::vector<simfil::SchemaId> pending{root};
        std::set<simfil::SchemaId> visited;
        while (!pending.empty()) {
            auto id = pending.back();
            pending.pop_back();
            if (!valid(id)) {
                return false;
            }
            if (!visited.insert(id).second) {
                continue;
            }
            auto const& schema = schemas_[id];
            if (schema.open_ ||
                simfil::Schema::kindNameId(schema.kind_) ==
                    simfil::Schema::kindNameId(Kind::Unknown) ||
                simfil::Schema::kindNameId(schema.kind_) == simfil::Schema::kindNameId(Kind::Any) ||
                (schema.composition_ == Composition::AllOf && schema.alternatives_.empty()))
            {
                return false;
            }
            for (auto const& field : schema.directFields_) {
                auto children = schema.childSchemas_.find(field);
                if (children == schema.childSchemas_.end() || children->second.empty()) {
                    return false;
                }
                pending.insert(pending.end(), children->second.begin(), children->second.end());
            }
            if (hasAffinity(schema.kind_, simfil::ValueType::Array) &&
                schema.composition_ == Composition::None && schema.elementSchemas_.empty())
            {
                return false;
            }
            pending.insert(
                pending.end(),
                schema.elementSchemas_.begin(),
                schema.elementSchemas_.end());
            pending.insert(pending.end(), schema.alternatives_.begin(), schema.alternatives_.end());
        }
        return true;
    }

    /** Summarize logical alternatives while leaving their operator and identities intact. */
    [[nodiscard]] uint16_t
    domainAffinities(simfil::SchemaId id, std::set<simfil::SchemaId>& active) const
    {
        if (!valid(id) || !active.insert(id).second) {
            return simfil::Schema::AllAffinities;
        }
        auto const& schema = schemas_[id];
        auto bits = simfil::Schema::affinities(schema.kind_);
        if (schema.composition_ != Composition::None) {
            bits = schema.composition_ == Composition::AllOf ? simfil::Schema::AllAffinities : 0;
            for (auto child : schema.alternatives_) {
                auto childBits = domainAffinities(child, active);
                bits = schema.composition_ == Composition::AllOf ?
                    bits & childBits :
                    bits | childBits;
            }
        }
        if (schema.nullable_) {
            auto nullBit = simfil::valueTypeAffinity(simfil::ValueType::Null);
            bits = *schema.nullable_ ? bits | nullBit : bits & ~nullBit;
        }
        active.erase(id);
        return bits;
    }

    void finalize(simfil::SchemaId id)
    {
        if (!valid(id) || schemas_[id].finalized_) {
            return;
        }

        std::vector<std::string> fields;
        std::vector<simfil::SchemaId> visitedFields;
        collectFields(id, visitedFields, fields);
        std::ranges::sort(fields);
        auto duplicates = std::ranges::unique(fields);
        fields.erase(duplicates.begin(), duplicates.end());
        schemas_[id].flatFields_ = std::move(fields);

        std::vector<std::string> symbols;
        std::vector<simfil::SchemaId> visitedEnumSymbols;
        collectEnumSymbols(id, visitedEnumSymbols, symbols);
        std::ranges::sort(symbols);
        auto symbolDuplicates = std::ranges::unique(symbols);
        symbols.erase(symbolDuplicates.begin(), symbolDuplicates.end());
        schemas_[id].flatEnumSymbols_ = std::move(symbols);

        std::set<simfil::SchemaId> active;
        auto bits = domainAffinities(id, active);
        schemas_[id].kind_ =
            simfil::Schema::makeKind(simfil::Schema::kindNameId(schemas_[id].kind_), bits);
        schemas_[id].complete_ = computeComplete(id);
        schemas_[id].finalized_ = true;
    }

    void collectFields(
        simfil::SchemaId id,
        std::vector<simfil::SchemaId>& visited,
        std::vector<std::string>& fields) const
    {
        if (!valid(id) || std::ranges::find(visited, id) != visited.end()) {
            return;
        }
        visited.push_back(id);

        auto const& schema = schemas_[id];
        fields.insert(fields.end(), schema.directFields_.begin(), schema.directFields_.end());
        for (auto const& [_, children] : schema.childSchemas_) {
            for (auto child : children) {
                collectFields(child, visited, fields);
            }
        }
        for (auto child : schema.elementSchemas_) {
            collectFields(child, visited, fields);
        }
        for (auto child : schema.alternatives_) {
            collectFields(child, visited, fields);
        }
    }

    void collectEnumSymbols(
        simfil::SchemaId id,
        std::vector<simfil::SchemaId>& visited,
        std::vector<std::string>& symbols) const
    {
        if (!valid(id) || std::ranges::find(visited, id) != visited.end()) {
            return;
        }
        visited.push_back(id);

        auto const& schema = schemas_[id];
        symbols.insert(symbols.end(), schema.directEnumSymbols_.begin(), schema.directEnumSymbols_.end());
        for (auto const& [_, children] : schema.childSchemas_) {
            for (auto child : children) {
                collectEnumSymbols(child, visited, symbols);
            }
        }
        for (auto child : schema.elementSchemas_) {
            collectEnumSymbols(child, visited, symbols);
        }
        for (auto child : schema.alternatives_) {
            collectEnumSymbols(child, visited, symbols);
        }
    }

    [[nodiscard]] bool canHaveField(simfil::SchemaId id, std::string_view fieldName)
    {
        if (!valid(id)) {
            return true;
        }
        if (fieldName == "attributes" &&
            metaType(id) == "Feature" &&
            schemas_[id].childSchemas_.contains(std::string(fieldName))) {
            // `attributes` is a feature-root alias for `properties`, but it is
            // intentionally kept out of direct/flat field caches so schema-
            // generated paths remain canonical.
            return true;
        }
        finalize(id);
        if (!schemas_[id].complete_) {
            return true;
        }
        auto const& fields = schemas_[id].flatFields_;
        return std::ranges::binary_search(fields, fieldName);
    }

    [[nodiscard]] bool canHaveEnumSymbol(simfil::SchemaId id, std::string_view symbolName)
    {
        if (!valid(id)) {
            return false;
        }
        finalize(id);
        auto const& symbols = schemas_[id].flatEnumSymbols_;
        return std::ranges::binary_search(symbols, symbolName);
    }

    [[nodiscard]] std::span<const std::string> directFields(simfil::SchemaId id) const
    {
        if (!valid(id)) {
            return {};
        }
        return schemas_[id].directFields_;
    }

    [[nodiscard]] std::span<const std::string> nestedFields(simfil::SchemaId id) const
    {
        if (!valid(id)) {
            return {};
        }
        return schemas_[id].flatFields_;
    }

    [[nodiscard]] std::span<const std::string> nestedEnumSymbols(simfil::SchemaId id) const
    {
        if (!valid(id)) {
            return {};
        }
        return schemas_[id].flatEnumSymbols_;
    }

    [[nodiscard]] std::span<const std::string> directEnumSymbols(simfil::SchemaId id) const
    {
        if (!valid(id)) {
            return {};
        }
        return schemas_[id].directEnumSymbols_;
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

    void forEachElementSchema(simfil::SchemaId id, const std::function<void(simfil::SchemaId)>& fn) const
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

    [[nodiscard]] std::optional<LayerSchema::NamedSchemaPath> firstScalarFieldPath(
        simfil::SchemaId id,
        bool skipRootAttributeMetadataFields = false) const
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
                if (isRoot
                    && skipRootAttributeMetadataFields
                    && isAttributeScalarShorthandMetadataField(fieldName)) {
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
                        false)) {
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
                    false)) {
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

    [[nodiscard]] std::vector<LayerSchema::NamedSchemaPath> scalarFieldPathsForAttribute(
        simfil::SchemaId rootSchema,
        std::string_view attributeTypeCode) const
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
                    collectScalarFieldPathsForAttribute(child, attributeTypeCode, visited, current, paths);
                }
                current.pop_back();
            }
        }
        else if (hasAffinity(schema.kind_, simfil::ValueType::Array)) {
            current.push_back({simfil::SchemaPathSegment::Kind::ArrayElement, {}});
            for (auto child : schema.elementSchemas_) {
                collectScalarFieldPathsForAttribute(child, attributeTypeCode, visited, current, paths);
            }
            current.pop_back();
        }
        visited.pop_back();
    }
};

namespace
{

/** Recursive JSON Schema compiler used by LayerSchema construction. */
class LayerSchemaCompiler
{
public:
    LayerSchemaCompiler(LayerSchema::Impl& registry, nlohmann::json const& root)
        : registry_(registry), root_(root)
    {
    }

    /** Reserve canonical IDs before following references; ordinary input uses deterministic
     * traversal. */
    void buildAll()
    {
        auto metadata = mapgetMetadata(root_);
        if (metadata && metadata->value("domainDefinitions", false)) {
            auto const* keyword = root_.contains("definitions") ? "definitions" : "$defs";
            auto const& definitions = root_.at(keyword);
            auto prefix = "/" + std::string(keyword) + "/";
            size_t highest = 0;
            for (auto const& [name, definition] : definitions.items()) {
                auto id = definition.at("x-mapget").at("schemaId").get<uint32_t>();
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
        else {
            build(root_, "#", {}, std::nullopt);
        }
        registry_.finalizeAll();
    }

    /** Append an input fragment without confusing its local references with another fragment. */
    simfil::SchemaId append()
    {
        pointerPrefix_ = "#/import/" + std::to_string(registry_.schemas_.size());
        return build(root_, pointerPrefix_, {}, std::nullopt);
    }

private:
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
        auto const* metadata = mapgetMetadata(schema);
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
                        found != registry_.idsByKey_.end()) {
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
            registry_.registerSchemaMetadata(id, schema, context, metaType);
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
            types != schema.end() && types->is_array() && !(metadata && metadata->contains("kind")))
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
        if (metadata && metadata->contains("kind") && metadata->at("kind") == uint32_t(Kind::Bytes))
        {
            // The JSON byte wrapper is not an object in the runtime domain graph.
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
        registry_.registerSchemaMetadata(id, schema, context, metaType);
        memo_[memoKey] = id;

        auto propertiesIt = schema.find("properties");
        if (propertiesIt == schema.end() || !propertiesIt->is_object()) {
            return id;
        }

        auto const shouldAddFeatureAttributesAlias = reservedIds_.empty() &&
            metaType == "Feature" && propertiesIt->contains("properties") &&
            !propertiesIt->contains("attributes");
        if (shouldAddFeatureAttributesAlias) {
            registry_.addDirectField(id, "attributes");
        }

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
        registry_.registerSchemaMetadata(id, schema, context, metaType);
        memo_[memoKey] = id;

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
        auto id = allocate(kind, std::move(key), std::move(pointer), metaType);
        registry_.schemas_[id].enumValues_ = std::move(values);
        registry_.registerSchemaMetadata(id, schema, context, metaType);
        memo_[memoKey] = id;

        registry_.addEnumSymbols(id, symbols);
        return id;
    }

    LayerSchema::Impl& registry_;
    nlohmann::json const& root_;
    std::map<std::string, simfil::SchemaId> memo_;
    std::set<std::string> activeRefs_;
    std::set<simfil::SchemaId> reservedIds_;
    std::map<std::string, simfil::SchemaId> transportedIds_;
    std::string pointerPrefix_;
};

/** SIMFIL schema adapter that resolves StringIds without inserting schema field names. */
class BoundSchema final : public simfil::Schema
{
public:
    BoundSchema(
        std::shared_ptr<LayerSchema const> registry,
        std::shared_ptr<simfil::StringPool const> strings,
        simfil::SchemaId id,
        bool materializeSchemaStrings = false)
        : registry_(std::move(registry)), strings_(std::move(strings)), id_(id)
    {
        if (materializeSchemaStrings) {
            auto mutableStrings = std::const_pointer_cast<simfil::StringPool>(strings_);
            materializeStringIds(mutableStrings);
        }
    }

    /** Return the object/array/value kind for the stable mapget SchemaId. */
    auto kind() const -> Kind override { return registry_ ? registry_->kind(id_) : Kind::Unknown; }

    /** Preserve producer metadata on the same lazy binding used by traversal. */
    auto typeName() const -> std::string_view override { return registry_->typeName(id_); }
    /** Retain the logical operator rather than pretending alternatives are fields. */
    auto composition() const -> Composition override { return registry_->composition(id_); }
    /** Visit alternative definitions at the same value position. */
    auto alternatives() const& -> std::span<const simfil::SchemaId> override
    {
        return registry_->alternatives(id_);
    }
    /** Share non-string literals; canonical string symbols stay pool-local. */
    auto enumValues() const& -> std::span<const simfil::ScalarValueType> override
    {
        return registry_->enumValues(id_);
    }
    /** Expose unknown/optional nullability independently of field presence. */
    auto nullable() const -> std::optional<bool> override { return registry_->nullable(id_); }
    /** Unknown/open metadata never proves a field absent. */
    auto open() const -> bool override { return registry_->open(id_); }
    /** Keep incomplete indexes conservative even after finalization. */
    auto reachabilityComplete() const -> bool override
    {
        return registry_->reachabilityComplete(id_);
    }
    /** Definitions are immutable for the lifetime of an installed binding. */
    auto finalized() const -> bool override { return true; }
    /** Resolve edge metadata through this binding's namespace. */
    auto fieldRequired(simfil::StringId field) const -> std::optional<bool> override
    {
        auto name = strings_->resolve(field);
        return name ? registry_->fieldRequired(id_, *name) : std::nullopt;
    }

    /** Resolve the field id through the datasource-owned pool and match by name. */
    auto canHaveField(simfil::StringId fieldId) const -> bool override
    {
        if (!registry_ || !strings_) {
            return true;
        }
        auto fieldName = strings_->resolve(fieldId);
        return !fieldName || registry_->canHaveField(id_, *fieldName);
    }

    /** Resolve enum-like string symbols through the datasource-owned pool and match by name. */
    auto canHaveEnumSymbol(simfil::StringId symbolId) const -> bool override
    {
        if (!registry_ || !strings_) {
            return false;
        }
        auto symbolName = strings_->resolve(symbolId);
        return symbolName && registry_->canHaveEnumSymbol(id_, *symbolName);
    }

    /** Return nested schema fields when this adapter was built for completion. */
    auto nestedFields() const& -> std::span<const simfil::StringId> override
    {
        return nestedFields_;
    }

    /** Return completion-local ids for direct schema fields, if materialized. */
    auto directFields() const& -> std::span<const simfil::StringId> override
    {
        return directFields_;
    }

    /** Return nested schema enum symbols when this adapter was built for completion. */
    auto nestedEnumSymbols() const& -> std::span<const simfil::StringId> override
    {
        return nestedEnumSymbols_;
    }

    /** Return direct schema enum symbols when this adapter was built for completion/compile. */
    auto directEnumSymbols() const& -> std::span<const simfil::StringId> override
    {
        return directEnumSymbols_;
    }

    /** Runtime literals live in value storage, not necessarily in the datasource dictionary. */
    auto hasDirectEnumSymbol(std::string_view symbol, simfil::StringPool const&) const
        -> bool override
    {
        auto names = registry_->directEnumSymbols(id_);
        return std::ranges::find(names, symbol) != names.end();
    }

    /** Return the overlay-name predicate for a matching attribute root. */
    auto symbolEqualityPaths(
        simfil::StringId symbolId,
        const std::function<const simfil::Schema*(simfil::SchemaId)>&) const -> std::vector<simfil::SchemaPath> override
    {
        if (!registry_ || !strings_) {
            return {};
        }

        auto symbolName = strings_->resolve(symbolId);
        if (!symbolName || registry_->attributeTypeCode(id_) != *symbolName) {
            return {};
        }

        auto nameId = std::const_pointer_cast<simfil::StringPool>(strings_)->get("$name");
        if (nameId == simfil::StringPool::Empty) {
            return {};
        }
        return {simfil::SchemaPath{{simfil::SchemaPathSegment::Kind::Field, nameId}}};
    }

    /** Return mapget attribute type-code scalar shorthand paths. */
    auto scalarFieldPathsForSymbol(
        simfil::StringId symbolId,
        const std::function<const simfil::Schema*(simfil::SchemaId)>&) const -> std::vector<simfil::SchemaPath> override
    {
        if (!registry_ || !strings_) {
            return {};
        }

        auto symbolName = strings_->resolve(symbolId);
        if (!symbolName) {
            return {};
        }

        auto namedPaths = registry_->scalarFieldPathsForAttribute(id_, *symbolName);
        std::vector<simfil::SchemaPath> result;
        result.reserve(namedPaths.size());
        auto mutableStrings = std::const_pointer_cast<simfil::StringPool>(strings_);
        for (auto const& namedPath : namedPaths) {
            simfil::SchemaPath path;
            path.reserve(namedPath.size());
            bool complete = true;
            for (auto const& segment : namedPath) {
                if (segment.kind_ == simfil::SchemaPathSegment::Kind::ArrayElement) {
                    path.push_back({simfil::SchemaPathSegment::Kind::ArrayElement, 0});
                    continue;
                }
                auto fieldId = mutableStrings->get(segment.field_);
                if (fieldId == simfil::StringPool::Empty) {
                    complete = false;
                    break;
                }
                path.push_back({simfil::SchemaPathSegment::Kind::Field, fieldId});
            }
            if (complete) {
                result.push_back(std::move(path));
            }
        }
        return result;
    }

private:
    /** Visit direct fields using ids from the completion/compile-local pool. */
    auto forEachDirectField(
        const std::function<void(simfil::StringId, std::span<const simfil::SchemaId>)>& fn) const -> void override
    {
        if (!registry_ || !strings_) {
            return;
        }

        auto mutableStrings = std::const_pointer_cast<simfil::StringPool>(strings_);
        registry_->forEachDirectField(id_, [&](std::string_view fieldName, std::span<const simfil::SchemaId> schemas) {
            auto fieldId = mutableStrings->get(fieldName);
            if (fieldId != simfil::StringPool::Empty) {
                fn(fieldId, schemas);
            }
        });
    }

    /** Visit possible array element schemas. */
    auto forEachElementSchema(const std::function<void(simfil::SchemaId)>& fn) const -> void override
    {
        if (!registry_) {
            return;
        }
        registry_->forEachElementSchema(id_, fn);
    }

    /** Insert schema-owned strings into the completion-local pool. */
    auto materializeStringIds(std::shared_ptr<simfil::StringPool> const& strings) -> void
    {
        if (!registry_ || !strings) {
            return;
        }

        materialize(registry_->directFields(id_), *strings, directFields_);
        materialize(registry_->nestedFields(id_), *strings, nestedFields_);
        materialize(registry_->nestedEnumSymbols(id_), *strings, nestedEnumSymbols_);
        materialize(registry_->directEnumSymbols(id_), *strings, directEnumSymbols_);
    }

    /** Convert schema-owned strings into StringIds in the provided temporary pool. */
    static auto materialize(
        std::span<const std::string> names,
        simfil::StringPool& strings,
        std::vector<simfil::StringId>& ids) -> void
    {
        ids.reserve(names.size());
        for (auto const& name : names) {
            if (auto id = strings.emplace(name)) {
                ids.push_back(*id);
            }
        }
    }

    /** Runtime pruning calls canHaveField directly, so no recursive StringId cache is built here. */
    auto collectNestedFields(
        const std::function<Schema*(simfil::SchemaId)>&,
        SchemaIdStack&,
        std::vector<simfil::StringId>&) const -> void override
    {
    }

    std::shared_ptr<LayerSchema const> registry_;
    std::shared_ptr<simfil::StringPool const> strings_;
    simfil::SchemaId id_ = simfil::NoSchemaId;
    std::vector<simfil::StringId> directFields_;
    std::vector<simfil::StringId> nestedFields_;
    std::vector<simfil::StringId> nestedEnumSymbols_;
    std::vector<simfil::StringId> directEnumSymbols_;
};

} // namespace

void installLayerSchemaImpl(
    simfil::Environment& env,
    std::shared_ptr<LayerSchema const> registry,
    std::shared_ptr<simfil::StringPool const> strings,
    bool materializeSchemaStrings);

LayerSchema::LayerSchema()
    : impl_(std::make_shared<Impl>())
{
}

LayerSchema::LayerSchema(nlohmann::json schema)
    : transportJsonSchema_(std::move(schema)),
      impl_(std::make_shared<Impl>())
{
    if (!transportJsonSchema_.is_null()) {
        LayerSchemaCompiler(*impl_, transportJsonSchema_).buildAll();
    }
}

std::shared_ptr<LayerSchema> LayerSchema::fromJsonSchema(nlohmann::json schema)
{
    if (schema.is_null()) {
        return nullptr;
    }
    return std::shared_ptr<LayerSchema>(new LayerSchema(std::move(schema)));
}

nlohmann::json LayerSchema::toJsonSchema() const
{
    std::lock_guard lock(transportJsonSchemaMutex_);
    if (transportJsonSchema_.is_null()) {
        transportJsonSchema_ = impl_->exportDomains();
    }
    return transportJsonSchema_;
}

MemoryUsageBreakdown LayerSchema::memoryUsage() const
{
    MemoryUsageBreakdown result;
    result.add("object", {sizeof(LayerSchema) + sizeof(Impl), sizeof(LayerSchema) + sizeof(Impl)});
    result.add("logical-schemas", vectorMemoryUsage(impl_->schemas_));
    for (auto const& schema : impl_->schemas_) {
        result.add("logical-schema-strings", stringMemoryUsage(schema.entry_.key_));
        result.add("logical-schema-strings", stringMemoryUsage(schema.entry_.jsonPointer_));
        result.add("logical-schema-strings", stringMemoryUsage(schema.entry_.metaType_));
        result.add("logical-schema-strings", stringMemoryUsage(schema.attributeTypeCode_));
        result.add("logical-schema-strings", stringMemoryUsage(schema.attributeType_));
        result.add("logical-schema-strings", stringMemoryUsage(schema.zserioType_));
        result.add("logical-schema-strings", stringMemoryUsage(schema.typeName_));
        result.add("alternatives", vectorMemoryUsage(schema.alternatives_));
        result.add("enum-values", vectorMemoryUsage(schema.enumValues_));
        result.add("schema-annotations", jsonMemoryUsage(schema.annotations_));
        result.add(
            "multimap-fields",
            {schema.multimapFields_.size() * sizeof(std::string),
             schema.multimapFields_.size() * (sizeof(std::string) + 3 * sizeof(void*))});
        for (auto const& field : schema.multimapFields_) {
            result.add("multimap-field-strings", stringMemoryUsage(field));
        }
        for (auto const& value : schema.enumValues_) {
            if (auto bytes = std::get_if<simfil::ByteArray>(&value)) {
                result.add("enum-value-bytes", stringMemoryUsage(bytes->bytes));
            }
        }
        result.add(
            "required-fields",
            {
                schema.required_.size() * sizeof(decltype(schema.required_)::value_type),
                schema.required_.size() *
                    (sizeof(decltype(schema.required_)::value_type) + 3 * sizeof(void*)),
            });
        for (auto const& [field, _] : schema.required_) {
            result.add("required-field-names", stringMemoryUsage(field));
        }
        result.add("direct-fields", stringVectorMemoryUsage(schema.directFields_));
        result.add("direct-enum-symbols", stringVectorMemoryUsage(schema.directEnumSymbols_));
        result.add("element-schemas", vectorMemoryUsage(schema.elementSchemas_));
        result.add("flat-fields", stringVectorMemoryUsage(schema.flatFields_));
        result.add("flat-enum-symbols", stringVectorMemoryUsage(schema.flatEnumSymbols_));
        result.add("attribute-owners", vectorMemoryUsage(schema.attributeOwners_));
        for (auto const& owner : schema.attributeOwners_) {
            result.add("attribute-owner-strings", stringMemoryUsage(owner.featureType_));
            result.add("attribute-owner-strings", stringMemoryUsage(owner.attributeLayerName_));
            result.add("attribute-owner-strings", stringMemoryUsage(owner.attributeName_));
        }
        result.add("child-schema-map", {
            schema.childSchemas_.size() * sizeof(decltype(schema.childSchemas_)::value_type),
            schema.childSchemas_.size() *
                (sizeof(decltype(schema.childSchemas_)::value_type) + 3 * sizeof(void*)),
        });
        for (auto const& [field, children] : schema.childSchemas_) {
            result.add("child-schema-fields", stringMemoryUsage(field));
            result.add("child-schema-values", vectorMemoryUsage(children));
        }
    }
    result.add("entries-by-id", vectorMemoryUsage(impl_->entriesById_));
    result.add(
        "attribute-query-roots",
        {
            impl_->attributeQueryRoots_.size() *
                sizeof(decltype(impl_->attributeQueryRoots_)::value_type),
            impl_->attributeQueryRoots_.size() *
                (sizeof(decltype(impl_->attributeQueryRoots_)::value_type) + 3 * sizeof(void*)),
        });
    for (auto const& entry : impl_->entriesById_) {
        result.add("entry-strings", stringMemoryUsage(entry.key_));
        result.add("entry-strings", stringMemoryUsage(entry.jsonPointer_));
        result.add("entry-strings", stringMemoryUsage(entry.metaType_));
    }
    result.add("key-index", {
        impl_->idsByKey_.size() * sizeof(decltype(impl_->idsByKey_)::value_type),
        impl_->idsByKey_.size() *
            (sizeof(decltype(impl_->idsByKey_)::value_type) + 3 * sizeof(void*)),
    });
    for (auto const& [key, _] : impl_->idsByKey_) {
        result.add("key-index-strings", stringMemoryUsage(key));
    }
    {
        std::lock_guard lock(transportJsonSchemaMutex_);
        // Diagnostics must not materialize the transport representation.
        if (!transportJsonSchema_.is_null()) {
            result.add("materialized-transport-json", jsonMemoryUsage(transportJsonSchema_));
        }
    }
    return result;
}

std::shared_ptr<LayerSchema const> LayerSchema::detachedCopy() const
{
    auto result = std::shared_ptr<LayerSchema>(new LayerSchema());
    result->impl_ = std::make_shared<Impl>(*impl_);
    // Imported schemas may contain validation keywords outside the domain model.
    // Preserve existing transport data without forcing a native producer to generate it.
    std::lock_guard lock(transportJsonSchemaMutex_);
    result->transportJsonSchema_ = transportJsonSchema_;
    return result;
}

simfil::SchemaId LayerSchema::addSchema(
    simfil::Schema::Kind kind,
    std::string key,
    std::string metaType,
    std::string jsonPointer)
{
    if (jsonPointer.empty()) {
        jsonPointer = "#/direct/" + std::to_string(impl_->schemas_.size());
    }
    return impl_->allocate(kind, std::move(key), std::move(jsonPointer), std::move(metaType));
}

simfil::SchemaId LayerSchema::addJsonSchema(nlohmann::json const& schema)
{
    return LayerSchemaCompiler(*impl_, schema).append();
}

void LayerSchema::setJsonSchemaAnnotations(simfil::SchemaId id, nlohmann::json annotations)
{
    if (!annotations.is_object() || Impl::annotations(annotations) != annotations) {
        throw std::invalid_argument(
            "Schema annotations cannot override domain structure or transport identities.");
    }
    if (impl_->valid(id)) {
        impl_->schemas_[id].annotations_ = std::move(annotations);
    }
}

void LayerSchema::registerSchemaKey(std::string key, simfil::SchemaId id)
{
    impl_->registerKey(key, id);
}

void LayerSchema::addFieldSchema(
    simfil::SchemaId parent,
    std::string fieldName,
    simfil::SchemaId child,
    bool multimap)
{
    impl_->addDirectField(parent, fieldName);
    if (child != simfil::NoSchemaId) {
        impl_->addChild(parent, fieldName, child);
    }
    if (multimap && impl_->valid(parent)) {
        impl_->schemas_[parent].multimapFields_.insert(std::move(fieldName));
    }
}

void LayerSchema::addElementSchema(simfil::SchemaId parent, simfil::SchemaId child)
{
    impl_->addElementSchema(parent, child);
}

void LayerSchema::addEnumSymbol(simfil::SchemaId schemaId, std::string symbolName)
{
    impl_->addEnumSymbol(schemaId, symbolName);
}

void LayerSchema::addEnumSymbols(simfil::SchemaId schemaId, std::span<const std::string> symbolNames)
{
    impl_->addEnumSymbols(schemaId, symbolNames);
}

void LayerSchema::addEnumValue(simfil::SchemaId schemaId, simfil::ScalarValueType value)
{
    if (std::holds_alternative<std::string>(value) ||
        std::holds_alternative<std::string_view>(value)) {
        throw std::invalid_argument("String enum values must use addEnumSymbol.");
    }
    if (impl_->valid(schemaId)) {
        impl_->schemas_[schemaId].enumValues_.push_back(std::move(value));
    }
}

void LayerSchema::setComposition(simfil::SchemaId schemaId, Composition composition)
{
    if (!impl_->valid(schemaId)) {
        return;
    }
    auto& schema = impl_->schemas_[schemaId];
    schema.composition_ = composition;
    if (composition != Composition::None) {
        schema.kind_ = composition == Composition::AllOf ?
            Kind::Intersection :
            composition == Composition::OneOf ?
            Kind::OneOf :
            Kind::Union;
    }
}

void LayerSchema::addAlternative(simfil::SchemaId schemaId, simfil::SchemaId alternative)
{
    if (impl_->valid(schemaId)) {
        impl_->schemas_[schemaId].alternatives_.push_back(alternative);
    }
}

void LayerSchema::setTypeName(simfil::SchemaId schemaId, std::string name)
{
    if (impl_->valid(schemaId)) {
        impl_->schemas_[schemaId].typeName_ = std::move(name);
    }
}

void LayerSchema::setOpen(simfil::SchemaId schemaId, bool open)
{
    if (impl_->valid(schemaId)) {
        impl_->schemas_[schemaId].open_ = open;
    }
}

void LayerSchema::setNullable(simfil::SchemaId schemaId, std::optional<bool> nullable)
{
    if (impl_->valid(schemaId)) {
        impl_->schemas_[schemaId].nullable_ = nullable;
    }
}

void LayerSchema::setFieldRequired(
    simfil::SchemaId schemaId,
    std::string field,
    std::optional<bool> required)
{
    if (!impl_->valid(schemaId)) {
        return;
    }
    if (required) {
        impl_->schemas_[schemaId].required_[std::move(field)] = *required;
    }
    else {
        impl_->schemas_[schemaId].required_.erase(field);
    }
}

void LayerSchema::setZserioType(simfil::SchemaId schemaId, std::string zserioType)
{
    if (impl_->valid(schemaId)) {
        impl_->schemas_[schemaId].zserioType_ = std::move(zserioType);
        if (impl_->schemas_[schemaId].typeName_.empty()) {
            impl_->schemas_[schemaId].typeName_ = impl_->schemas_[schemaId].zserioType_;
        }
    }
}

void LayerSchema::setAttributeMetadata(
    simfil::SchemaId schemaId,
    AttributePathOwner owner,
    std::string attributeType,
    std::string zserioType)
{
    if (!impl_->valid(schemaId)) {
        return;
    }

    impl_->schemas_[schemaId].attributeTypeCode_ = owner.attributeName_;
    impl_->schemas_[schemaId].attributeType_ = std::move(attributeType);
    if (!zserioType.empty()) {
        impl_->schemas_[schemaId].zserioType_ = std::move(zserioType);
    }
    if (impl_->schemas_[schemaId].typeName_.empty()) {
        impl_->schemas_[schemaId].typeName_ = impl_->schemas_[schemaId].zserioType_.empty() ?
            impl_->schemas_[schemaId].attributeType_ :
            impl_->schemas_[schemaId].zserioType_;
    }
    impl_->registerAttributeOwner(schemaId, std::move(owner));
}

void LayerSchema::finalize()
{
    impl_->finalizeAll();
    std::lock_guard lock(transportJsonSchemaMutex_);
    transportJsonSchema_ = nullptr;
}

std::string LayerSchema::featureKey(std::string_view featureType)
{
    return "Feature:" + std::string(featureType);
}

std::string LayerSchema::featurePropertiesKey(std::string_view featureType)
{
    return "FeatureProperties:" + std::string(featureType);
}

std::string LayerSchema::attributeLayerMapKey(std::string_view featureType)
{
    return "AttributeLayerMap:" + std::string(featureType);
}

std::string LayerSchema::attributeContainerKey(
    std::string_view featureType,
    std::string_view attributeLayerName)
{
    return "AttributeContainer:" + std::string(featureType) + ":" + std::string(attributeLayerName);
}

std::string LayerSchema::attributeKey(
    std::string_view featureType,
    std::string_view attributeLayerName,
    std::string_view attributeTypeCode)
{
    return "Attribute:" + std::string(featureType) + ":" + std::string(attributeLayerName) + ":" +
           std::string(attributeTypeCode);
}

LayerSchema::Entry const* LayerSchema::getSchema(std::string_view keyOrFeatureType) const
{
    auto exact = impl_->idsByKey_.find(keyOrFeatureType);
    auto id = exact == impl_->idsByKey_.end() ? featureSchema(keyOrFeatureType) : exact->second;
    if (id == simfil::NoSchemaId || id >= impl_->entriesById_.size()) {
        return nullptr;
    }
    return &impl_->entriesById_[id];
}

simfil::SchemaId LayerSchema::schemaId(std::string_view key) const
{
    auto it = impl_->idsByKey_.find(key);
    if (it == impl_->idsByKey_.end()) {
        return simfil::NoSchemaId;
    }
    return it->second;
}

simfil::Schema::Kind LayerSchema::kind(simfil::SchemaId schemaId) const
{
    return impl_->kind(schemaId);
}

std::string_view LayerSchema::typeName(simfil::SchemaId id) const
{
    return impl_->valid(id) ? impl_->schemas_[id].typeName_ : std::string_view{};
}

Composition LayerSchema::composition(simfil::SchemaId id) const
{
    return impl_->valid(id) ? impl_->schemas_[id].composition_ : Composition::None;
}

std::span<simfil::SchemaId const> LayerSchema::alternatives(simfil::SchemaId id) const
{
    return impl_->valid(id) ?
        std::span<simfil::SchemaId const>(impl_->schemas_[id].alternatives_) :
        std::span<simfil::SchemaId const>{};
}

std::span<simfil::ScalarValueType const> LayerSchema::enumValues(simfil::SchemaId id) const
{
    return impl_->valid(id) ?
        std::span<simfil::ScalarValueType const>(impl_->schemas_[id].enumValues_) :
        std::span<simfil::ScalarValueType const>{};
}

bool LayerSchema::open(simfil::SchemaId id) const
{
    return !impl_->valid(id) || impl_->schemas_[id].open_;
}

std::optional<bool> LayerSchema::nullable(simfil::SchemaId id) const
{
    return impl_->valid(id) ? impl_->schemas_[id].nullable_ : std::nullopt;
}

std::optional<bool> LayerSchema::fieldRequired(simfil::SchemaId id, std::string_view field) const
{
    if (impl_->valid(id)) {
        auto const& fields = impl_->schemas_[id].required_;
        if (auto found = fields.find(field); found != fields.end()) {
            return found->second;
        }
    }
    return std::nullopt;
}

bool LayerSchema::reachabilityComplete(simfil::SchemaId id) const
{
    return impl_->valid(id) && impl_->schemas_[id].finalized_ && impl_->schemas_[id].complete_;
}

simfil::SchemaId
LayerSchema::attributeQuerySchema(std::string_view featureType, simfil::SchemaId attributeSchema)
    const
{
    auto found = impl_->attributeQueryRoots_.find({featureSchema(featureType), attributeSchema});
    return found == impl_->attributeQueryRoots_.end() ? simfil::NoSchemaId : found->second;
}

bool LayerSchema::canHaveField(simfil::SchemaId schemaId, std::string_view fieldName) const
{
    return impl_->canHaveField(schemaId, fieldName);
}

bool LayerSchema::canHaveEnumSymbol(simfil::SchemaId schemaId, std::string_view symbolName) const
{
    return impl_->canHaveEnumSymbol(schemaId, symbolName);
}

std::span<const std::string> LayerSchema::directFields(simfil::SchemaId schemaId) const
{
    return impl_->directFields(schemaId);
}

std::span<const std::string> LayerSchema::nestedFields(simfil::SchemaId schemaId) const
{
    return impl_->nestedFields(schemaId);
}

std::span<const std::string> LayerSchema::nestedEnumSymbols(simfil::SchemaId schemaId) const
{
    return impl_->nestedEnumSymbols(schemaId);
}

std::span<const std::string> LayerSchema::directEnumSymbols(simfil::SchemaId schemaId) const
{
    return impl_->directEnumSymbols(schemaId);
}

std::string_view LayerSchema::attributeTypeCode(simfil::SchemaId schemaId) const
{
    return impl_->attributeTypeCode(schemaId);
}

std::vector<std::string> LayerSchema::constantTypeNames(
    simfil::SchemaId schemaId,
    std::string_view symbolName) const
{
    return impl_->constantTypeNames(schemaId, symbolName);
}

void LayerSchema::forEachDirectField(
    simfil::SchemaId schemaId,
    const std::function<void(std::string_view, std::span<const simfil::SchemaId>)>& fn) const
{
    impl_->forEachDirectField(schemaId, fn);
}

void LayerSchema::forEachElementSchema(
    simfil::SchemaId schemaId,
    const std::function<void(simfil::SchemaId)>& fn) const
{
    impl_->forEachElementSchema(schemaId, fn);
}

simfil::SchemaId LayerSchema::featureSchema(std::string_view featureType) const
{
    return schemaId(featureKey(featureType));
}

simfil::SchemaId LayerSchema::featurePropertiesSchema(std::string_view featureType) const
{
    return schemaId(featurePropertiesKey(featureType));
}

simfil::SchemaId LayerSchema::attributeLayerMapSchema(std::string_view featureType) const
{
    return schemaId(attributeLayerMapKey(featureType));
}

simfil::SchemaId LayerSchema::childSchema(
    simfil::SchemaId parent,
    std::string_view fieldName,
    std::optional<simfil::Schema::Kind> preferredKind) const
{
    return impl_->childSchema(parent, fieldName, preferredKind);
}

LayerSchema::PathOwner LayerSchema::ownerForPath(
    std::string_view featureType,
    simfil::SchemaId rootSchema,
    std::span<const std::string> fieldPath) const
{
    auto currentSchema = rootSchema;
    if (!impl_->valid(currentSchema)) {
        return {};
    }

    auto attributeOwner = impl_->uniqueAttributeOwner(rootSchema, featureType);
    auto currentLayerName = std::optional<std::string>{};
    auto enteredAttributeBranch =
        impl_->metaType(rootSchema) == "AttributeLayerMap" ||
        impl_->metaType(rootSchema) == "AttributeContainer" ||
        impl_->metaType(rootSchema) == "Attribute";

    for (auto const& fieldName : fieldPath) {
        if (fieldName.empty()) {
            continue;
        }

        auto const parentSchema = currentSchema;
        auto const parentMetaType = impl_->metaType(parentSchema);
        auto pendingAttributeName = std::optional<std::string>{};

        if (parentMetaType == "AttributeLayerMap") {
            // This edge selects the concrete attribute layer, but not yet a
            // concrete attribute within that layer.
            enteredAttributeBranch = true;
            currentLayerName = fieldName;
        }
        else if (parentMetaType == "AttributeContainer") {
            // This edge selects the attribute object inside the active layer.
            enteredAttributeBranch = true;
            pendingAttributeName = fieldName;
        }

        currentSchema = impl_->childSchema(currentSchema, fieldName, std::nullopt);
        if (currentSchema == simfil::NoSchemaId) {
            return {};
        }

        auto const currentMetaType = impl_->metaType(currentSchema);
        if (currentMetaType == "AttributeLayerMap" || currentMetaType == "AttributeContainer") {
            enteredAttributeBranch = true;
        }
        if (currentMetaType == "Attribute") {
            enteredAttributeBranch = true;
            if (auto owner = impl_->uniqueAttributeOwner(currentSchema, featureType)) {
                attributeOwner = *owner;
                continue;
            }

            // If the schema id is intentionally shared, the path edge still
            // contains enough context to identify the selected attribute.
            auto attributeName = std::string(impl_->attributeTypeCode(currentSchema));
            if (attributeName.empty() && pendingAttributeName) {
                attributeName = *pendingAttributeName;
            }
            if (currentLayerName && !attributeName.empty()) {
                attributeOwner = AttributePathOwner{
                    std::string(featureType),
                    *currentLayerName,
                    std::move(attributeName),
                    currentSchema};
            }
        }
        else if (auto owner = impl_->uniqueAttributeOwner(currentSchema, featureType)) {
            attributeOwner = *owner;
        }
    }

    if (attributeOwner) {
        return {PathOwnerKind::Attribute, *attributeOwner};
    }

    if (enteredAttributeBranch) {
        return {};
    }

    if (rootSchema == featureSchema(featureType) || rootSchema == featurePropertiesSchema(featureType)) {
        return {PathOwnerKind::Feature, {}};
    }

    return {};
}

std::vector<LayerSchema::NamedSchemaPath> LayerSchema::scalarFieldPathsForAttribute(
    simfil::SchemaId rootSchema,
    std::string_view attributeTypeCode) const
{
    return impl_->scalarFieldPathsForAttribute(rootSchema, attributeTypeCode);
}

std::vector<std::string> LayerSchema::featureTypes() const
{
    std::vector<std::string> result;
    for (auto const& entry : impl_->entriesById_) {
        constexpr std::string_view prefix = "Feature:";
        if (entry.metaType_ == "Feature" && entry.key_.starts_with(prefix)) {
            result.push_back(entry.key_.substr(prefix.size()));
        }
    }
    std::ranges::sort(result);
    auto duplicates = std::ranges::unique(result);
    result.erase(duplicates.begin(), duplicates.end());
    return result;
}

std::vector<LayerSchema::AttributePathOwner> LayerSchema::attributeScopes() const
{
    std::vector<AttributePathOwner> result;
    std::set<std::tuple<std::string, std::string, std::string>> seen;
    for (auto const& schema : impl_->schemas_) {
        for (auto const& owner : schema.attributeOwners_) {
            auto key = std::make_tuple(
                owner.featureType_,
                owner.attributeLayerName_,
                owner.attributeName_);
            if (seen.insert(std::move(key)).second) {
                result.push_back(owner);
            }
        }
    }
    std::ranges::sort(result, {}, [](auto const& owner) {
        return std::tie(owner.featureType_, owner.attributeLayerName_, owner.attributeName_);
    });
    return result;
}

tl::expected<LayerSchema::SearchQueryNormalization, simfil::Error> LayerSchema::normalizeSearchQuery(
    std::string_view query,
    SearchQueryRequestedScope requestedScope) const
{
    SearchQueryNormalization result;
    result.originalQuery_ = std::string(query);
    result.normalizedQuery_ = trimQuery(query);
    result.requestedScope_ = requestedScope;
    result.concreteScope_ = requestedScope == SearchQueryRequestedScope::Attribute
        ? SearchQueryConcreteScope::Attribute
        : SearchQueryConcreteScope::Feature;

    if (result.normalizedQuery_.empty()) {
        return result;
    }

    // Step 1: parse exact whole-query shorthands through SIMFIL, then compile
    // the original expression against every feature root with SIMFIL schema
    // rewrites enabled. `standaloneQuerySymbol` is deliberately AST-based; it
    // only recognizes a whole-query field/string expression and does not scan
    // arbitrary source terms.
    auto strings = std::make_shared<StringPool>("SearchQueryNormalizationSymbol");
    auto env = makeEnvironment(strings);
    auto standaloneSymbol = simfil::standaloneQuerySymbol(*env, result.normalizedQuery_);
    if (!standaloneSymbol) {
        return tl::unexpected(standaloneSymbol.error());
    }

    // The schema-aware compile keeps SIMFIL's generic rewrite engine as the
    // source of truth:
    // - `**.field` becomes WildcardFieldExpr with schema-pruned paths.
    // - Attribute type-code operands can become scalar attribute value paths.
    // - Enum constants can become `exact.path == "ENUM"` AST comparisons.
    // The normalizer consumes referencedSchemaPaths from that rewritten AST;
    // it does not tokenize or term-scan the query to infer post-processing.
    std::vector<AttributeQueryReference> attributeReferences;
    std::set<std::string> seenReferenceScopes;
    bool hasFeatureOwnedTerm = false;
    for (auto const& featureType : featureTypes()) {
        auto analysis = analyzeFeatureQueryAst(*this, result.normalizedQuery_, featureType);
        if (!analysis) {
            return tl::unexpected(analysis.error());
        }
        if (result.compiledAstDebug_.empty()) {
            result.compiledAstDebug_ = analysis->astDebug;
        }
        hasFeatureOwnedTerm = hasFeatureOwnedTerm || analysis->hasFeatureOwnedReference;
        for (auto const& reference : analysis->attributeReferences) {
            auto scopeKey = attributeOwnerKey(reference.owner);
            if (seenReferenceScopes.insert(scopeKey).second) {
                result.attributeScopes_.push_back(reference.owner);
            }
            attributeReferences.push_back(reference);
        }
    }

    // Whole-query attribute type-codes (`WARNING_SIGN`) are valid even when no
    // field path exists below the feature root. Resolve those from the
    // registry's attribute index, but only for exact AST symbols.
    auto const standaloneAttributeScopes = *standaloneSymbol
        ? attributeScopesForStandaloneSymbol(*this, **standaloneSymbol)
        : std::vector<AttributePathOwner>{};
    bool const hasStandaloneAttributeSymbol = !standaloneAttributeScopes.empty();
    for (auto const& scope : standaloneAttributeScopes) {
        if (seenReferenceScopes.insert(attributeOwnerKey(scope)).second) {
            result.attributeScopes_.push_back(scope);
        }
    }

    // Step 2: choose concrete scope. Auto becomes attribute scope from
    // attribute-owned AST references. Explicit feature-owned references keep
    // mixed queries in feature scope, but unresolved/dynamic terms do not
    // cancel a proven attribute scope. This keeps schema-generated wildcard
    // and enum rewrites useful instead of falling back to feature scope just
    // because not every intermediate SIMFIL node has a concrete source path.
    if (hasFeatureOwnedTerm && requestedScope == SearchQueryRequestedScope::Auto && !hasStandaloneAttributeSymbol) {
        result.attributeScopes_.clear();
    }
    if (requestedScope == SearchQueryRequestedScope::Attribute && result.attributeScopes_.empty()) {
        result.attributeScopes_ = attributeScopes();
    }
    result.attributeScopeCandidateCount_ = result.attributeScopes_.size();

    auto const shouldUseAttributeScope = requestedScope == SearchQueryRequestedScope::Attribute
        || (requestedScope == SearchQueryRequestedScope::Auto && !result.attributeScopes_.empty());

    if (result.attributeScopeCandidateCount_ > kMaxNormalizedAttributeScopes) {
        result.rewriteSuppressed_ = true;
        result.rewriteSuppressionReason_ = fmt::format(
            "Attribute query rewrite suppressed: {} candidate scopes exceed the limit of {}.",
            result.attributeScopeCandidateCount_,
            kMaxNormalizedAttributeScopes);
        result.attributeScopes_.clear();
        result.concreteScope_ = shouldUseAttributeScope
            ? SearchQueryConcreteScope::Attribute
            : SearchQueryConcreteScope::Feature;
        if (result.concreteScope_ == SearchQueryConcreteScope::Attribute) {
            if (auto genericQuery = genericAttributeRootQuery(result.normalizedQuery_, attributeReferences);
                !genericQuery.empty()) {
                result.normalizedQuery_ = std::move(genericQuery);
            }
        }
        return result;
    }

    if (shouldUseAttributeScope) {
        result.concreteScope_ = SearchQueryConcreteScope::Attribute;
    }

    if (result.concreteScope_ == SearchQueryConcreteScope::Feature) {
        return result;
    }

    std::set<std::string> seenFeatureTypes;
    for (auto const& scope : result.attributeScopes_) {
        if (seenFeatureTypes.insert(scope.featureType_).second) {
            result.matchedFeatureTypes_.push_back(scope.featureType_);
        }
    }

    if (result.attributeScopes_.empty()) {
        return result;
    }

    // Step 3: generate one guarded attribute-root branch per matched
    // attribute context. The branch guard selects the concrete mapget
    // attribute overlay (`$feature.typeId`, `$layer`, `$name`). The branch
    // body is then produced from schema-AST references:
    // - explicit feature-root paths are replaced by their attribute-root
    //   suffix using AST source locations;
    // - generated enum comparisons are emitted as `suffix == "ENUM"` because
    //   their AST path is not present as source text in the original query;
    // - recursive wildcard-field references stay untouched, so SIMFIL can
    //   still compile them against the concrete attribute root schema.
    std::vector<std::string> branches;
    branches.reserve(result.attributeScopes_.size());
    for (auto const& scope : result.attributeScopes_) {
        auto guard = attributeScopeGuard(scope);
        auto const guardOnlyForExactTypeCode = std::ranges::any_of(
            standaloneAttributeScopes,
            [&](auto const& standaloneScope) {
                return sameAttributeOwner(standaloneScope, scope);
            });
        std::vector<SourceRewrite> rewrites;
        bool guardOnlyForAstIdentity = false;
        if (!guardOnlyForExactTypeCode) {
            std::vector<std::string> generatedPredicates;
            for (auto const& reference : attributeReferences) {
                if (!sameAttributeOwner(reference.owner, scope)) {
                    continue;
                }
                auto replacement = attributeRootPathForFeaturePath(reference.expressionPath, scope);
                if (!replacement) {
                    continue;
                }

                auto const coversWholeQuery = sourceRangeCoversWholeQuery(result.normalizedQuery_, reference.location);
                if (*replacement == "true" && coversWholeQuery) {
                    guardOnlyForAstIdentity = true;
                    continue;
                }

                if (auto predicate = generatedAttributeRootPredicate(reference, scope, result.normalizedQuery_)) {
                    generatedPredicates.push_back(std::move(*predicate));
                    continue;
                }

                auto emittedReplacement = *replacement;
                if (reference.equalsStringLiteral && coversWholeQuery) {
                    emittedReplacement += " == ";
                    emittedReplacement += simfilStringLiteral(*reference.equalsStringLiteral);
                }
                rewrites.push_back({
                    reference.location.offset,
                    reference.location.size,
                    std::move(emittedReplacement)});
            }
            if (!generatedPredicates.empty() && rewrites.empty() && !guardOnlyForAstIdentity) {
                auto generatedBody = joinOr(std::move(generatedPredicates));
                branches.push_back(std::move(guard) + " and " + parenthesized(std::move(generatedBody)));
                continue;
            }
        }
        auto const guardOnly = guardOnlyForExactTypeCode || guardOnlyForAstIdentity;
        auto body = guardOnly
            ? std::string{}
            : applySourceRewrites(result.normalizedQuery_, std::move(rewrites));
        branches.push_back(guardOnly
            ? std::move(guard)
            : std::move(guard) + " and " + parenthesized(std::move(body)));
    }
    result.normalizedQuery_ = joinOr(std::move(branches));
    return result;
}

void installLayerSchema(
    simfil::Environment& env,
    std::shared_ptr<LayerSchema const> registry,
    std::shared_ptr<simfil::StringPool const> strings)
{
    installLayerSchemaImpl(env, std::move(registry), std::move(strings), false);
}

void installCompletionLayerSchema(
    simfil::Environment& env,
    std::shared_ptr<LayerSchema const> registry,
    std::shared_ptr<simfil::StringPool> strings)
{
    installLayerSchemaImpl(env, std::move(registry), std::move(strings), true);
}

void installLayerSchemaImpl(
    simfil::Environment& env,
    std::shared_ptr<LayerSchema const> registry,
    std::shared_ptr<simfil::StringPool const> strings,
    bool materializeSchemaStrings)
{
    auto schemas = std::make_shared<std::map<simfil::SchemaId, std::unique_ptr<BoundSchema>>>();
    env.querySchemaCallback = [registry = std::move(registry),
                               strings = std::move(strings),
                               schemas = std::move(schemas),
                               materializeSchemaStrings](simfil::SchemaId schemaId) {
        if (!registry || schemaId == simfil::NoSchemaId) {
            return static_cast<simfil::Schema const*>(nullptr);
        }
        auto [it, inserted] = schemas->try_emplace(schemaId);
        if (inserted) {
            it->second = std::make_unique<BoundSchema>(registry, strings, schemaId, materializeSchemaStrings);
        }
        return static_cast<simfil::Schema const*>(it->second.get());
    };
}

} // namespace mapget
