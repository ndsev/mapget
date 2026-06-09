#include "mapget/model/schemaregistry.h"
#include "mapget/model/simfilutil.h"
#include "mapget/model/stringpool.h"

#include <algorithm>
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
    return schema.contains("properties") || hasType(schema, "object");
}

/** Return whether this schema branch should be treated as an array schema. */
bool isArraySchema(nlohmann::json const& schema)
{
    return schema.contains("items") || hasType(schema, "array");
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
    return fieldName == "_sourceData"
        || fieldName == "conditions"
        || fieldName == "properties"
        || fieldName == "references"
        || fieldName == "validity";
}

/** Collect string-valued const/enum entries from a JSON Schema branch. */
std::vector<std::string> stringEnumSymbols(nlohmann::json const& schema)
{
    std::vector<std::string> symbols;
    if (auto constIt = schema.find("const"); constIt != schema.end() && constIt->is_string()) {
        symbols.push_back(constIt->get<std::string>());
    }

    if (auto enumIt = schema.find("enum"); enumIt != schema.end() && enumIt->is_array()) {
        for (auto const& value : *enumIt) {
            if (value.is_string()) {
                symbols.push_back(value.get<std::string>());
            }
        }
    }

    std::ranges::sort(symbols);
    auto duplicates = std::ranges::unique(symbols);
    symbols.erase(duplicates.begin(), duplicates.end());
    return symbols;
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

/** Return whether a oneOf object/array wrapper represents a mapget multimap view. */
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
        throw std::runtime_error("SchemaRegistry only supports local JSON Schema references.");
    }
    auto pointer = ref.substr(1);
    return root.at(nlohmann::json::json_pointer(pointer));
}

/** Convert a local JSON Schema reference into a canonical registry pointer. */
std::string refToPointer(std::string const& ref)
{
    if (ref.empty() || ref.front() != '#') {
        throw std::runtime_error("SchemaRegistry only supports local JSON Schema references.");
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
std::string attributeScopeGuard(SchemaRegistry::AttributePathOwner const& scope)
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
    SchemaRegistry::AttributePathOwner owner;
    std::vector<std::string> fieldPath;
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

/** Stable identity for one layer-local attribute owner. */
std::string attributeOwnerKey(SchemaRegistry::AttributePathOwner const& owner)
{
    return owner.featureType_ + "\n" + owner.attributeLayerName_ + "\n" + owner.attributeName_;
}

/** Return whether two attribute owner records address the same layer-local attribute context. */
bool sameAttributeOwner(
    SchemaRegistry::AttributePathOwner const& lhs,
    SchemaRegistry::AttributePathOwner const& rhs)
{
    return lhs.featureType_ == rhs.featureType_
        && lhs.attributeLayerName_ == rhs.attributeLayerName_
        && lhs.attributeName_ == rhs.attributeName_;
}

/** Return the attribute-root suffix of a feature-root path owned by the supplied attribute. */
std::optional<std::string> attributeRootPathForFeaturePath(
    std::vector<std::string> const& fieldPath,
    SchemaRegistry::AttributePathOwner const& owner)
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
        if (!result.empty()) {
            result += ".";
        }
        if (isIdentifier(*it)) {
            result += *it;
        }
        else {
            result += "[" + simfilStringLiteral(*it) + "]";
        }
    }
    return result.empty() ? std::optional<std::string>{"true"} : std::optional<std::string>{result};
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
bool sourceRangeCoversWholeQuery(std::string const& query, simfil::SourceLocation location)
{
    if (location.size == 0 || location.offset + location.size > query.size()) {
        return false;
    }
    return location.offset == 0 && location.size == query.size();
}

/** Return all attribute contexts whose mapget type-code matches one exact AST symbol. */
std::vector<SchemaRegistry::AttributePathOwner> attributeScopesForStandaloneSymbol(
    SchemaRegistry const& registry,
    std::string_view symbol)
{
    std::vector<SchemaRegistry::AttributePathOwner> result;
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
    SchemaRegistry const& registry,
    std::string_view query,
    std::string_view featureType)
{
    auto strings = std::make_shared<StringPool>("SearchQueryNormalization");
    auto env = makeEnvironment(strings);
    auto registryPtr = std::shared_ptr<SchemaRegistry const>(&registry, [](SchemaRegistry const*) {});
    installCompletionSchemaRegistry(*env, std::move(registryPtr), strings);

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
        if (!fieldPath) {
            result.hasDynamicReference = true;
            continue;
        }
        auto owner = registry.ownerForPath(featureType, featureSchemaId, *fieldPath);
        if (owner.kind_ == SchemaRegistry::PathOwnerKind::Feature) {
            result.hasFeatureOwnedReference = true;
            continue;
        }
        if (owner.kind_ != SchemaRegistry::PathOwnerKind::Attribute) {
            result.hasDynamicReference = true;
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
                reference.location,
                reference.viaWildcard,
                reference.equalsStringLiteral});
        }
    }
    return result;
}

} // namespace

