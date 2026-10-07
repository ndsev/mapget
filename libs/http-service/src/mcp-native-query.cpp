#include "mcp-native-call.h"

#include "mapget/model/attr.h"
#include "mapget/model/attrlayer.h"
#include "mapget/model/featureid.h"
#include "mapget/model/layerschema.h"
#include "mapget/model/sourcedata.h"
#include "mapget/model/sourcedatareference.h"
#include "mapget/service/locate.h"
#include "simfil/model/schema-model.h"
#include "simfil/result.h"

#include <charconv>
#include <cmath>
#include <set>

namespace mapget::detail
{
using Json = nlohmann::json;
using simfil::ValueType;

/** Native trace capture serializes bounded samples immediately, never retaining source models. */
class McpNativeTools::Call::Trace final : public simfil::Function
{
public:
    /** Bind samples and budgets to the invocation owning this environment. */
    explicit Trace(Call& call) : call_(call) {}
    /** Keep the normal simfil signature and compilation semantics. */
    simfil::FnInfo const& ident() const override { return simfil::TraceFn::Fn.ident(); }
    /** Forward values regardless of capture, honoring both simfil and native resource limits. */
    tl::expected<simfil::Result, simfil::Error> eval(
        simfil::Context ctx,
        simfil::Value const& value,
        std::vector<simfil::ExprPtr> const& args,
        simfil::ResultFn const& result) const override
    {
        if (args.empty() || args.size() > 3)
            return tl::unexpected(simfil::Error{
                simfil::Error::InvalidArguments,
                "trace expects 1..3 arguments"});
        if (ctx.phase == simfil::Context::Compilation)
            return result(ctx, simfil::Value::undef());
        auto limit = int64_t{100};
        std::string name = args[0]->toString();
        for (size_t i = 1; i < args.size(); ++i) {
            size_t count = 0;
            auto argument = args[i]->eval(
                ctx,
                value,
                simfil::LambdaResultFn(
                    [&](simfil::Context,
                        simfil::Value const& v) -> tl::expected<simfil::Result, simfil::Error>
                    {
                        if (++count > 1 || (i == 1 && v.type != ValueType::Int) ||
                            (i == 2 && v.type != ValueType::String))
                            return tl::unexpected(simfil::Error{
                                simfil::Error::InvalidArguments,
                                "Invalid trace argument"});
                        if (i == 1)
                            limit = v.as<ValueType::Int>();
                        else {
                            auto view = std::get_if<std::string_view>(&v.value);
                            std::string_view text = view ?
                                *view :
                                std::string_view(std::get<std::string>(v.value));
                            if (text.size() > 512)
                                return tl::unexpected(simfil::Error{
                                    simfil::Error::InvalidArguments,
                                    "Trace name too long"});
                            try {
                                name = text;
                            }
                            catch (...) {
                                return simfil::Stop;
                            }
                        }
                        return simfil::Continue;
                    }));
            if (!argument)
                return argument;
            if (count != 1)
                return tl::unexpected(simfil::Error{
                    simfil::Error::InvalidArguments,
                    "Missing trace argument"});
        }
        auto const capture = call_.arguments.value("trace", false);
        auto started = std::chrono::steady_clock::now();
        if (capture &&
            (name.size() > 512 || (!call_.traces_.contains(name) && call_.traces_.size() >= 16))) {
            call_.truncate("trace_limit");
            return simfil::Stop;
        }
        if (capture && !call_.traces_.contains(name)) {
            call_.charge(6 * name.size() + 128);
            call_.traces_[name] = {
                {"calls", 0},
                {"totalUs", 0},
                {"values", Json::array()},
                {"samplesTruncated", false}};
        }
        auto forwarded = args[0]->eval(
            ctx,
            value,
            simfil::LambdaResultFn(
                [&](simfil::Context context,
                    simfil::Value const& v) -> tl::expected<simfil::Result, simfil::Error>
                {
                    try {
                        if (capture) {
                            auto& trace = call_.traces_[name];
                            auto max = limit < 0 ? size_t{100} : std::min<size_t>(limit, 100);
                            if (trace["values"].size() < max)
                                trace["values"].push_back(call_.valueJson(v));
                            else
                                trace["samplesTruncated"] = true;
                        }
                        return result(context, v);
                    }
                    catch (...) {
                        call_.truncate("trace_limit");
                        return simfil::Stop;
                    }
                }));
        if (capture && call_.traces_.contains(name)) {
            auto& trace = call_.traces_[name];
            trace["calls"] = trace["calls"].get<size_t>() + 1;
            trace["totalUs"] = trace["totalUs"].get<int64_t>() +
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count();
        }
        return forwarded;
    }

private:
    Call& call_;
};

Json McpNativeTools::Call::boundedJson(Json const& value, size_t depth)
{
    if (!step())
        throw std::length_error("work");
    if (depth > SerializationNestingLimit) {
        truncate("serialization_limit");
        throw std::length_error("depth");
    }
    charge(24);
    if (value.is_string()) {
        charge(value.get_ref<std::string const&>().size() * 6);
        return value;
    }
    if (value.is_number_unsigned() && value.get<uint64_t>() > 9007199254740991ULL)
        return {{"$mapget", "uint64"}, {"value", std::to_string(value.get<uint64_t>())}};
    if (value.is_number_integer() && !value.is_number_unsigned() &&
        (value.get<int64_t>() < -9007199254740991LL || value.get<int64_t>() > 9007199254740991LL))
        return {{"$mapget", "int64"}, {"value", std::to_string(value.get<int64_t>())}};
    if (!value.is_structured())
        return value;
    auto result = value.is_array() ? Json::array() : Json::object();
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (value.is_array())
            result.push_back(boundedJson(*it, depth + 1));
        else {
            charge(it.key().size() * 6);
            result[it.key()] = boundedJson(*it, depth + 1);
        }
    }
    // Reserved scalar tags cannot be confused with a genuine user object's field names.
    return result.is_object() && result.contains("$mapget") ?
        Json{{"$mapget", "object"}, {"value", std::move(result)}} :
        result;
}

