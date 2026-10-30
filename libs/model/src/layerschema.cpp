#include "mapget/model/layerschema.h"
#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <tuple>
#include "mapget/model/mapschema.h"
#include "mapget/model/simfilutil.h"
#include "simfil/simfil.h"

namespace mapget
{
namespace
{
using Kind = simfil::Schema::Kind;
using Composition = simfil::Schema::Composition;
constexpr size_t kMaxNormalizedAttributeScopes = 8;
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
    return std::ranges::all_of(
        value.begin() + 1,
        value.end(),
        [](char ch)
        {
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

/** Convert one compile-local SchemaPath into field names. Array markers are ignored for
 * source-level path rewrites. */
std::optional<std::vector<std::string>>
schemaPathFieldNames(simfil::Environment& env, simfil::SchemaPath const& path)
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
std::optional<std::vector<std::string>>
schemaPathExpressionSegments(simfil::Environment& env, simfil::SchemaPath const& path)
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
    return lhs.featureType_ == rhs.featureType_ &&
        lhs.attributeLayerName_ == rhs.attributeLayerName_ &&
        lhs.attributeName_ == rhs.attributeName_;
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
    if (!reference.equalsStringLiteral ||
        (!reference.viaWildcard && reference.location.size != 0 &&
         !sourceRangeCoversWholeQuery(query, reference.location)))
    {
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
    std::ranges::sort(rewrites, {}, [](auto const& rewrite) { return rewrite.offset; });

    std::vector<SourceRewrite> nonOverlapping;
    uint32_t previousEnd = 0;
    for (auto const& rewrite : rewrites) {
        if (rewrite.size == 0 || rewrite.offset < previousEnd ||
            rewrite.offset + rewrite.size > query.size())
        {
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
            std::ranges::transform(
                name,
                name.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
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
        std::string_view("sum")};
    return std::ranges::any_of(
        enclosingCalls,
        [](std::string const& name)
        { return std::ranges::find(aggregateFunctions, name) != aggregateFunctions.end(); });
}

/** Attribute scope/style inference must ignore references inside feature-level aggregate calls. */
bool referenceIsInferenceEligible(std::string_view query, simfil::SourceLocation location)
{
    return !sourceRangeIsInsideAggregateCall(query, location);
}

/** Return all attribute contexts whose mapget type-code matches one exact AST symbol. */
std::vector<LayerSchema::AttributePathOwner>
attributeScopesForStandaloneSymbol(LayerSchema const& registry, std::string_view symbol)
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
    auto registryPtr = LayerSchema::Ptr(registry);
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
    result.hasDynamicReference = references->hasBroadWildcardAccess ||
        references->hasDynamicAccess || references->hasUnresolvedAccess;

    std::set<std::tuple<
        std::string,
        std::string,
        std::string,
        uint32_t,
        uint32_t,
        std::optional<std::string>>>
        seenReferences;
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
            result.attributeReferences.push_back(
                {owner.attribute_,
                 std::move(*fieldPath),
                 std::move(*expressionPath),
                 reference.location,
                 reference.viaWildcard,
                 reference.equalsStringLiteral});
        }
    }
    return result;
}

}  // namespace

namespace
{
/** SIMFIL schema adapter that resolves StringIds without inserting schema field names. */
class BoundSchema final : public simfil::Schema
{
public:
    BoundSchema(
        LayerSchema::Ptr registry,
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

    auto numericDomain() const -> std::optional<NumericDomain> override
    {
        return registry_->numericDomain(id_);
    }
    auto bitWidth() const -> uint8_t override { return registry_->bitWidth(id_); }
    void forEachNamedValue(std::function<void(NamedValue const&)> const& fn) const override
    {
        registry_->forEachNamedValue(id_, fn);
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
        auto name = strings_->resolve(canonicalField(field));
        return name ? registry_->fieldRequired(id_, *name) : std::nullopt;
    }

    /** Match Feature::get's attributes alias while keeping field enumeration canonical. */
    auto canonicalField(simfil::StringId field) const -> simfil::StringId override
    {
        if (field == StringPool::AttributesStr &&
            simfil::Schema::kindNameId(kind()) ==
                simfil::Schema::kindNameId(LayerSchema::FeatureKind))
        {
            auto fields = registry_->directFields(id_);
            if (std::ranges::find(fields, "attributes") == fields.end() &&
                std::ranges::find(fields, "properties") != fields.end())
                return StringPool::PropertiesStr;
        }
        return field;
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
    auto
    hasDirectEnumSymbol(std::string_view symbol, simfil::StringPool const&) const -> bool override
    {
        auto names = registry_->directEnumSymbols(id_);
        return std::ranges::find(names, symbol) != names.end();
    }

    /** Return the overlay-name predicate for a matching attribute root. */
    auto symbolEqualityPaths(
        simfil::StringId symbolId,
        const std::function<const simfil::Schema*(simfil::SchemaId)>&) const
        -> std::vector<simfil::SchemaPath> override
    {
        if (!registry_ || !strings_) {
            return {};
        }

        auto symbolName = strings_->resolve(symbolId);
        auto typeCode = registry_->attributeTypeCode(id_);
        if (typeCode.empty() || !symbolName || typeCode != *symbolName) {
            return {};
        }

        // Only an owned attribute-query overlay has the runtime $name member.
        // A matching label on a feature or raw attribute payload is insufficient.
        auto nameId = strings_->get("$name");
        auto feature = registry_->childSchema(id_, "$feature");
        if (!nameId || !feature || !registry_->childSchema(id_, "$name")) {
            return {};
        }
        for (auto const& type : registry_->featureTypes()) {
            if (registry_->featureSchema(type) != feature)
                continue;
            auto owner = registry_->ownerForPath(type, id_, {});
            if (owner.kind_ == LayerSchema::PathOwnerKind::Attribute &&
                owner.attribute_.attributeName_ == typeCode)
            {
                return {simfil::SchemaPath{{simfil::SchemaPathSegment::Kind::Field, nameId}}};
            }
        }
        return {};
    }

    /** Return mapget attribute type-code scalar shorthand paths. */
    auto scalarFieldPathsForSymbol(
        simfil::StringId symbolId,
        const std::function<const simfil::Schema*(simfil::SchemaId)>&) const
        -> std::vector<simfil::SchemaPath> override
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
        const std::function<void(simfil::StringId, std::span<const simfil::SchemaId>)>& fn) const
        -> void override
    {
        if (!registry_ || !strings_) {
            return;
        }

        auto mutableStrings = std::const_pointer_cast<simfil::StringPool>(strings_);
        registry_->forEachDirectField(
            id_,
            [&](std::string_view fieldName, std::span<const simfil::SchemaId> schemas)
            {
                auto fieldId = mutableStrings->get(fieldName);
                if (fieldId != simfil::StringPool::Empty) {
                    fn(fieldId, schemas);
                }
            });
    }

    /** Visit possible array element schemas. */
    auto
    forEachElementSchema(const std::function<void(simfil::SchemaId)>& fn) const -> void override
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
        std::span<const std::string_view> names,
        simfil::StringPool& strings,
        std::vector<simfil::StringId>& ids) -> void
    {
        ids.reserve(names.size());
        for (auto const& name : names) {
            auto id = strings.emplace(name);
            if (!id)
                throw std::length_error(id.error().message);
            ids.push_back(*id);
        }
    }