struct SchemaRegistry::Impl
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

    [[nodiscard]] bool valid(simfil::SchemaId id) const
    {
        return id != simfil::NoSchemaId && id < schemas_.size();
    }

    [[nodiscard]] Kind kind(simfil::SchemaId id) const
    {
        return valid(id) ? schemas_[id].kind_ : Kind::Object;
    }

    [[nodiscard]] std::string_view metaType(simfil::SchemaId id) const
    {
        return valid(id) ? entriesById_[id].metaType_ : std::string_view{};
    }

    [[nodiscard]] std::string_view attributeTypeCode(simfil::SchemaId id) const
    {
        return valid(id) ? schemas_[id].attributeTypeCode_ : std::string_view{};
    }

    simfil::SchemaId allocate(Kind kind, std::string key, std::string pointer, std::string metaType)
    {
        if (schemas_.size() > simfil::MaxSchemaId) {
            throw std::runtime_error("SchemaRegistry exhausted the uint16 SchemaId domain.");
        }

        auto id = static_cast<simfil::SchemaId>(schemas_.size());
        Entry entry{id, key, pointer, std::move(metaType)};
        LogicalSchema schema;
        schema.id_ = id;
        schema.kind_ = kind;
        schema.entry_ = entry;
        schemas_.push_back(std::move(schema));
        entriesById_.push_back(std::move(entry));
        registerKey(key, id);
        registerKey(pointer, id);
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

        if (metaType != "Attribute") {
            return;
        }

        auto attributeName = metadataString(metadata, "attributeTypeCode");
        schemas_[id].attributeType_ = metadataString(metadata, "attributeType");
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
        fields.emplace_back(fieldName);
    }

    void addEnumSymbol(simfil::SchemaId parent, std::string_view symbolName)
    {
        if (!valid(parent)) {
            return;
        }
        auto& symbols = schemas_[parent].directEnumSymbols_;
        symbols.emplace_back(symbolName);
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

    void finalizeAll()
    {
        for (size_t id = 1; id < schemas_.size(); ++id) {
            finalize(static_cast<simfil::SchemaId>(id));
        }
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
    }

    [[nodiscard]] bool canHaveField(simfil::SchemaId id, std::string_view fieldName)
    {
        if (!valid(id)) {
            return true;
        }
        finalize(id);
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
        if (schema.kind_ == Kind::Array) {
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
            if (!preferredKind || kind(id) == *preferredKind) {
                return id;
            }
        }
        return fieldIt->second.empty() ? simfil::NoSchemaId : fieldIt->second.front();
    }

    [[nodiscard]] std::optional<SchemaRegistry::NamedSchemaPath> firstScalarFieldPath(
        simfil::SchemaId id,
        bool skipRootAttributeMetadataFields = false) const
    {
        std::vector<simfil::SchemaId> visited;
        SchemaRegistry::NamedSchemaPath current;
        return firstScalarFieldPath(id, visited, current, skipRootAttributeMetadataFields, true);
    }

    [[nodiscard]] std::optional<SchemaRegistry::NamedSchemaPath> firstScalarFieldPath(
        simfil::SchemaId id,
        std::vector<simfil::SchemaId>& visited,
        SchemaRegistry::NamedSchemaPath& current,
        bool skipRootAttributeMetadataFields,
        bool isRoot) const
    {
        if (!valid(id) || std::ranges::find(visited, id) != visited.end()) {
            return std::nullopt;
        }

        auto const& schema = schemas_[id];
        if (schema.kind_ == Kind::Value) {
            return current;
        }

        visited.push_back(id);
        if (schema.kind_ == Kind::Object) {
            for (auto const& fieldName : schema.directFields_) {
                if (isRoot
                    && skipRootAttributeMetadataFields
                    && isAttributeScalarShorthandMetadataField(fieldName)) {
                    continue;
                }
                current.push_back({simfil::SchemaPathSegment::Kind::Field, fieldName});
                auto childIt = schema.childSchemas_.find(fieldName);
                if (childIt == schema.childSchemas_.end() || childIt->second.empty()) {
                    auto result = current;
                    current.pop_back();
                    visited.pop_back();
                    return result;
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

    [[nodiscard]] std::vector<SchemaRegistry::NamedSchemaPath> scalarFieldPathsForAttribute(
        simfil::SchemaId rootSchema,
        std::string_view attributeTypeCode) const
    {
        std::vector<SchemaRegistry::NamedSchemaPath> paths;
        std::vector<simfil::SchemaId> visited;
        SchemaRegistry::NamedSchemaPath current;
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
        SchemaRegistry::NamedSchemaPath& current,
        std::vector<SchemaRegistry::NamedSchemaPath>& paths) const
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
        if (schema.kind_ == Kind::Object) {
            for (auto const& fieldName : schema.directFields_) {
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
        else if (schema.kind_ == Kind::Array) {
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

/** Recursive JSON Schema compiler used by SchemaRegistry construction. */
class RegistryBuilder
{
public:
    RegistryBuilder(SchemaRegistry::Impl& registry, nlohmann::json const& root)
        : registry_(registry), root_(root)
    {
    }

    void buildAll()
    {
        build(root_, "#", {}, std::nullopt);
        registry_.finalizeAll();
    }

private:
    simfil::SchemaId build(
        nlohmann::json const& schema,
        std::string pointer,
        BuildContext context,
        std::optional<Kind> preferredKind)
    {
        if (schema.is_boolean() || !schema.is_object()) {
            return simfil::NoSchemaId;
        }

        auto refIt = schema.find("$ref");
        if (refIt != schema.end() && refIt->is_string()) {
            auto ref = refIt->get<std::string>();
            return build(resolveLocalRef(root_, ref), refToPointer(ref), std::move(context), preferredKind);
        }

        if (auto combiner = combinerKey(schema); !combiner.empty()) {
            return buildCombined(schema, std::string(combiner), std::move(pointer), std::move(context), preferredKind);
        }

        auto* metadata = mapgetMetadata(schema);
        auto metaType = metadataString(metadata, "metaType");
        auto featureType = metadataString(metadata, "featureType");
        if (!featureType.empty()) {
            context.featureType_ = std::move(featureType);
        }

        auto const key = annotatedKey(schema, pointer, context);
        auto const memoKey = contextMemoKey(pointer, preferredKind, context);
        if (auto memoIt = memo_.find(memoKey); memoIt != memo_.end()) {
            registry_.registerKey(key, memoIt->second);
            registry_.registerSchemaMetadata(memoIt->second, schema, context, metaType);
            return memoIt->second;
        }

        if (isArraySchema(schema) && (!preferredKind || *preferredKind == Kind::Array) && !isObjectSchema(schema)) {
            return buildArray(schema, std::move(pointer), std::move(context), std::move(key), std::move(metaType), memoKey);
        }
        if (isObjectSchema(schema) && (!preferredKind || *preferredKind == Kind::Object)) {
            return buildObject(schema, std::move(pointer), std::move(context), std::move(key), std::move(metaType), memoKey);
        }
        if (isArraySchema(schema) && (!preferredKind || *preferredKind == Kind::Array)) {
            return buildArray(schema, std::move(pointer), std::move(context), std::move(key), std::move(metaType), memoKey);
        }
        if (isValueSchema(schema) && (!preferredKind || *preferredKind == Kind::Value)) {
            return buildValue(schema, std::move(pointer), std::move(context), std::move(key), std::move(metaType), memoKey);
        }
        return simfil::NoSchemaId;
    }

    simfil::SchemaId buildCombined(
        nlohmann::json const& schema,
        std::string const& combiner,
        std::string pointer,
        BuildContext context,
        std::optional<Kind> preferredKind)
    {
        auto const& branches = schema.at(combiner);
        std::vector<simfil::SchemaId> branchIds;
        branchIds.reserve(branches.size());
        for (size_t i = 0; i < branches.size(); ++i) {
            auto branchPointer = pointer + "/" + combiner + "/" + std::to_string(i);
            branchIds.push_back(build(branches[i], branchPointer, context, preferredKind));
        }

        auto choose = [&](std::optional<Kind> kind) {
            for (auto id : branchIds) {
                if (id != simfil::NoSchemaId && (!kind || registry_.kind(id) == *kind)) {
                    return id;
                }
            }
            return simfil::NoSchemaId;
        };

        if (auto selected = choose(preferredKind); selected != simfil::NoSchemaId) {
            registerCombinedAliases(schema, pointer, context, selected);
            return selected;
        }
        if (auto selected = choose(Kind::Object); selected != simfil::NoSchemaId) {
            registerCombinedAliases(schema, pointer, context, selected);
            return selected;
        }
        if (auto selected = choose(Kind::Array); selected != simfil::NoSchemaId) {
            registerCombinedAliases(schema, pointer, context, selected);
            return selected;
        }
        if (auto selected = choose(Kind::Value); selected != simfil::NoSchemaId) {
            registerCombinedAliases(schema, pointer, context, selected);
            return selected;
        }
        return simfil::NoSchemaId;
    }

    void registerCombinedAliases(
        nlohmann::json const& schema,
        std::string const& pointer,
        BuildContext const& context,
        simfil::SchemaId selected)
    {
        registry_.registerKey(pointer, selected);
        auto key = annotatedKey(schema, pointer, context);
        registry_.registerKey(key, selected);
        registry_.registerSchemaMetadata(
            selected,
            schema,
            context,
            metadataString(mapgetMetadata(schema), "metaType"));
    }

    simfil::SchemaId buildObject(
        nlohmann::json const& schema,
        std::string pointer,
        BuildContext context,
        std::string key,
        std::string metaType,
        std::string const& memoKey)
    {
        auto id = registry_.allocate(
            Kind::Object,
            std::move(key),
            pointer,
            metaType);
        registry_.registerSchemaMetadata(id, schema, context, metaType);
        memo_[memoKey] = id;

        auto propertiesIt = schema.find("properties");
        if (propertiesIt == schema.end() || !propertiesIt->is_object()) {
            return id;
        }

        for (auto const& [fieldName, childSchemaJson] : propertiesIt->items()) {
            registry_.addDirectField(id, fieldName);

            auto childContext = context;
            if (metaType == "AttributeLayerMap") {
                childContext.attributeLayerName_ = fieldName;
            }

            auto const childPointer = pointer + "/properties/" + pointerToken(fieldName);
            auto addChildSchema = [&](std::optional<Kind> preferredKind) {
                auto childId = build(
                    childSchemaJson,
                    childPointer,
                    childContext,
                    preferredKind);
                if (childId != simfil::NoSchemaId) {
                    registry_.addChild(id, fieldName, childId);
                }
            };

            if (isMapgetMultimap(childSchemaJson)) {
                // Simfil sees the logical object view. The array branch is only
                // a JSON serialization detail for duplicate field names.
                addChildSchema(Kind::Object);
            }
            else {
                addChildSchema(std::nullopt);
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
        auto id = registry_.allocate(
            Kind::Array,
            std::move(key),
            pointer,
            metaType);
        registry_.registerSchemaMetadata(id, schema, context, metaType);
        memo_[memoKey] = id;

        auto itemsIt = schema.find("items");
        if (itemsIt == schema.end()) {
            return id;
        }

        auto childId = build(*itemsIt, pointer + "/items", std::move(context), std::nullopt);
        if (childId != simfil::NoSchemaId) {
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
        auto id = registry_.allocate(
            Kind::Value,
            std::move(key),
            std::move(pointer),
            metaType);
        registry_.registerSchemaMetadata(id, schema, context, metaType);
        memo_[memoKey] = id;

        auto symbols = stringEnumSymbols(schema);
        registry_.addEnumSymbols(id, symbols);
        return id;
    }

    SchemaRegistry::Impl& registry_;
    nlohmann::json const& root_;
    std::map<std::string, simfil::SchemaId> memo_;
};

/** SIMFIL schema adapter that resolves StringIds without inserting schema field names. */
class BoundSchema final : public simfil::Schema
{
public:
    BoundSchema(
        std::shared_ptr<SchemaRegistry const> registry,
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
    auto kind() const -> Kind override
    {
        return registry_ ? registry_->kind(id_) : Kind::Object;
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
                auto fieldId = mutableStrings->emplace(segment.field_);
                if (!fieldId) {
                    complete = false;
                    break;
                }
                path.push_back({simfil::SchemaPathSegment::Kind::Field, *fieldId});
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

    std::shared_ptr<SchemaRegistry const> registry_;
    std::shared_ptr<simfil::StringPool const> strings_;
    simfil::SchemaId id_ = simfil::NoSchemaId;
    std::vector<simfil::StringId> directFields_;
    std::vector<simfil::StringId> nestedFields_;
    std::vector<simfil::StringId> nestedEnumSymbols_;
    std::vector<simfil::StringId> directEnumSymbols_;
};

} // namespace

void installSchemaRegistryImpl(
    simfil::Environment& env,
    std::shared_ptr<SchemaRegistry const> registry,
    std::shared_ptr<simfil::StringPool const> strings,
    bool materializeSchemaStrings);

SchemaRegistry::SchemaRegistry(nlohmann::json const& schema)
    : impl_(std::make_shared<Impl>())
{
    if (!schema.is_null()) {
        RegistryBuilder(*impl_, schema).buildAll();
    }
}

std::shared_ptr<SchemaRegistry> SchemaRegistry::fromJson(nlohmann::json const& schema)
{
    if (schema.is_null()) {
        return nullptr;
    }
    return std::make_shared<SchemaRegistry>(schema);
}

SchemaRegistry::Entry const* SchemaRegistry::getSchema(std::string_view keyOrFeatureType) const
{
    auto exact = impl_->idsByKey_.find(keyOrFeatureType);
    auto id = exact == impl_->idsByKey_.end() ? featureSchema(keyOrFeatureType) : exact->second;
    if (id == simfil::NoSchemaId || id >= impl_->entriesById_.size()) {
        return nullptr;
    }
    return &impl_->entriesById_[id];
}

simfil::SchemaId SchemaRegistry::schemaId(std::string_view key) const
{
    auto it = impl_->idsByKey_.find(key);
    if (it == impl_->idsByKey_.end()) {
        return simfil::NoSchemaId;
    }
    return it->second;
}

simfil::Schema::Kind SchemaRegistry::kind(simfil::SchemaId schemaId) const
{
    return impl_->kind(schemaId);
}

bool SchemaRegistry::canHaveField(simfil::SchemaId schemaId, std::string_view fieldName) const
{
    return impl_->canHaveField(schemaId, fieldName);
}

bool SchemaRegistry::canHaveEnumSymbol(simfil::SchemaId schemaId, std::string_view symbolName) const
{
    return impl_->canHaveEnumSymbol(schemaId, symbolName);
}

std::span<const std::string> SchemaRegistry::directFields(simfil::SchemaId schemaId) const
{
    return impl_->directFields(schemaId);
}

std::span<const std::string> SchemaRegistry::nestedFields(simfil::SchemaId schemaId) const
{
    return impl_->nestedFields(schemaId);
}

std::span<const std::string> SchemaRegistry::nestedEnumSymbols(simfil::SchemaId schemaId) const
{
    return impl_->nestedEnumSymbols(schemaId);
}

std::span<const std::string> SchemaRegistry::directEnumSymbols(simfil::SchemaId schemaId) const
{
    return impl_->directEnumSymbols(schemaId);
}

std::string_view SchemaRegistry::attributeTypeCode(simfil::SchemaId schemaId) const
{
    return impl_->attributeTypeCode(schemaId);
}

std::vector<std::string> SchemaRegistry::constantTypeNames(
    simfil::SchemaId schemaId,
    std::string_view symbolName) const
{
    return impl_->constantTypeNames(schemaId, symbolName);
}

void SchemaRegistry::forEachDirectField(
    simfil::SchemaId schemaId,
    const std::function<void(std::string_view, std::span<const simfil::SchemaId>)>& fn) const
{
    impl_->forEachDirectField(schemaId, fn);
}

void SchemaRegistry::forEachElementSchema(
    simfil::SchemaId schemaId,
    const std::function<void(simfil::SchemaId)>& fn) const
{
    impl_->forEachElementSchema(schemaId, fn);
}

simfil::SchemaId SchemaRegistry::featureSchema(std::string_view featureType) const
{
    return schemaId(featureKey(featureType));
}

simfil::SchemaId SchemaRegistry::featurePropertiesSchema(std::string_view featureType) const
{
    return schemaId(featurePropertiesKey(featureType));
}

simfil::SchemaId SchemaRegistry::attributeLayerMapSchema(std::string_view featureType) const
{
    return schemaId(attributeLayerMapKey(featureType));
}

simfil::SchemaId SchemaRegistry::childSchema(
    simfil::SchemaId parent,
    std::string_view fieldName,
    std::optional<simfil::Schema::Kind> preferredKind) const
{
    return impl_->childSchema(parent, fieldName, preferredKind);
}

SchemaRegistry::PathOwner SchemaRegistry::ownerForPath(
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

std::vector<SchemaRegistry::NamedSchemaPath> SchemaRegistry::scalarFieldPathsForAttribute(
    simfil::SchemaId rootSchema,
    std::string_view attributeTypeCode) const
{
    return impl_->scalarFieldPathsForAttribute(rootSchema, attributeTypeCode);
}

std::vector<std::string> SchemaRegistry::featureTypes() const
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

std::vector<SchemaRegistry::AttributePathOwner> SchemaRegistry::attributeScopes() const
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

tl::expected<SchemaRegistry::SearchQueryNormalization, simfil::Error> SchemaRegistry::normalizeSearchQuery(
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

    if (requestedScope == SearchQueryRequestedScope::Attribute
        || (requestedScope == SearchQueryRequestedScope::Auto && !result.attributeScopes_.empty())) {
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
                auto replacement = attributeRootPathForFeaturePath(reference.fieldPath, scope);
                if (!replacement) {
                    continue;
                }

                auto const coversWholeQuery = sourceRangeCoversWholeQuery(result.normalizedQuery_, reference.location);
                if (*replacement == "true" && coversWholeQuery) {
                    guardOnlyForAstIdentity = true;
                    continue;
                }

                if (reference.equalsStringLiteral && (reference.viaWildcard || reference.location.size == 0)) {
                    generatedPredicates.push_back(
                        *replacement + " == " + simfilStringLiteral(*reference.equalsStringLiteral));
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

void installSchemaRegistry(
    simfil::Environment& env,
    std::shared_ptr<SchemaRegistry const> registry,
    std::shared_ptr<simfil::StringPool const> strings)
{
    installSchemaRegistryImpl(env, std::move(registry), std::move(strings), false);
}

void installCompletionSchemaRegistry(
    simfil::Environment& env,
    std::shared_ptr<SchemaRegistry const> registry,
    std::shared_ptr<simfil::StringPool> strings)
{
    installSchemaRegistryImpl(env, std::move(registry), std::move(strings), true);
}

void installSchemaRegistryImpl(
    simfil::Environment& env,
    std::shared_ptr<SchemaRegistry const> registry,
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