Json McpNativeTools::Call::valueJson(simfil::Value const& value, size_t depth)
{
    if (!step())
        throw std::length_error("work");
    if (depth > SerializationNestingLimit) {
        truncate("serialization_limit");
        throw std::length_error("depth");
    }
    charge(32);
    switch (value.type) {
    case ValueType::Undef: return {{"$mapget", "undefined"}};
    case ValueType::Null: return nullptr;
    case ValueType::Bool: return value.as<ValueType::Bool>();
    case ValueType::Int: {
        auto n = value.as<ValueType::Int>();
        if (n < -9007199254740991LL || n > 9007199254740991LL)
            return {{"$mapget", "int64"}, {"value", std::to_string(n)}};
        return n;
    }
    case ValueType::Float: {
        auto n = value.as<ValueType::Float>();
        return std::isfinite(n) ?
            Json(n) :
            Json{
                {"$mapget", "nonfinite"},
                {"value",
                 std::isnan(n) ? "NaN" :
                     n > 0     ? "Infinity" :
                                 "-Infinity"}};
    }
    case ValueType::String: {
        auto view = std::get_if<std::string_view>(&value.value);
        std::string_view text = view ? *view : std::string_view(std::get<std::string>(value.value));
        charge(text.size() * 6);
        return text;
    }
    case ValueType::Bytes: {
        auto const& bytes = value.as<ValueType::Bytes>();
        charge(bytes.bytes.size() * 2);
        return {{"$mapget", "bytes"}, {"hex", bytes.toHex(false)}};
    }
    case ValueType::Array:
    case ValueType::Object: {
        auto node = value.type == ValueType::Array ?
            value.as<ValueType::Array>() :
            value.as<ValueType::Object>();
        if (!node)
            return nullptr;
        auto model =
            std::dynamic_pointer_cast<PartitionFeatureModelLayerBase const>(node->owningModel());
        if (model &&
            node->addr().column() == PartitionFeatureModelLayerBase::ColumnId::SourceDataReferences)
        {
            auto reference = model->resolve<SourceDataReferenceItem>(*node);
            return boundedJson(
                {{"layerId", reference->layerId()},
                 {"address", std::to_string(reference->address().u64())},
                 {"qualifier", reference->qualifier()}},
                depth);
        }
        auto result = value.type == ValueType::Array ? Json::array() : Json::object();
        std::set<std::string> repeated;
        for (uint32_t i = 0; i < node->size(); ++i) {
            // Empty/missing children still consume traversal work.
            if (!step())
                throw std::length_error("work");
            auto child = node->at(i);
            if (!child)
                continue;
            auto item = valueJson(simfil::Value::field(child), depth + 1);
            if (value.type == ValueType::Array) {
                result.push_back(std::move(item));
                continue;
            }
            auto id = node->keyAt(i);
            auto key = node->owningModel() ?
                node->owningModel()->lookupStringId(id) :
                env_->strings()->resolve(id);
            if (!key)
                throw std::runtime_error("unresolved model field");
            charge(key->size() * 6);
            auto text = std::string(*key);
            if (result.contains(text)) {
                if (repeated.insert(text).second)
                    result[text] = Json::array({std::move(result[text])});
                result[text].push_back(std::move(item));
                result["_multimap"] = true;
            }
            else
                result[text] = std::move(item);
        }
        if (model &&
            node->addr().column() == PartitionFeatureModelLayerBase::ColumnId::AttributeLayers) {
            auto layer = model->resolve<AttributeLayer>(*node);
            if (auto id = layer->id())
                result["id"] = boundedJson(*id, depth + 1);
        }
        return result.is_object() && result.contains("$mapget") ?
            Json{{"$mapget", "object"}, {"value", std::move(result)}} :
            result;
    }
    default:
        truncate("unsupported_value_type");
        throw std::length_error("Cannot serialize transient values");
    }
}

