#include "mcp-native-call.h"

#include "mapget/model/attr.h"
#include "mapget/model/attrlayer.h"
#include "mapget/model/featureid.h"
#include "mapget/model/layerschema.h"
#include "mapget/model/mapschema.h"
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
                            auto text = v.stringView();
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
    return result.is_object() && (result.contains("$mapget") || result.contains("_bitmask")) ?
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
    auto scalar = [&](Json raw)
    {
        // Provenance is explicit response metadata, never inferred from a numeric code.
        auto type = value.schemaReference();
        if (!type)
            return raw;
        charge(64);
        chargeString(type.binding->identity());
        return Json{
            {"$mapget", "qualified"},
            {"value", std::move(raw)},
            {"schema", type.binding->identity()},
            {"typeId", type.id}};
    };
    switch (value.type) {
    case ValueType::Undef: return {{"$mapget", "undefined"}};
    case ValueType::Null: return nullptr;
    case ValueType::Bool: return scalar(value.as<ValueType::Bool>());
    case ValueType::Int: {
        auto n = value.as<ValueType::Int>();
        if (n < -9007199254740991LL || n > 9007199254740991LL)
            return scalar({{"$mapget", "int64"}, {"value", std::to_string(n)}});
        return scalar(n);
    }
    case ValueType::Float: {
        auto n = value.as<ValueType::Float>();
        return scalar(
            std::isfinite(n) ?
                Json(n) :
                Json{
                    {"$mapget", "nonfinite"},
                    {"value",
                     std::isnan(n) ? "NaN" :
                         n > 0     ? "Infinity" :
                                     "-Infinity"}});
    }
    case ValueType::String: {
        auto text = value.stringView();
        chargeString(text);
        return scalar(text);
    }
    case ValueType::Bytes: {
        auto const& bytes = value.as<ValueType::Bytes>();
        charge(bytes.bytes.size() * 2);
        return scalar({{"$mapget", "bytes"}, {"hex", bytes.toHex(false)}});
    }
    case ValueType::Bitmask: {
        auto const& mask = value.as<ValueType::Bitmask>();
        // simfil owns the full-width tag and type validation; no mapget-specific codec.
        charge(96);
        if (mask.type)
            chargeString(mask.type.binding->identity());
        return mask.toJson();
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
            auto item = valueJson(simfil::Value::at(*node, *child, i), depth + 1);
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
        return result.is_object() && (result.contains("$mapget") || result.contains("_bitmask")) ?
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
    simfil::TypeId schema,
    bool predicate)
{
    auto result = Json::array();
    if (!step())
        return result;
    auto key = std::make_tuple(query, schema, predicate);
    auto found = expressions_.find(key);
    if (found == expressions_.end()) {
        auto expression = query;
        if (predicate && name == "mapget_extract_features" && arguments.value("rewrite", false) &&
            layer_->layerSchema())
        {
            auto scope = arguments.value("scope", "feature") == "attribute" ?
                LayerSchema::SearchQueryRequestedScope::Attribute :
                LayerSchema::SearchQueryRequestedScope::Feature;
            // Extraction has installed the actual partition's owner, not a catalog replacement.
            auto normal = layer_->layerSchema()->normalizeSearchQuery(query, scope);
            if (!normal)
                throw std::invalid_argument("Invalid predicate normalization");
            expression = normal->normalizedQuery_;
        }
        auto ast = simfil::compile(
            *env_,
            expression,
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
    std::set<simfil::TypeId> const& expanded,
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
            expand = expanded.contains(ids.get<simfil::TypeId>());
        else if (ids.is_array())
            for (auto const& id : ids)
                expand |= expanded.contains(id.get<simfil::TypeId>());
        result["$ref"] = std::move(ids);
    }
    for (uint32_t i = 0; i < node.size(); ++i) {
        if (!step())
            throw std::length_error("work");
        auto key = node.keyAt(i);
        if (key == simfil::StringPool::SchemaRef)
            continue;
        bool fields = key == simfil::StringPool::SchemaFields;
        bool types = key == simfil::StringPool::SchemaElements ||
            key == simfil::StringPool::SchemaAlternatives;
        // A semantic reference is intentional, not a truncated query. Enum previews are bounded.
        if ((fields || types) && (!expand || depth >= 2)) {
            if (node.at(i)->size())
                result["childrenOmitted"] = true;
            continue;
        }
        auto name = node.owningModel()->lookupStringId(key).value();
        chargeString(name);
        auto child = node.at(i);
        if ((key == simfil::StringPool::SchemaEnum || name == "namedValues") && child->size() > 8) {
            auto prefix = key == simfil::StringPool::SchemaEnum ? "enum" : "namedValue";
            result[std::string(prefix) + "Count"] = child->size();
            auto& preview = result[std::string(prefix) + "Preview"] = Json::array();
            for (uint32_t j = 0; j < 8; ++j)
                preview.push_back(valueJson(simfil::Value::at(*child, *child->at(j), j)));
            continue;
        }
        if (!fields && !types) {
            result[name] = valueJson(simfil::Value::field(child));
            continue;
        }
        auto values = fields ? Json::object() : Json::array();
        if (child->size() > 16) {
            result[fields ? "fieldCount" : "typeCount"] = child->size();
            result["childrenOmitted"] = true;
        }
        for (uint32_t j = 0; j < std::min<uint32_t>(child->size(), 16); ++j) {
            // The native graph omits lookup-only aliases from enumeration.
            // Explicitly declared members retain their ordinary expansion policy.
            auto value = schemaOverview(*child->at(j), expanded, depth + 1);
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
    if (!selectSchemaLayer())
        return;
    auto schema = layer_->layerSchema();
    if (!schema) {
        fail("unavailable", "Layer has no bound schema.");
        return;
    }
    auto reserveContract = [&](size_t bytes)
    {
        if (!step())
            throw std::length_error("Schema contract work budget.");
        charge(bytes);
    };
    auto types = arguments.contains("featureType") ?
        std::vector<std::string>{arguments["featureType"]} :
        schema->featureTypes();
    if (layer_->type_ != LayerType::Features && arguments.contains("featureType"))
        throw std::invalid_argument("featureType requires a feature layer");
    std::vector<simfil::TypeId> roots;
    if (arguments.contains("typeId")) {
        if (arguments.contains("featureType"))
            throw std::invalid_argument("Choose featureType or typeId");
        auto id = arguments["typeId"].get<simfil::TypeId>();
        if (!schema->hasSchema(id)) {
            fail(
                "unknown_schema",
                "The selected layer has no such typeId. Read mapget_query_schema "
                "with the same mapId/layerId and no typeId to discover its current roots.");
            return;
        }
        roots.push_back(id);
    }
    else if (layer_->type_ != LayerType::Features) {
        roots.push_back(schema->rootSchema());
    }
    else {
        for (auto const& type : types) {
            // Identity-only declarations can have a synthesized common-field type,
            // but that type does not describe data produced by this layer.
            if (arguments.contains("featureType") && schema->getTypeInfo(type, false) &&
                !schema->producesFeatureType(type))
            {
                if (arguments.contains("query") || arguments.contains("find")) {
                    fail("unknown_feature_type", "This featureType is an identity-only reference.");
                    return;
                }
                append(
                    {{"featureType", type},
                     {"typeId", nullptr},
                     {"contract", boundedJson(schema->featureTypeContract(type, reserveContract))},
                     {"values", Json::array()}});
                return;
            }
            auto id = schema->featureSchema(type);
            if (id == simfil::NoTypeId) {
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
    environment();
    auto model = std::make_shared<simfil::SchemaModel>(
        env_->strings(),
        env_->querySchemaCallback,
        std::max(size_t{2}, remainingWork_));
    for (size_t i = 0; i < roots.size() && step(); ++i) {
        auto id = roots[i];
        auto root = model->root(id);
        Json values;
        if (arguments.contains("query")) {
            // Explicit queries see the whole lazy graph, never the overview's projection.
            values = evaluate(arguments["query"], *root, simfil::NoTypeId);
        }
        else {
            // Unfocused discovery is a type inventory. Opening a type/definition is shallow:
            // compound types remain directly followable $refs instead of exhausting a page.
            std::set<simfil::TypeId> expanded;
            if (arguments.contains("featureType") || arguments.contains("typeId") ||
                layer_->type_ != LayerType::Features)
                expanded.insert(id);
            if (arguments.contains("featureType")) {
                auto properties = simfil::Schema::fieldSchemas(
                    id,
                    env_->querySchemaCallback,
                    StringPool::PropertiesStr);
                expanded.insert(properties.begin(), properties.end());
            }
            // Logical alternatives describe the same selected value position.
            std::vector<simfil::TypeId> pending(expanded.begin(), expanded.end());
            while (!pending.empty() && step()) {
                auto current = pending.back();
                pending.pop_back();
                for (auto alternative : schema->alternatives(current))
                    if (expanded.insert(alternative).second)
                        pending.push_back(alternative);
            }
            expanded.erase(simfil::NoTypeId);
            values = Json::array({schemaOverview(*root, expanded)});
        }
        // Projections such as **.typename may never emit a node-budget marker themselves.
        if (model->exhausted())
            truncate("schema_node_limit");
        Json item{{"typeId", id}, {"values", std::move(values)}};
        if (arguments.contains("featureType") && !arguments.contains("query"))
            item["contract"] = boundedJson(schema->featureTypeContract(types[i], reserveContract));
        if (!arguments.contains("typeId") && layer_->type_ == LayerType::Features)
            item["featureType"] = types[i];
        else if (layer_->type_ != LayerType::Features)
            item["layerId"] = layer_->layerId_;
        if (!append(std::move(item)))
            break;
    }
}

void McpNativeTools::Call::findSchema(
    std::vector<simfil::TypeId> const& roots,
    std::vector<std::string> const& types)
{
    auto schema = layer_->layerSchema();
    simfil::EvaluationSummary summary;
    auto found = schema->find(
        arguments.at("find").get<std::string>(),
        roots,
        !arguments.contains("typeId") && layer_->type_ == LayerType::Features ?
            std::span<std::string const>(types) :
            std::span<std::string const>{},
        {.maxResults = limit_,
         .maxWork = remainingWork_,
         .deadline = deadline_,
         .cancel = cancelled.get()},
        remainingBytes_,
        &summary);
    remainingWork_ -= std::min(remainingWork_, summary.work);
    if (!step(0)) {
        discovery_ = std::move(found["discovery"]);
        discovery_["returnedTypes"] = 0;
        discovery_["omittedTypes"] = discovery_["matchedTypes"];
        return;
    }
    auto selection = Json{
        {"layerId", arguments.at("layerId")},
        {"mapSchemaId", layer_->schemaOwner()->schemaId()}};
    for (auto const* key : {"mapId", "sourceId", "mapFormat"})
        if (arguments.contains(key))
            selection[key] = arguments[key];
    // Only this transport adapter knows tool argument names or recommends follow-up calls.
    for (auto const& root : found["rootMatches"]) {
        auto next = selection;
        next["find"] = arguments["find"];
        if (root.contains("featureType"))
            next["featureType"] = root["featureType"];
        else
            next["typeId"] = root["typeId"];
        narrowing_.push_back({{"arguments", next}, {"matchedTypes", root["matchedTypes"]}});
    }
    for (auto& item : found["items"]) {
        try {
            auto expand = selection;
            expand["typeId"] = item["typeId"];
            item["expandArguments"] = expand;
            if (item.contains("namedValues")) {
                auto read = expand;
                read["query"] = "namedValues.*";
                read["limit"] = std::min<size_t>(1000, item["namedValues"]["count"].get<size_t>());
                item["namedValues"]["valuesArguments"] = std::move(read);
            }
            if (item.contains("enum")) {
                auto read = expand;
                read["query"] = "enum.*";
                read["limit"] = std::min<size_t>(
                    1000,
                    item["enum"]["symbolCount"].get<size_t>() +
                        item["enum"]["literalCount"].get<size_t>());
                item["enum"]["valuesArguments"] = std::move(read);
            }
            if (roots.size() == 1 && narrowing_.size() < 8) {
                auto next = expand;
                for (auto const& context : item["contexts"])
                    if (context.contains("attributeContext")) {
                        next["typeId"] = context["attributeContext"]["typeId"];
                        break;
                    }
                next["find"] = arguments["find"];
                auto same = std::any_of(
                    narrowing_.begin(),
                    narrowing_.end(),
                    [&](auto const& choice) { return choice["arguments"] == next; });
                if (!same && next["typeId"] != roots.front())
                    narrowing_.push_back({{"arguments", next}});
            }
            if (!append(boundedJson(item)))
                break;
        }
        catch (std::length_error const&) {
            break;
        }
    }
    discovery_ = std::move(found["discovery"]);
    discovery_["returnedTypes"] = items_.size();
    discovery_["omittedTypes"] = discovery_["matchedTypes"].get<size_t>() - items_.size();
    if (!found["complete"].get<bool>())
        truncate(
            found["reason"] == "cancelled" ?
                "cancelled_or_authority_expired" :
                found["reason"].get<std::string>());
}

void McpNativeTools::Call::validateExpression()
{
    if (!selectSchemaLayer())
        return;
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
    auto featureLayer = layer_->type_ == LayerType::Features;
    if (!featureLayer &&
        (arguments.contains("featureType") || arguments.contains("attributeTypeId") ||
         scope == "attribute"))
        throw std::invalid_argument("Feature/attribute context requires a feature layer");
    if ((arguments.value("rewrite", false) || scope == "auto") && schema && featureLayer) {
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
        schema && featureLayer ?
        schema->featureTypes() :
        std::vector<std::string>{""};
    std::set<std::pair<std::string, simfil::TypeId>> contexts;
    for (auto const& type : types) {
        if (!step())
            break;
        auto id = schema ?
            (featureLayer ? schema->featureSchema(type) : schema->rootSchema()) :
            simfil::NoTypeId;
        if (arguments.contains("featureType") && schema &&
            (id == simfil::NoTypeId || !schema->producesFeatureType(type)))
        {
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
                    (arguments.contains("attributeTypeId") &&
                     arguments["attributeTypeId"] != attribute.attributeSchema_))
                    continue;
                contexts
                    .emplace(type, schema->attributeQuerySchema(type, attribute.attributeSchema_));
                found = true;
            }
            if (!found && arguments.contains("attributeTypeId"))
                throw std::invalid_argument("attribute schema");
            if (!found)
                contexts.emplace(type, simfil::NoTypeId);
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
        Json context{{"featureType", type}, {"typeId", id}, {"schemaCertain", false}};
        if (!featureLayer) {
            context.erase("featureType");
            context["layerId"] = layer_->layerId_;
        }
        if (!ast) {
            item["valid"] = false;
            context["error"] = {
                {"message", ast.error().message},
                {"offset", ast.error().location.offset},
                {"size", ast.error().location.size}};
        }
        else if (id != simfil::NoTypeId) {
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
