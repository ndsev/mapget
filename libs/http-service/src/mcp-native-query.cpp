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

#include <cctype>
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
            call_.chargeString(name);
            call_.charge(128);
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

void McpNativeTools::Call::chargeString(std::string_view text)
{
    // Reject oversized input before walking it. UTF-8 bytes are preserved by JSON dumping.
    charge(text.size() + 2);
    for (unsigned char c : text) {
        if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t')
            charge(1);
        else if (c < 0x20)
            charge(5);
    }
}

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
        chargeString(value.get_ref<std::string const&>());
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
            chargeString(it.key());
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
        chargeString(text);
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
            chargeString(*key);
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

void McpNativeTools::Call::diagnostics(
    simfil::Diagnostics const& data,
    std::string const& expression)
{
    auto messages = simfil::diagnostics(data);
    if (!messages)
        return;
    for (auto const& message : *messages) {
        auto text = message.message;
        if (name == "mapget_extract_features" && arguments.value("scope", "feature") == "feature" &&
            (text == "No matches for field '$feature'" || text == "No matches for field '$'"))
            text +=
                ". Feature scope starts at the feature: use _.properties or properties. "
                "$feature is available in scope:attribute only; $ is not the root alias. "
                "This diagnostic does not establish that the requested property is absent.";
        // Sparse optional fields commonly emit the same warning for hundreds of records.
        // Keep its frequency and expression without stopping before a later matching record.
        auto existing = std::find_if(
            issues_.begin(),
            issues_.end(),
            [&](Json const& issue)
            {
                return issue.value("expression", "") == expression &&
                    issue.value("message", "") == text && issue.contains("offset") &&
                    issue["offset"] == message.location.offset &&
                    issue["size"] == message.location.size;
            });
        if (existing != issues_.end()) {
            auto previous = (*existing)["occurrences"].get<size_t>();
            if (std::to_string(previous).size() < std::to_string(previous + 1).size())
                charge(1);
            (*existing)["occurrences"] = previous + 1;
            continue;
        }
        if (issues_.size() >= 100) {
            truncate("diagnostic_limit");
            break;
        }
        issues_.push_back(boundedJson(
            {{"message", text},
             {"expression", expression},
             {"occurrences", 1},
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
                    // Predicate results are internal decisions, not response payload.
                    // They consume evaluator work but must not exhaust output bytes.
                    result.push_back(
                        predicate ?
                            Json(value.type == ValueType::Bool && value.as<ValueType::Bool>()) :
                            valueJson(value));
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
        diagnostics(data, query);
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
        // A semantic reference is intentional, not a truncated query. Enum previews are bounded.
        if ((fields || domains) && (!expand || depth >= 2)) {
            if (node.at(i)->size())
                result["childrenOmitted"] = true;
            continue;
        }
        auto name = node.owningModel()->lookupStringId(key).value();
        chargeString(name);
        auto child = node.at(i);
        if (key == simfil::StringPool::SchemaEnum && child->size() > 8) {
            result["enumCount"] = child->size();
            result["enumPreview"] = Json::array();
            for (uint32_t j = 0; j < 8; ++j)
                result["enumPreview"].push_back(valueJson(simfil::Value::field(child->at(j))));
            continue;
        }
        if (!fields && !domains) {
            result[name] = valueJson(simfil::Value::field(child));
            continue;
        }
        auto values = fields ? Json::object() : Json::array();
        if (child->size() > 16) {
            result[fields ? "fieldCount" : "domainCount"] = child->size();
            result["childrenOmitted"] = true;
        }
        for (uint32_t j = 0; j < std::min<uint32_t>(child->size(), 16); ++j) {
            // Feature.attributes aliases Feature.properties. Preserve its reference without
            // expanding the same large attribute inventory twice.
            auto const alias = fields && depth == 0 &&
                child->owningModel()->lookupStringId(child->keyAt(j)).value() == "attributes";
            auto value = schemaOverview(
                *child->at(j),
                alias ? std::set<simfil::SchemaId>{} : expanded,
                depth + 1);
            if (fields) {
                auto field = child->owningModel()->lookupStringId(child->keyAt(j)).value();
                chargeString(field);
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
        if (!schema->hasSchema(id)) {
            fail(
                "unknown_schema",
                "The selected layer has no such schemaId. Read mapget_query_schema "
                "with the same mapId/layerId and no schemaId to discover its current roots.");
            return;
        }
        roots.push_back(id);
    }
    else {
        for (auto const& type : types) {
            auto id = schema->featureSchema(type);
            if (id == simfil::NoSchemaId) {
                fail(
                    "unknown_feature_type",
                    "The selected layer has no schema root for this featureType. "
                    "Omit featureType to list its roots. Some reference-only identifier types "
                    "may belong to another layer.");
                return;
            }
            roots.push_back(id);
        }
    }
    if (arguments.contains("find")) {
        findSchema(roots, types);
        return;
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
            // Unfocused discovery is a type inventory. Opening a type/definition is shallow:
            // compound domains remain directly followable $refs instead of exhausting a page.
            std::set<simfil::SchemaId> expanded;
            if (arguments.contains("featureType") || arguments.contains("schemaId"))
                expanded.insert(id);
            if (arguments.contains("featureType")) {
                auto properties = simfil::Schema::fieldSchemas(
                    id,
                    env_->querySchemaCallback,
                    StringPool::PropertiesStr);
                expanded.insert(properties.begin(), properties.end());
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

void McpNativeTools::Call::findSchema(
    std::vector<simfil::SchemaId> const& roots,
    std::vector<std::string> const& types)
{
    auto const schema = layer_->layerSchema();
    auto normalize = [](std::string_view value)
    {
        std::string result;
        for (unsigned char c : value)
            if (!std::isspace(c) && c != '_' && c != '-')
                result.push_back(static_cast<char>(std::tolower(c)));
        return result;
    };
    auto needle = normalize(arguments.at("find").get<std::string>());
    if (needle.empty()) {
        fail("invalid_arguments", "find requires a declared field, type or enum symbol name.");
        return;
    }
    auto match = [&](std::string_view name)
    {
        auto text = normalize(name);
        return text == needle ? 2 : text.find(needle) != std::string::npos ? 1 : 0;
    };
    auto selection = Json{{"mapId", arguments.at("mapId")}, {"layerId", arguments.at("layerId")}};
    if (arguments.contains("sourceId"))
        selection["sourceId"] = arguments["sourceId"];
    // Retain only the best bounded set, while counting distinct matching definitions and
    // preserving representative owners. An exact enum symbol outranks incidental field text.
    std::map<simfil::SchemaId, std::pair<int, Json>> best;
    std::set<simfil::SchemaId> matchedDomains;
    size_t reserve = std::min<size_t>(remainingWork_ / 4, 8000);
    remainingWork_ -= reserve;
    for (size_t i = 0; i < roots.size() && step(); ++i) {
        std::set<simfil::SchemaId> visited, rootMatches;
        std::vector<std::string> path;
        std::vector<bool> elementSteps;
        std::function<void(simfil::SchemaId, std::string_view)> walk;
        walk = [&](simfil::SchemaId id, std::string_view field)
        {
            if (!step() || id == simfil::NoSchemaId)
                return;
            if (path.size() > SerializationNestingLimit) {
                truncate("serialization_limit");
                return;
            }
            auto fieldMatch = match(field), typeMatch = match(schema->typeName(id));
            int rank = fieldMatch == 2 ? 1 :
                typeMatch == 2         ? 2 :
                fieldMatch             ? 3 :
                typeMatch              ? 5 :
                                         6;
            auto symbols = schema->directEnumSymbols(id);
            Json matched = Json::array();
            size_t symbolMatches = 0;
            for (auto const& symbol : symbols) {
                if (!step())
                    return;
                auto score = match(symbol);
                if (!score)
                    continue;
                rank = std::min(rank, score == 2 ? 0 : 4);
                ++symbolMatches;
                // Exact matches stay visible even after a full substring preview.
                if (score == 2) {
                    matched.insert(matched.begin(), symbol);
                    if (matched.size() > 8)
                        matched.erase(matched.end() - 1);
                }
                else if (matched.size() < 8)
                    matched.push_back(symbol);
            }
            // Prefer the nearby owning field/assignment within each relevance class.
            // Numeric SchemaId order is not a useful ranking for broad concepts.
            rank = rank * 512 + static_cast<int>(path.size());
            if (rank < 6 * 512) {
                matchedDomains.insert(id);
                rootMatches.insert(id);
                auto found = best.find(id);
                if (found == best.end() && best.size() >= limit_) {
                    auto worst = std::max_element(
                        best.begin(),
                        best.end(),
                        [](auto const& a, auto const& b) {
                            return std::pair(a.second.first, a.first) <
                                std::pair(b.second.first, b.first);
                        });
                    if (std::pair(rank, id) < std::pair(worst->second.first, worst->first))
                        best.erase(worst);
                }
                if (found != best.end() || best.size() < limit_) {
                    auto& entry = best[id];
                    if (entry.second.is_null()) {
                        entry.first = rank;
                        auto expand = selection;
                        expand["schemaId"] = id;
                        entry.second = {
                            {"schemaId", id},
                            {"kind",
                             env_->strings()
                                 ->resolve(simfil::Schema::kindNameId(schema->kind(id)))
                                 .value_or("unknown")},
                            {"contexts", Json::array()},
                            {"contextsOmitted", false},
                            {"expandArguments", expand}};
                        if (!schema->typeName(id).empty())
                            entry.second["typename"] = schema->typeName(id);
                        if (!symbols.empty() || !schema->enumValues(id).empty()) {
                            auto& domain = entry.second["enum"];
                            domain = {
                                {"symbolCount", symbols.size()},
                                {"matchingSymbolCount", symbolMatches},
                                {"matchingSymbols", matched},
                                {"symbolsOmitted", symbols.size() > 8},
                                {"preview", Json::array()}};
                            for (size_t n = 0; n < std::min<size_t>(symbols.size(), 8); ++n)
                                domain["preview"].push_back(symbols[n]);
                            domain["literalCount"] = schema->enumValues(id).size();
                            domain["valuesArguments"] = expand;
                            domain["valuesArguments"]["query"] = "enum.*";
                            domain["valuesArguments"]["limit"] = std::min<size_t>(
                                1000,
                                symbols.size() + schema->enumValues(id).size());
                            domain["symbolSemantics"] = schema->kind(id) ==
                                    LayerSchema::BitmaskKind ?
                                "flags" :
                                "string_literals";
                        }
                    }
                    auto preferContext = rank < entry.first;
                    entry.first = std::min(entry.first, rank);
                    Json context{{"path", path}};
                    if (!field.empty())
                        context["fieldName"] = field;
                    if (!arguments.contains("schemaId")) {
                        context["featureType"] = types[i];
                        auto owner = schema->ownerForPath(types[i], roots[i], path);
                        // ownerForPath accepts named fields, not array wildcards. Keep the
                        // enclosing assignment owner when the discovered value is inside an array.
                        for (size_t end = path.size();
                             owner.kind_ == LayerSchema::PathOwnerKind::Unknown && end;
                             --end)
                            owner = schema->ownerForPath(
                                types[i],
                                roots[i],
                                std::span(path.data(), end - 1));
                        if (owner.kind_ == LayerSchema::PathOwnerKind::Attribute)
                            context["attributeContext"] = {
                                {"featureType", owner.attribute_.featureType_},
                                {"layer", owner.attribute_.attributeLayerName_},
                                {"name", owner.attribute_.attributeName_},
                                {"schemaId", owner.attribute_.attributeSchema_}};
                        // String subscripts preserve field names; dot-string syntax is a literal.
                        std::string query = "_";
                        for (size_t segment = 0; segment < path.size(); ++segment)
                            query += elementSteps[segment] ?
                                ".*" :
                                "[" + Json(path[segment]).dump() + "]";
                        context["featureQuery"] = query;
                    }
                    else
                        context["relativeToSchemaId"] = roots[i];
                    auto& contexts = entry.second["contexts"];
                    if (std::find(contexts.begin(), contexts.end(), context) == contexts.end()) {
                        if (preferContext) {
                            contexts.insert(contexts.begin(), context);
                            if (contexts.size() > 2) {
                                contexts.erase(contexts.end() - 1);
                                entry.second["contextsOmitted"] = true;
                            }
                        }
                        else if (contexts.size() < 2)
                            contexts.push_back(context);
                        else
                            entry.second["contextsOmitted"] = true;
                    }
                }
            }
            if (!visited.insert(id).second)
                return;
            schema->forEachDirectField(
                id,
                [&](std::string_view name, auto children)
                {
                    if (!step())
                        return;
                    path.emplace_back(name);
                    elementSteps.push_back(false);
                    for (auto child : children)
                        walk(child, name);
                    path.pop_back();
                    elementSteps.pop_back();
                });
            schema->forEachElementSchema(
                id,
                [&](simfil::SchemaId child)
                {
                    path.emplace_back("*");
                    elementSteps.push_back(true);
                    walk(child, {});
                    path.pop_back();
                    elementSteps.pop_back();
                });
            for (auto alternative : schema->alternatives(id))
                walk(alternative, field);
        };
        walk(roots[i], {});
        if (!rootMatches.empty() && narrowing_.size() < 8 && roots.size() > 1) {
            auto next = selection;
            next["find"] = arguments["find"];
            if (arguments.contains("schemaId"))
                next["schemaId"] = roots[i];
            else
                next["featureType"] = types[i];
            narrowing_.push_back({{"arguments", next}, {"matchedDomains", rootMatches.size()}});
        }
    }
    auto scanReason = std::exchange(incomplete_, {});
    remainingWork_ += reserve;
    std::vector<std::pair<int, Json>> ordered;
    for (auto& [_, entry] : best)
        ordered.push_back(std::move(entry));
    std::stable_sort(
        ordered.begin(),
        ordered.end(),
        [](auto const& a, auto const& b) { return a.first < b.first; });
    for (auto& [_, item] : ordered) {
        try {
            if (item.contains("enum") && item["enum"]["literalCount"] != 0) {
                auto model = std::make_shared<
                    simfil::SchemaModel>(env_->strings(), env_->querySchemaCallback, 64);
                auto values = model->root(item["schemaId"].get<simfil::SchemaId>())
                                  ->get(simfil::StringPool::SchemaEnum);
                auto start = item["enum"]["symbolCount"].get<size_t>();
                item["enum"]["literalPreview"] = Json::array();
                for (size_t n = start; values && n < std::min<size_t>(values->size(), start + 8);
                     ++n)
                    item["enum"]["literalPreview"]
                        .push_back(valueJson(simfil::Value::field(values->at(n))));
                item["enum"]["literalsOmitted"] = item["enum"]["literalCount"].get<size_t>() > 8;
            }
            if (roots.size() == 1 && narrowing_.size() < 8) {
                auto next = item["expandArguments"];
                for (auto const& context : item["contexts"])
                    if (context.contains("attributeContext")) {
                        next["schemaId"] = context["attributeContext"]["schemaId"];
                        break;
                    }
                next["find"] = arguments["find"];
                auto same = std::any_of(
                    narrowing_.begin(),
                    narrowing_.end(),
                    [&](auto const& choice) { return choice["arguments"] == next; });
                if (!same && next["schemaId"] != roots.front())
                    narrowing_.push_back({{"arguments", next}});
            }
            if (!append(boundedJson(item)))
                break;
        }
        catch (std::length_error const&) {
            break;
        }
    }
    discovery_ = {
        {"scanComplete", scanReason.empty()},
        {"matchedDomains", matchedDomains.size()},
        {"returnedDomains", items_.size()},
        {"omittedDomains", matchedDomains.size() - items_.size()},
        {"paths", "representative"}};
    if (!scanReason.empty())
        truncate(scanReason);
    else if (matchedDomains.size() > items_.size())
        truncate("matches_omitted");
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
        if (arguments.contains("featureType") && schema && id == simfil::NoSchemaId) {
            fail(
                "unknown_feature_type",
                "The selected layer has no schema root for this featureType. "
                "Use mapget_query_schema without featureType to list its roots. "
                "Some reference-only identifier types may belong to another layer.");
            return;
        }
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
    auto const offset = arguments.value("offset", size_t{0});
    item["contextCount"] = contexts.size();
    item["contextOffset"] = offset;
    item["scope"] = scope;
    item = boundedJson(item);
    size_t position = 0;
    for (auto const& [type, id] : contexts) {
        if (position++ < offset)
            continue;
        if (item["contexts"].size() >= limit_) {
            nextOffset_ = position - 1;
            truncate("item_limit");
            break;
        }
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
        try {
            item["contexts"].push_back(boundedJson(context));
        }
        catch (std::length_error const&) {
            break;  // Keep successful context diagnostics when a later one exceeds the budget.
        }
    }
    append(std::move(item));
}

}  // namespace mapget::detail