void McpNativeTools::Call::environment(std::shared_ptr<simfil::StringPool> strings)
{
    expressions_.clear();
    if (strings)
        strings = std::make_shared<simfil::StringPool>(*strings);
    else
        strings = std::make_shared<StringPool>("mcp-query");
    env_ = makeEnvironment(strings);
    if (layer_)
        installCompletionLayerSchema(*env_, layer_->layerSchema(), strings);
    traceFunction_ = std::make_unique<Trace>(*this);
    env_->functions["trace"] = traceFunction_.get();
}

void McpNativeTools::Call::diagnostics(simfil::Diagnostics const& data)
{
    auto messages = simfil::diagnostics(data);
    if (!messages)
        return;
    for (auto const& message : *messages) {
        if (issues_.size() >= 100) {
            truncate("diagnostic_limit");
            break;
        }
        issues_.push_back(boundedJson(
            {{"message", message.message},
             {"offset", message.location.offset},
             {"size", message.location.size}}));
    }
}

Json McpNativeTools::Call::evaluate(
    std::string const& query,
    simfil::ModelNode const& root,
    simfil::SchemaId schema,
    bool predicate)
{
    auto result = Json::array();
    if (!step())
        return result;
    auto key = std::make_tuple(query, schema, predicate);
    auto found = expressions_.find(key);
    if (found == expressions_.end()) {
        auto ast = simfil::compile(
            *env_,
            query,
            {.any = predicate, .rewriteMode = simfil::RewriteMode::Schema, .rootSchema = schema});
        if (!ast) {
            issues_.push_back(boundedJson(
                {{"phase", "compile"},
                 {"message", ast.error().message},
                 {"offset", ast.error().location.offset},
                 {"size", ast.error().location.size}}));
            truncate("query_error");
            return result;
        }
        found = expressions_.emplace(std::move(key), std::move(*ast)).first;
    }
    simfil::Diagnostics data;
    auto summary = simfil::eval(
        *env_,
        *found->second,
        root,
        simfil::LambdaResultFn(
            [&](simfil::Context,
                simfil::Value const& value) -> tl::expected<simfil::Result, simfil::Error>
            {
                try {
                    // Probe one extra value to distinguish a complete exact-limit sequence from
                    // a truncated one. Item and expression limits must not suppress geometry
                    // output.
                    if (result.size() >= limit_) {
                        truncate("expression_result_limit");
                        return simfil::Stop;
                    }
                    result.push_back(valueJson(value));
                    return simfil::Continue;
                }
                catch (...) {
                    if (incomplete_.empty())
                        truncate("serialization_failed");
                    return simfil::Stop;
                }
            }),
        {.maxResults = predicate ? size_t{2} : limit_ + 1,
         .maxWork = remainingWork_,
         .deadline = deadline_,
         .cancel = cancelled.get()},
        &data);
    if (summary) {
        remainingWork_ -= std::min(remainingWork_, summary->work);
        if (summary->reason != simfil::EvaluationSummary::Reason::Complete)
            truncate(
                summary->reason == simfil::EvaluationSummary::Reason::Timeout ?
                    "timeout" :
                    summary->reason == simfil::EvaluationSummary::Reason::Canceled ?
                    "cancelled" :
                    "evaluation_limit");
    }
    else {
        issues_.push_back(boundedJson(
            {{"phase", "evaluate"},
             {"message", summary.error().message},
             {"offset", summary.error().location.offset},
             {"size", summary.error().location.size}}));
        truncate("query_error");
    }
    if (incomplete_.empty())
        diagnostics(data);
    return result;
}

