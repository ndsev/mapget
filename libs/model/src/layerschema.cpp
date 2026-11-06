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
                }
                interpretation.query_ =
                    applySourceRewrites(std::string(query), std::move(rewrites));
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
             .query_ = normalized->normalizedQuery_,
             .scope_ = normalized->concreteScope_,
             .attributeScopes_ = normalized->attributeScopes_,
             .featureTypes_ = normalized->matchedFeatureTypes_}};
    }

    auto const& symbol = **standalone;
    auto graph = mapSchema();
    auto const& schemaStrings = graph->dictionary();
    std::map<std::string, SearchInterpretation> meanings;
    auto contribute =
        [&](std::string id,
            std::string label,
            std::string kind,
            std::string predicate,
            AttributePathOwner const& owner)
    {
        auto [it, inserted] = meanings.try_emplace(id);
        auto& interpretation = it->second;
        if (inserted) {
            interpretation.id_ = std::move(id);
            interpretation.label_ = std::move(label);
            interpretation.kind_ = std::move(kind);
            interpretation.symbol_ = symbol;
            interpretation.scope_ = requested == SearchQueryRequestedScope::Feature ?
                SearchQueryConcreteScope::Feature :
                SearchQueryConcreteScope::Attribute;
        }
        // Feature mode retains containing-feature semantics explicitly. Each
        // attribute branch is evaluated within its owning layer/assignment.
        auto branch = parenthesized(attributeScopeGuard(owner) + " and (" + predicate + ")");
        if (interpretation.scope_ == SearchQueryConcreteScope::Feature) {
            auto path = "properties.layer[" + simfilStringLiteral(owner.attributeLayerName_) +
                "][" + simfilStringLiteral(owner.attributeName_) + "]";
            branch = "(typeId == " + simfilStringLiteral(owner.featureType_) + " and any(" + path +
                ".(" + predicate + ")))";
        }
        interpretation.query_ = interpretation.query_.empty() ?
            branch :
            interpretation.query_ + " or " + branch;
        interpretation.attributeScopes_.push_back(owner);
        if (std::ranges::find(interpretation.featureTypes_, owner.featureType_) ==
            interpretation.featureTypes_.end())
            interpretation.featureTypes_.push_back(owner.featureType_);
    };

    std::map<simfil::TypeId, std::vector<simfil::TypeId>> symbolTypes;
    auto declaringTypes = [&](simfil::TypeId root) -> std::vector<simfil::TypeId> const&
    {
        auto [it, inserted] = symbolTypes.try_emplace(root);
        if (inserted)
            it->second = graph->schemaType(root).symbolTypes(symbol);
        return it->second;
    };
    auto owners = attributeScopes();
    for (auto const& owner : owners) {
        if (owner.attributeName_ == symbol || attributeTypeCode(owner.attributeSchema_) == symbol)
            contribute(
                "attribute:" + owner.attributeName_,
                owner.attributeName_ + " attribute",
                "attribute",
                "true",
                owner);
        auto root = owner.attributeSchema_;
        // Enumerating paths is only evidence of a declared symbol. Runtime
        // hasSymbol follows actual finite data, preserving recursive schemas
        // and named-value provenance instead of serializing truncated paths.
        auto stringId = schemaStrings.get(symbol);
        auto fields = stringId ?
            graph->schemaType(root).fieldPaths(stringId) :
            std::vector<simfil::SchemaPath>{};
        if (!fields.empty())
            contribute(
                "field:" + owner.attributeName_ + ":" + symbol,
                owner.attributeName_ + "." + symbol + " field",
                "field",
                isIdentifier(symbol) ? "**." + symbol : "**[" + simfilStringLiteral(symbol) + "]",
                owner);
        for (auto type : declaringTypes(root)) {
            auto declaredName = graph->schemaType(type).typeName();
            auto name = declaredName.empty() ?
                "type-" + std::to_string(type) :
                std::string(declaredName);
            // Anonymous type numbers are graph-local; equal numbers in two
            // unrelated artifacts must never silently become one meaning.
            auto identity = declaredName.empty() ?
                std::string(mapSchema()->schemaId()) + ":" + name :
                name;
            auto id = "enum:attribute:" + identity + ":" + symbol;
            contribute(
                id,
                name + " = " + symbol,
                "enum",
                fmt::format(
                    "hasSymbol(_, {}, {}, {})",
                    simfilStringLiteral(symbol),
                    simfilStringLiteral(mapSchema()->schemaId()),
                    type),
                owner);
        }
    }

    auto symbolId = schemaStrings.get(symbol);
    for (auto const& feature : featureTypes()) {
        auto root = featureSchema(feature);
        auto typeView = graph->schemaType(root);
        auto excluded = std::array{attributeLayerMapSchema(feature)};
        for (auto type : typeView.symbolTypes(symbol, excluded)) {
            auto declaredName = graph->schemaType(type).typeName();
            auto typeLabel = declaredName.empty() ?
                "type-" + std::to_string(type) :
                std::string(declaredName);
            auto identity = declaredName.empty() ?
                std::string(graph->schemaId()) + ":" + typeLabel :
                typeLabel;
            auto id = "enum:feature:" + identity + ":" + symbol;
            auto& candidate = meanings[id];
            candidate.id_ = id;
            candidate.kind_ = "enum";
            candidate.symbol_ = symbol;
            candidate.label_ = typeLabel + " = " + symbol;
            auto predicate = fmt::format(
                "(typeId == {} and hasSymbol(_, {}, {}, {}))",
                simfilStringLiteral(feature),
                simfilStringLiteral(symbol),
                simfilStringLiteral(mapSchema()->schemaId()),
                type);
            candidate.query_ = candidate.query_.empty() ?
                predicate :
                candidate.query_ + " or " + predicate;
            candidate.featureTypes_.push_back(feature);
        }
        for (auto const& path :
             symbolId ? typeView.fieldPaths(symbolId) : std::vector<simfil::SchemaPath>{})
        {
            auto names = schemaPathFieldNames(schemaStrings, path);
            if (!names || ownerForPath(feature, root, *names).kind_ != PathOwnerKind::Feature)
                continue;
            auto& candidate = meanings["field:" + symbol];
            candidate.id_ = "field:" + symbol;
            candidate.label_ = symbol + " feature field";
            candidate.kind_ = "field";
            candidate.symbol_ = symbol;
            candidate.query_ = isIdentifier(symbol) ?
                "**." + symbol :
                "**[" + simfilStringLiteral(symbol) + "]";
            candidate.featureTypes_.push_back(feature);
            break;
        }
    }
    std::vector<SearchInterpretation> result;
    for (auto& [_, interpretation] : meanings)
        result.push_back(std::move(interpretation));
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

}  // namespace mapget
