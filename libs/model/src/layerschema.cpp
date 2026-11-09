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
simfil::SchemaType LayerSchema::rootType() const
{
    return mapSchema()->schemaType(rootSchema());
}

namespace
{
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

/** Select a named field at any depth, including names requiring bracket syntax. */
std::string recursiveField(std::string_view name)
{
    return isIdentifier(name) ? "**." + std::string(name) : "**[" + simfilStringLiteral(name) + "]";
}

/**
 * Find ordinary field predicates for a declared enum or named value. Visit
 * graph edges rather than expanding every root-to-leaf path: a recursive
 * payload still needs just `**.warningSign == "SPEED_LIMIT"`. Named unions
 * shadow their physical alternatives, as they do in completion.
 *
 * The current declaring type groups fields belonging to one candidate. It
 * never appears in the resulting expression or its cross-map identity.
 */
std::map<simfil::TypeId, std::set<std::string>> symbolFieldPredicates(
    simfil::SchemaType root,
    std::string_view symbol,
    std::span<simfil::TypeId const> excluded = {})
{
    // No field denotes a scalar root. An empty field name is still a real
    // dictionary entry and must produce a bracket selector, not `_`.
    using Position = std::tuple<simfil::TypeId, std::optional<simfil::StringId>, bool>;
    std::vector<Position> pending{{root.id(), std::nullopt, false}};
    std::set<Position> visited;
    std::map<simfil::TypeId, std::set<std::string>> result;
    while (!pending.empty()) {
        auto position = pending.back();
        pending.pop_back();
        auto [id, field, array] = position;
        if (!id || std::ranges::find(excluded, id) != excluded.end() ||
            !visited.insert(position).second)
            continue;
        auto type = root.pool()->schemaType(id);
        auto named = type.namedValue(symbol);
        auto declares = named || type.hasDirectEnumSymbol(symbol);
        if (declares) {
            auto selector = field ?
                recursiveField(*root.pool()->dictionary().resolve(*field)) :
                "_";
            // A scalar enum array may be nested or recursive. Enumerating its
            // descendants keeps the saved query independent of array depth.
            if (array)
                selector += ".**";
            if (type.isBitmask())
                result[id].insert("hasFlag(" + selector + ", " + simfilStringLiteral(symbol) + ")");
            else if (!named || std::holds_alternative<std::string_view>(named->value))
                result[id].insert(
                    selector + " == " +
                    simfilStringLiteral(named ? std::get<std::string_view>(named->value) : symbol));
            else if (isIdentifier(symbol))
                result[id].insert(selector + " == " + std::string(symbol));
        }
        else {
            for (auto child : type.alternatives())
                pending.emplace_back(child, field, array);
        }
        type.forEachField(
            [&](simfil::SchemaField const& child)
            {
                if (child.role() != simfil::SchemaType::FieldRole::Context)
                    for (auto target : child.types())
                        pending.emplace_back(target, child.nameId(), false);
            });
        for (auto child : type.elements())
            pending.emplace_back(child, field, true);
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
schemaPathFieldNames(simfil::StringPool const& strings, simfil::SchemaPath const& path)
{
    std::vector<std::string> result;
    for (auto const& segment : path) {
        if (segment.kind != simfil::SchemaPathSegment::Kind::Field) {
            continue;
        }
        auto fieldName = strings.resolve(segment.field);
        if (!fieldName) {
            return std::nullopt;
        }
        result.emplace_back(*fieldName);
    }
    return result;
}

/** Convert one compile-local SchemaPath into SIMFIL path segments, preserving array wildcards. */
std::optional<std::vector<std::string>>
schemaPathExpressionSegments(simfil::StringPool const& strings, simfil::SchemaPath const& path)
{
    std::vector<std::string> result;
    for (auto const& segment : path) {
        if (segment.kind == simfil::SchemaPathSegment::Kind::ArrayElement) {
            result.emplace_back("*");
            continue;
        }
        auto fieldName = strings.resolve(segment.field);
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

/** Recognize a source range nested in one of the known aggregate calls. */
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

/** Return attribute contexts whose declared name or type code matches one exact AST symbol. */
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
    env->setSchema(registry.rootType());

    auto const featureSchemaId = registry.featureSchema(featureType);
    auto graph = registry.mapSchema();
    auto featureSchema = graph->schemaType(featureSchemaId);
    auto ast = simfil::compile(
        *env,
        query,
        simfil::CompileOptions{
            .any = false,
            .rewriteMode = simfil::RewriteMode::Schema,
            .rootSchema = featureSchema});
    if (!ast) {
        return tl::unexpected(ast.error());
    }

    FeatureQueryAnalysis result;
    result.astDebug = (*ast)->expr().toString();
    auto references = simfil::referencedSchemaPaths(*env, **ast, featureSchema);
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
        auto fieldPath = schemaPathFieldNames(graph->dictionary(), reference.path);
        auto expressionPath = schemaPathExpressionSegments(graph->dictionary(), reference.path);
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

tl::expected<std::vector<LayerSchema::SearchInterpretation>, simfil::Error>
LayerSchema::searchInterpretations(std::string_view query, SearchQueryRequestedScope requested)
    const
{
    auto strings = std::make_shared<StringPool>("SearchInterpretations");
    auto env = makeEnvironment(strings);
    env->setSchema(rootType());
    auto standalone = simfil::standaloneQuerySymbol(*env, query);
    if (!standalone)
        return tl::unexpected(standalone.error());
    if (!*standalone) {
        auto symbols = simfil::predicateQuerySymbols(*env, query);
        if (!symbols)
            return tl::unexpected(symbols.error());
        if (!symbols->empty()) {
            // Bind predicate-position names before wrappers or boolean
            // operators can turn them into scalar shorthand. Ordinary RHS
            // literals and explicit paths remain in their operand contexts.
            bool featureContext = requested == SearchQueryRequestedScope::Feature;
            for (auto const& feature : featureTypes()) {
                auto analyzed = analyzeFeatureQueryAst(*this, query, feature);
                if (!analyzed)
                    return tl::unexpected(analyzed.error());
                featureContext |= analyzed->hasFeatureOwnedReference;
            }
            auto scope = featureContext ? SearchQueryRequestedScope::Feature : requested;
            std::vector<std::vector<SearchInterpretation>> meanings;
            for (auto const& symbol : *symbols) {
                auto choices = searchInterpretations(simfilStringLiteral(symbol.name), scope);
                if (!choices)
                    return tl::unexpected(choices.error());
                if (choices->empty())
                    choices->push_back(
                        {.id_ = "absent:" + symbol.name,
                         .label_ = symbol.name + " is not declared here",
                         .kind_ = "expression",
                         .expression_ = "false",
                         .query_ = "false"});
                featureContext |= std::ranges::any_of(
                    *choices,
                    [](auto const& choice) {
                        return choice.scope_ == SearchQueryConcreteScope::Feature &&
                            choice.kind_ != "expression";
                    });
                meanings.push_back(std::move(*choices));
            }
            if (featureContext && scope != SearchQueryRequestedScope::Feature)
                return searchInterpretations(query, SearchQueryRequestedScope::Feature);
            std::vector<SearchInterpretation> result;
            std::vector<size_t> selected(meanings.size());
            while (true) {
                if (result.size() >= 64)
                    return tl::unexpected(simfil::Error{
                        simfil::Error::InvalidArguments,
                        "Too many possible interpretations. Qualify predicate names with explicit "
                        "fields."});
                SearchInterpretation interpretation;
                interpretation.kind_ = "expression";
                interpretation.scope_ = featureContext ?
                    SearchQueryConcreteScope::Feature :
                    SearchQueryConcreteScope::Attribute;
                interpretation.featureTypes_ = featureTypes();
                std::vector<SourceRewrite> rewrites;
                std::vector<SourceRewrite> expressions;
                for (size_t i = 0; i < meanings.size(); ++i) {
                    auto const& meaning = meanings[i][selected[i]];
                    interpretation.id_ += (i ? ";" : "") + meaning.id_;
                    interpretation.label_ += (i ? "; " : "") + meaning.label_;
                    interpretation.attributeScopes_.insert(
                        interpretation.attributeScopes_.end(),
                        meaning.attributeScopes_.begin(),
                        meaning.attributeScopes_.end());
                    rewrites.push_back(
                        {(*symbols)[i].location.offset,
                         (*symbols)[i].location.size,
                         parenthesized(meaning.query_)});
                    expressions.push_back(
                        {(*symbols)[i].location.offset,
                         (*symbols)[i].location.size,
                         parenthesized(meaning.expression_)});
                }
                interpretation.query_ =
                    applySourceRewrites(std::string(query), std::move(rewrites));
                interpretation.expression_ =
                    applySourceRewrites(std::string(query), std::move(expressions));
                result.push_back(std::move(interpretation));
                size_t index = 0;
                for (; index < selected.size(); ++index) {
                    if (++selected[index] < meanings[index].size())
                        break;
                    selected[index] = 0;
                }
                if (index == selected.size())
                    break;
            }
            return result;
        }
        auto normalized = normalizeSearchQuery(query, requested);
        if (!normalized)
            return tl::unexpected(normalized.error());
        if (normalized->rewriteSuppressed_)
            return tl::unexpected(simfil::Error{
                simfil::Error::InvalidArguments,
                normalized->rewriteSuppressionReason_});
        return std::vector<SearchInterpretation>{
            {.id_ = "expression",
             .label_ = "Explicit expression",
             .kind_ = "expression",
             .expression_ = trimQuery(query),
             .query_ = normalized->normalizedQuery_,
             .scope_ = normalized->concreteScope_,
             .attributeScopes_ = normalized->attributeScopes_,
             .featureTypes_ = normalized->matchedFeatureTypes_}};
    }

    return symbolInterpretations(**standalone, requested, true);
}

std::vector<LayerSchema::SearchInterpretation>
LayerSchema::enumSearchInterpretations(std::string const& symbol, SearchQueryRequestedScope scope)
    const
{
    return symbolInterpretations(symbol, scope, false);
}

std::vector<LayerSchema::SearchInterpretation> LayerSchema::symbolInterpretations(
    std::string const& symbol,
    SearchQueryRequestedScope requested,
    bool includeFields) const
{
    auto graph = mapSchema();
    auto const& schemaStrings = graph->dictionary();
    std::map<std::string, SearchInterpretation> meanings;
    std::map<std::string, std::set<std::string>> expressions;
    std::map<simfil::TypeId, std::map<simfil::TypeId, std::set<std::string>>> enumPredicates;

    // Group declarations while inspecting this graph. Only the final ordinary
    // expression identifies a choice across maps; versioned type names and
    // graph-local IDs are evidence for discovery, not saved query conditions.
    auto contribute =
        [&](std::string key,
            std::string kind,
            std::string expression,
            SearchQueryConcreteScope scope,
            std::string const& feature,
            AttributePathOwner const* owner = nullptr)
    {
        auto& meaning = meanings[key];
        meaning.kind_ = std::move(kind);
        meaning.symbol_ = symbol;
        meaning.scope_ = scope;
        expressions[key].insert(std::move(expression));
        if (owner &&
            std::ranges::none_of(
                meaning.attributeScopes_,
                [&](auto const& existing) { return sameAttributeOwner(existing, *owner); }))
            meaning.attributeScopes_.push_back(*owner);
        if (std::ranges::find(meaning.featureTypes_, feature) == meaning.featureTypes_.end())
            meaning.featureTypes_.push_back(feature);
    };

    auto attributeScope = requested == SearchQueryRequestedScope::Feature ?
        SearchQueryConcreteScope::Feature :
        SearchQueryConcreteScope::Attribute;
    for (auto const& owner : attributeScopes()) {
        if (includeFields &&
            (owner.attributeName_ == symbol || attributeTypeCode(owner.attributeSchema_) == symbol))
            contribute(
                "attribute:" + owner.attributeName_,
                "attribute",
                recursiveField(owner.attributeName_),
                attributeScope,
                owner.featureType_,
                &owner);

        auto root = graph->schemaType(owner.attributeSchema_);
        auto stringId = schemaStrings.get(symbol);
        if (includeFields && stringId && !root.fieldPaths(stringId).empty())
            contribute(
                "field:attribute:" + symbol,
                "field",
                recursiveField(symbol),
                attributeScope,
                owner.featureType_,
                &owner);

        auto [found, inserted] = enumPredicates.try_emplace(root.id());
        if (inserted)
            found->second = symbolFieldPredicates(root, symbol);
        for (auto const& [type, predicates] : found->second) {
            for (auto const& predicate : predicates)
                contribute(
                    "enum:attribute:" + std::to_string(type),
                    "enum",
                    predicate,
                    attributeScope,
                    owner.featureType_,
                    &owner);
        }
    }

    auto symbolId = schemaStrings.get(symbol);
    for (auto const& feature : featureTypes()) {
        // A forced attribute search cannot evaluate feature-owned payloads.
        // Their names must not appear as choices in that scope either.
        if (requested == SearchQueryRequestedScope::Attribute)
            break;
        auto root = featureSchema(feature);
        auto typeView = graph->schemaType(root);
        auto excluded = std::array{attributeLayerMapSchema(feature)};
        for (auto const& [type, predicates] : symbolFieldPredicates(typeView, symbol, excluded)) {
            for (auto const& predicate : predicates)
                contribute(
                    "enum:feature:" + std::to_string(type),
                    "enum",
                    predicate,
                    SearchQueryConcreteScope::Feature,
                    feature);
        }
        for (auto const& path : includeFields&& symbolId ?
                 typeView.fieldPaths(symbolId) :
                 std::vector<simfil::SchemaPath>{})
        {
            auto names = schemaPathFieldNames(schemaStrings, path);
            if (names && ownerForPath(feature, root, *names).kind_ == PathOwnerKind::Feature) {
                contribute(
                    "field:feature:" + symbol,
                    "field",
                    recursiveField(symbol),
                    SearchQueryConcreteScope::Feature,
                    feature);
                break;
            }
        }
    }

    for (auto& [key, meaning] : meanings) {
        auto const& predicates = expressions.at(key);
        meaning.expression_ = joinOr({predicates.begin(), predicates.end()});
        if (meaning.kind_ == "enum" && meaning.scope_ == SearchQueryConcreteScope::Attribute) {
            // A homonymous field in a different attribute can represent a
            // genuinely different choice. Qualify only those collisions, using
            // ordinary owner names. Unambiguous warningSign fields need none
            // of the layer/type restrictions used during their discovery.
            std::vector<AttributePathOwner> conflicts;
            for (auto const& [otherKey, other] : meanings) {
                if (otherKey == key || other.kind_ != "enum" || other.scope_ != meaning.scope_)
                    continue;
                auto const& alternatives = expressions.at(otherKey);
                if (std::ranges::any_of(
                        predicates,
                        [&](auto const& predicate) { return alternatives.contains(predicate); }))
                    conflicts.insert(
                        conflicts.end(),
                        other.attributeScopes_.begin(),
                        other.attributeScopes_.end());
            }
            if (!conflicts.empty()) {
                std::set<std::string> guards;
                for (auto const& owner : meaning.attributeScopes_) {
                    auto guard = "$name == " + simfilStringLiteral(owner.attributeName_);
                    if (std::ranges::any_of(
                            conflicts,
                            [&](auto const& other)
                            { return other.attributeName_ == owner.attributeName_; }))
                    {
                        guard += " and $layer == " + simfilStringLiteral(owner.attributeLayerName_);
                        if (std::ranges::any_of(
                                conflicts,
                                [&](auto const& other)
                                {
                                    return other.attributeName_ == owner.attributeName_ &&
                                        other.attributeLayerName_ == owner.attributeLayerName_;
                                }))
                            guard += " and $feature.typeId == " +
                                simfilStringLiteral(owner.featureType_);
                    }
                    guards.insert(std::move(guard));
                }
                meaning.expression_ = parenthesized(joinOr({guards.begin(), guards.end()})) +
                    " and " + parenthesized(meaning.expression_);
            }
        }
        meaning.query_ = meaning.expression_;
        meaning.label_ = symbol + (meaning.kind_ == "enum" ? " enum value" : " field");
        if (meaning.kind_ == "attribute") {
            meaning.label_ = meaning.attributeScopes_.front().attributeName_ + " attribute";
            if (meaning.scope_ == SearchQueryConcreteScope::Attribute)
                meaning.query_ = "$name == " +
                    simfilStringLiteral(meaning.attributeScopes_.front().attributeName_);
        }
        meaning.id_ = meaning.kind_ + ":" +
            (meaning.scope_ == SearchQueryConcreteScope::Attribute ? "attribute:" : "feature:") +
            meaning.expression_;
    }
    std::vector<SearchInterpretation> result;
    for (auto& [_, meaning] : meanings) {
        auto existing = std::ranges::find(result, meaning.id_, &SearchInterpretation::id_);
        if (existing == result.end()) {
            result.push_back(std::move(meaning));
        }
        else {
            for (auto const& owner : meaning.attributeScopes_)
                if (std::ranges::none_of(
                        existing->attributeScopes_,
                        [&](auto const& other) { return sameAttributeOwner(owner, other); }))
                    existing->attributeScopes_.push_back(owner);
            for (auto const& feature : meaning.featureTypes_)
                if (std::ranges::find(existing->featureTypes_, feature) ==
                    existing->featureTypes_.end())
                    existing->featureTypes_.push_back(feature);
        }
    }
    return result;
}

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
    // Owner inference consumes referencedSchemaPaths from that rewritten AST;
    // source scanning only excludes known aggregate call contexts.
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

    // A recursive field predicate already works on every matching attribute.
    // Owner discovery helps choose its initial style; it must not expand that
    // predicate into a frozen list of feature/layer/name combinations.
    auto portableWildcard = !attributeReferences.empty() &&
        std::ranges::all_of(
            attributeReferences,
            [](auto const& reference)
            {
                auto suffix =
                    attributeRootPathForFeaturePath(reference.expressionPath, reference.owner);
                return reference.viaWildcard && suffix && *suffix != "true";
            });
    if (portableWildcard || attributeReferences.empty())
        return result;

    // Explicit feature-root paths must be rebased when returning attribute
    // matches. Retain only selectors actually present in the input path. For
    // example, `**.WARNING_SIGN` requires its name, while a path through
    // properties.layer.Guidance also requires Guidance. Feature types are
    // inferred execution context, not an additional authored condition.
    std::vector<std::string> branches;
    branches.reserve(result.attributeScopes_.size());
    for (auto const& scope : result.attributeScopes_) {
        auto guard = "$name == " + simfilStringLiteral(scope.attributeName_);
        if (std::ranges::any_of(
                attributeReferences,
                [&](auto const& reference)
                { return sameAttributeOwner(reference.owner, scope) && !reference.viaWildcard; }))
            guard = "$layer == " + simfilStringLiteral(scope.attributeLayerName_) + " and " + guard;
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
    std::ranges::sort(branches);
    branches.erase(std::unique(branches.begin(), branches.end()), branches.end());
    result.normalizedQuery_ = joinOr(std::move(branches));
    return result;
}

}  // namespace mapget