Json McpNativeTools::Call::schemaOverview(
    simfil::ModelNode const& node,
    std::set<simfil::SchemaId> const& expanded,
    size_t depth)
{
    if (!step())
        throw std::length_error("work");
    if (depth > SerializationNestingLimit) {
        truncate("serialization_limit");
        throw std::length_error("nesting");
    }
    charge(32);
    auto result = Json::object();
    auto reference = node.get(simfil::StringPool::SchemaRef);
    bool expand = false;
    if (reference) {
        auto ids = valueJson(simfil::Value::field(reference));
        if (ids.is_number_integer())
            expand = expanded.contains(ids.get<simfil::SchemaId>());
        else if (ids.is_array())
            for (auto const& id : ids)
                expand |= expanded.contains(id.get<simfil::SchemaId>());
        result["$ref"] = std::move(ids);
    }
    for (uint32_t i = 0; i < node.size(); ++i) {
        if (!step())
            throw std::length_error("work");
        auto key = node.keyAt(i);
        if (key == simfil::StringPool::SchemaRef)
            continue;
        bool fields = key == simfil::StringPool::SchemaFields;
        bool domains = key == simfil::StringPool::SchemaElements ||
            key == simfil::StringPool::SchemaAlternatives;
        // A semantic reference is intentional, not a truncated query. Scalars retain enums.
        if ((fields || domains) && !expand)
            continue;
        auto name = node.owningModel()->lookupStringId(key).value();
        charge(name.size() * 6);
        auto child = node.at(i);
        if (!fields && !domains) {
            result[name] = valueJson(simfil::Value::field(child));
            continue;
        }
        auto values = fields ? Json::object() : Json::array();
        for (uint32_t j = 0; j < child->size(); ++j) {
            auto value = schemaOverview(*child->at(j), expanded, depth + 1);
            if (fields) {
                auto field = child->owningModel()->lookupStringId(child->keyAt(j)).value();
                charge(field.size() * 6);
                values[field] = std::move(value);
            }
            else
                values.push_back(std::move(value));
        }
        result[name] = std::move(values);
    }
    return result;
}

void McpNativeTools::Call::querySchema()
{
    selectLayer();
    auto schema = layer_->layerSchema();
    if (!schema) {
        fail("unavailable", "Layer has no feature-model schema.");
        return;
    }
    environment();
    auto model = std::make_shared<simfil::SchemaModel>(
        env_->strings(),
        env_->querySchemaCallback,
        std::max(size_t{2}, remainingWork_));
    auto types = arguments.contains("featureType") ?
        std::vector<std::string>{arguments["featureType"]} :
        schema->featureTypes();
    std::vector<simfil::SchemaId> roots;
    if (arguments.contains("schemaId")) {
        if (arguments.contains("featureType"))
            throw std::invalid_argument("Choose featureType or schemaId");
        auto id = arguments["schemaId"].get<simfil::SchemaId>();
        if (!schema->hasSchema(id))
            throw std::invalid_argument("Unknown schema id");
        roots.push_back(id);
    }
    else {
        for (auto const& type : types) {
            auto id = schema->featureSchema(type);
            if (id == simfil::NoSchemaId)
                throw std::invalid_argument("Unknown feature type");
            roots.push_back(id);
        }
    }
    for (size_t i = 0; i < roots.size() && step(); ++i) {
        auto id = roots[i];
        auto root = model->root(id);
        Json values;
        if (arguments.contains("query")) {
            // Explicit queries see the whole lazy graph, never the overview's projection.
            values = evaluate(arguments["query"], *root, simfil::NoSchemaId);
        }
        else {
            std::set<simfil::SchemaId> expanded{id};
            if (simfil::Schema::kindNameId(schema->kind(id)) ==
                simfil::Schema::kindNameId(LayerSchema::FeatureKind))
            {
                // These are native Feature containers, not heuristics on producer payload names.
                auto properties = simfil::Schema::fieldSchemas(
                    id,
                    env_->querySchemaCallback,
                    StringPool::PropertiesStr);
                std::vector<simfil::SchemaId> layerMaps;
                for (auto property : properties) {
                    expanded.insert(property);
                    auto layers = simfil::Schema::fieldSchemas(
                        property,
                        env_->querySchemaCallback,
                        StringPool::LayerStr);
                    layerMaps.insert(layerMaps.end(), layers.begin(), layers.end());
                }
                std::set<simfil::SchemaId> visited;
                while (!layerMaps.empty() && step()) {
                    auto layers = layerMaps.back();
                    layerMaps.pop_back();
                    if (!visited.insert(layers).second)
                        continue;
                    expanded.insert(layers);
                    schema->forEachDirectField(
                        layers,
                        [&](auto, auto children)
                        { expanded.insert(children.begin(), children.end()); });
                    auto alternatives = schema->alternatives(layers);
                    layerMaps.insert(layerMaps.end(), alternatives.begin(), alternatives.end());
                }
            }
            // Logical alternatives describe the same selected value position.
            std::vector<simfil::SchemaId> pending(expanded.begin(), expanded.end());
            while (!pending.empty() && step()) {
                auto current = pending.back();
                pending.pop_back();
                for (auto alternative : schema->alternatives(current))
                    if (expanded.insert(alternative).second)
                        pending.push_back(alternative);
            }
            expanded.erase(simfil::NoSchemaId);
            values = Json::array({schemaOverview(*root, expanded)});
        }
        // Projections such as **.typename may never emit a node-budget marker themselves.
        if (model->exhausted())
            truncate("schema_node_limit");
        Json item{{"schemaId", id}, {"values", std::move(values)}};
        if (!arguments.contains("schemaId"))
            item["featureType"] = types[i];
        if (!append(std::move(item)))
            break;
    }
}