    /** Runtime pruning calls canHaveField directly, so no recursive StringId cache is built here.
     */
    auto collectNestedFields(
        const std::function<Schema*(simfil::SchemaId)>&,
        SchemaIdStack&,
        std::vector<simfil::StringId>&) const -> void override
    {
    }

    LayerSchema::Ptr registry_;
    std::shared_ptr<simfil::StringPool const> strings_;
    simfil::SchemaId id_ = simfil::NoSchemaId;
    std::vector<simfil::StringId> directFields_;
    std::vector<simfil::StringId> nestedFields_;
    std::vector<simfil::StringId> nestedEnumSymbols_;
    std::vector<simfil::StringId> directEnumSymbols_;
};

}  // namespace

void installLayerSchemaImpl(
    simfil::Environment& env,
    LayerSchema::Ptr registry,
    std::shared_ptr<simfil::StringPool const> strings,
    bool materializeSchemaStrings);

tl::expected<LayerSchema::SearchQueryNormalization, simfil::Error>
LayerSchema::normalizeSearchQuery(std::string_view query, SearchQueryRequestedScope requestedScope)
    const
{
    SearchQueryNormalization result;
    result.originalQuery_ = std::string(query);
    result.normalizedQuery_ = trimQuery(query);
    result.requestedScope_ = requestedScope;
    result.concreteScope_ = requestedScope == SearchQueryRequestedScope::Attribute ?
        SearchQueryConcreteScope::Attribute :
        SearchQueryConcreteScope::Feature;

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
    auto const standaloneAttributeScopes = *standaloneSymbol ?
        attributeScopesForStandaloneSymbol(*this, **standaloneSymbol) :
        std::vector<AttributePathOwner>{};
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
    if (hasFeatureOwnedTerm && requestedScope == SearchQueryRequestedScope::Auto &&
        !hasStandaloneAttributeSymbol)
    {
        result.attributeScopes_.clear();
    }
    if (requestedScope == SearchQueryRequestedScope::Attribute && result.attributeScopes_.empty()) {
        result.attributeScopes_ = attributeScopes();
    }
    result.attributeScopeCandidateCount_ = result.attributeScopes_.size();

    auto const shouldUseAttributeScope = requestedScope == SearchQueryRequestedScope::Attribute ||
        (requestedScope == SearchQueryRequestedScope::Auto && !result.attributeScopes_.empty());

    if (result.attributeScopeCandidateCount_ > kMaxNormalizedAttributeScopes) {
        result.rewriteSuppressed_ = true;
        result.rewriteSuppressionReason_ = fmt::format(
            "Attribute query rewrite suppressed: {} candidate scopes exceed the limit of {}.",
            result.attributeScopeCandidateCount_,
            kMaxNormalizedAttributeScopes);
        result.attributeScopes_.clear();
        result.concreteScope_ = shouldUseAttributeScope ?
            SearchQueryConcreteScope::Attribute :
            SearchQueryConcreteScope::Feature;
        if (result.concreteScope_ == SearchQueryConcreteScope::Attribute) {
            if (auto genericQuery =
                    genericAttributeRootQuery(result.normalizedQuery_, attributeReferences);
                !genericQuery.empty())
            {
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
            [&](auto const& standaloneScope)
            { return sameAttributeOwner(standaloneScope, scope); });
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

                auto const coversWholeQuery =
                    sourceRangeCoversWholeQuery(result.normalizedQuery_, reference.location);
                if (*replacement == "true" && coversWholeQuery) {
                    guardOnlyForAstIdentity = true;
                    continue;
                }

                if (auto predicate =
                        generatedAttributeRootPredicate(reference, scope, result.normalizedQuery_))
                {
                    generatedPredicates.push_back(std::move(*predicate));
                    continue;
                }

                auto emittedReplacement = *replacement;
                if (reference.equalsStringLiteral && coversWholeQuery) {
                    emittedReplacement += " == ";
                    emittedReplacement += simfilStringLiteral(*reference.equalsStringLiteral);
                }
                rewrites.push_back(
                    {reference.location.offset,
                     reference.location.size,
                     std::move(emittedReplacement)});
            }
            if (!generatedPredicates.empty() && rewrites.empty() && !guardOnlyForAstIdentity) {
                auto generatedBody = joinOr(std::move(generatedPredicates));
                branches.push_back(
                    std::move(guard) + " and " + parenthesized(std::move(generatedBody)));
                continue;
            }
        }
        auto const guardOnly = guardOnlyForExactTypeCode || guardOnlyForAstIdentity;
        auto body = guardOnly ?
            std::string{} :
            applySourceRewrites(result.normalizedQuery_, std::move(rewrites));
        branches.push_back(
            guardOnly ? std::move(guard) :
                        std::move(guard) + " and " + parenthesized(std::move(body)));
    }
    result.normalizedQuery_ = joinOr(std::move(branches));
    return result;
}

void installLayerSchema(
    simfil::Environment& env,
    LayerSchema::Ptr registry,
    std::shared_ptr<simfil::StringPool const> strings)
{
    installLayerSchemaImpl(env, std::move(registry), std::move(strings), false);
}

void installCompletionLayerSchema(
    simfil::Environment& env,
    LayerSchema::Ptr registry,
    std::shared_ptr<simfil::StringPool> strings)
{
    installLayerSchemaImpl(env, std::move(registry), std::move(strings), true);
}

void installLayerSchemaImpl(
    simfil::Environment& env,
    LayerSchema::Ptr registry,
    std::shared_ptr<simfil::StringPool const> strings,
    bool materializeSchemaStrings)
{
    auto schemas = std::make_shared<std::map<simfil::SchemaId, std::unique_ptr<BoundSchema>>>();
    env.querySchemaCallback =
        [registry = std::move(registry),
         strings = std::move(strings),
         schemas = std::move(schemas),
         materializeSchemaStrings](simfil::SchemaId schemaId)
    {
        if (!registry || schemaId == simfil::NoSchemaId) {
            return static_cast<simfil::Schema const*>(nullptr);
        }
        auto [it, inserted] = schemas->try_emplace(schemaId);
        if (inserted) {
            it->second = std::make_unique<
                BoundSchema>(registry, strings, schemaId, materializeSchemaStrings);
        }
        return static_cast<simfil::Schema const*>(it->second.get());
    };
}

}  // namespace mapget