void McpNativeTools::Call::validateExpression()
{
    selectLayer();
    environment();
    auto query = arguments.at("expression").get<std::string>();
    auto schema = layer_->layerSchema();
    Json item{
        {"valid", true},
        {"schemaAvailable", bool(schema)},
        {"runtimeValidated", false},
        {"contexts", Json::array()},
        {"normalizedExpression", query}};
    auto scope = arguments.value("scope", "feature");
    if ((arguments.value("rewrite", false) || scope == "auto") && schema) {
        auto normal = schema->normalizeSearchQuery(
            query,
            scope == "attribute" ? LayerSchema::SearchQueryRequestedScope::Attribute :
                scope == "auto"  ? LayerSchema::SearchQueryRequestedScope::Auto :
                                   LayerSchema::SearchQueryRequestedScope::Feature);
        if (!normal) {
            item["valid"] = false;
            item["normalizationError"] = normal.error().message;
            append(boundedJson(item));
            return;
        }
        query = normal->normalizedQuery_;
        scope = normal->concreteScope_ == LayerSchema::SearchQueryConcreteScope::Attribute ?
            "attribute" :
            "feature";
        item["normalizedExpression"] = query;
        item["rewriteSuppressed"] = normal->rewriteSuppressed_;
        item["rewriteSuppressionReason"] = normal->rewriteSuppressionReason_;
    }
    auto types = arguments.contains("featureType") ?
        std::vector<std::string>{arguments["featureType"]} :
        schema ?
        schema->featureTypes() :
        std::vector<std::string>{""};
    std::set<std::pair<std::string, simfil::SchemaId>> contexts;
    for (auto const& type : types) {
        if (!step())
            break;
        auto id = schema ? schema->featureSchema(type) : simfil::NoSchemaId;
        if (arguments.contains("featureType") && schema && id == simfil::NoSchemaId)
            throw std::invalid_argument("type");
        if (scope == "attribute" && schema) {
            bool found = false;
            for (auto const& attribute : schema->attributeScopes()) {
                if (!step())
                    break;
                if (attribute.featureType_ != type ||
                    (arguments.contains("attributeSchema") &&
                     arguments["attributeSchema"] != attribute.attributeSchema_))
                    continue;
                contexts
                    .emplace(type, schema->attributeQuerySchema(type, attribute.attributeSchema_));
                found = true;
            }
            if (!found && arguments.contains("attributeSchema"))
                throw std::invalid_argument("attribute schema");
            if (!found)
                contexts.emplace(type, simfil::NoSchemaId);
        }
        else
            contexts.emplace(type, id);
    }
    for (auto const& [type, id] : contexts) {
        if (!step())
            break;
        auto ast = simfil::compile(
            *env_,
            query,
            {.any = arguments.value("predicate", false),
             .rewriteMode = simfil::RewriteMode::Schema,
             .rootSchema = id});
        Json context{{"featureType", type}, {"schemaId", id}, {"schemaCertain", false}};
        if (!ast) {
            item["valid"] = false;
            context["error"] = {
                {"message", ast.error().message},
                {"offset", ast.error().location.offset},
                {"size", ast.error().location.size}};
        }
        else if (id != simfil::NoSchemaId) {
            auto paths = simfil::referencedSchemaPaths(*env_, **ast, id);
            if (paths) {
                context["unresolvedAccess"] = paths->hasUnresolvedAccess;
                context["dynamicAccess"] = paths->hasDynamicAccess;
                context["broadWildcardAccess"] = paths->hasBroadWildcardAccess;
                context["schemaCertain"] = !paths->hasUnresolvedAccess &&
                    !paths->hasDynamicAccess && !paths->hasBroadWildcardAccess && !schema->open(id);
            }
        }
        item["contexts"].push_back(boundedJson(context));
    }
    append(boundedJson(item));
}

}  // namespace mapget::detail
