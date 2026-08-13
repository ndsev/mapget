#include "featurelayer-filter.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <tuple>
#include <utility>

#include "fmt/format.h"
#include "mapget/model/layerschema.h"
#include "mapget/model/simfilutil.h"
#include "simfil/overlay.h"
#include "simfil/simfil.h"

namespace mapget
{
namespace
{

constexpr size_t CancellationCheckBatch = 1024;

class CancellationProbe
{
public:
    explicit CancellationProbe(FeatureLayerFilterCancellationCheck const& check) : check_(check) {}

    [[nodiscard]] bool boundary() const { return check_ && check_(); }

    [[nodiscard]] bool periodic()
    {
        ++iterations_;
        return iterations_ % CancellationCheckBatch == 0 && boundary();
    }

private:
    FeatureLayerFilterCancellationCheck const& check_;
    size_t iterations_ = 0;
};

class FeatureArrayView;

/** Request-local storage for an array spanning several immutable source pools. */
struct FeatureArrayViewStorage final : simfil::Model
{
    explicit FeatureArrayViewStorage(std::vector<model_ptr<Feature>> features)
        : features_(std::move(features))
    {
    }

    tl::expected<void, simfil::Error>
    resolve(simfil::ModelNode const& node, ResolveFn const& callback) const override;

    std::vector<model_ptr<Feature>> features_;
};

/** Transient SIMFIL array exposing owner-aware source Feature nodes directly. */
class FeatureArrayView final : public simfil::MandatoryDerivedModelNodeBase<FeatureArrayViewStorage>
{
public:
    explicit FeatureArrayView(std::vector<model_ptr<Feature>> features, simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<FeatureArrayViewStorage>(
              std::make_shared<FeatureArrayViewStorage>(std::move(features)),
              {simfil::ModelPool::Arrays, 0},
              key)
    {
    }

    explicit FeatureArrayView(simfil::ModelNode const& node, simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<FeatureArrayViewStorage>(node, key)
    {
    }

    [[nodiscard]] simfil::ValueType type() const override { return simfil::ValueType::Array; }

    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t index) const override
    {
        if (index < 0 || static_cast<size_t>(index) >= model().features_.size()) {
            return {};
        }
        return simfil::ModelNode::Ptr(model().features_[static_cast<size_t>(index)]);
    }

    [[nodiscard]] uint32_t size() const override
    {
        return static_cast<uint32_t>(
            std::min<size_t>(model().features_.size(), std::numeric_limits<uint32_t>::max()));
    }

    bool iterate(IterCallback const& callback) const override
    {
        for (auto const& feature : model().features_) {
            if (!callback(*feature)) {
                return false;
            }
        }
        return true;
    }
};

tl::expected<void, simfil::Error>
FeatureArrayViewStorage::resolve(simfil::ModelNode const& node, ResolveFn const& callback) const
{
    auto resolved = simfil::model_ptr<FeatureArrayView>::make(node);
    callback(*resolved);
    return {};
}

struct EvaluatorFailure
{
    simfil::Error error_;
    bool compilation_ = false;
};

/** Cache SIMFIL programs without mutating the datasource-owned expression cache. */
class FilterEvaluator
{
public:
    using ProgramKey = std::tuple<std::string, bool, simfil::SchemaId>;

    /**
     * Successful expression values scoped to one exact model context.
     *
     * Programs remain evaluator-wide, while values must never escape the
     * feature/attribute root against which they were evaluated.
     */
    struct EvaluationCache
    {
        std::map<ProgramKey, std::vector<simfil::Value>> values_;
    };

    FilterEvaluator(
        std::shared_ptr<StringPool> strings,
        std::unique_ptr<simfil::Environment> environment)
        : strings_(std::move(strings)), environment_(std::move(environment))
    {
    }

    struct Result
    {
        std::vector<simfil::Value> values_;
        simfil::Diagnostics diagnostics_;
        std::map<std::string, simfil::Trace> traces_;
    };

    tl::expected<Result, EvaluatorFailure> evaluate(
        std::string_view expression,
        simfil::ModelNode const& context,
        bool anyMode,
        simfil::SchemaId rootSchema,
        EvaluationCache* valueCache = nullptr,
        bool collectDiagnostics = false)
    {
        ProgramKey key = std::make_tuple(std::string(expression), anyMode, rootSchema);
        if (valueCache && !collectDiagnostics) {
            if (auto cached = valueCache->values_.find(key); cached != valueCache->values_.end()) {
                Result result;
                result.values_ = cached->second;
                return result;
            }
        }

        auto found = programs_.find(key);
        if (found == programs_.end()) {
            Program program;
            auto compiled = simfil::compile(
                *environment_,
                expression,
                simfil::CompileOptions{
                    .any = anyMode,
                    .rewriteMode = simfil::RewriteMode::Schema,
                    .rootSchema = rootSchema,
                });
            if (!compiled) {
                program.error_ = compiled.error();
            }
            else {
                program.ast_ = std::move(*compiled);
            }
            found = programs_.emplace(key, std::move(program)).first;
        }

        if (found->second.error_) {
            return tl::unexpected(EvaluatorFailure{
                *found->second.error_,
                true,
            });
        }

        environment_->warnings.clear();
        environment_->traces.clear();
        Result result;
        auto values = simfil::eval(
            *environment_,
            *found->second.ast_,
            context,
            collectDiagnostics ? &result.diagnostics_ : nullptr);
        if (!values) {
            environment_->traces.clear();
            return tl::unexpected(EvaluatorFailure{
                std::move(values.error()),
                false,
            });
        }
        result.values_ = std::move(*values);
        result.traces_ = std::move(environment_->traces);
        environment_->traces.clear();
        // Trace-bearing expressions are intentionally re-evaluated so each
        // channel retains its own observable trace behavior.
        if (valueCache && !collectDiagnostics && result.traces_.empty()) {
            valueCache->values_.emplace(std::move(key), result.values_);
        }
        return result;
    }

    [[nodiscard]] std::shared_ptr<StringPool> const& strings() const { return strings_; }

private:
    struct Program
    {
        simfil::ASTPtr ast_;
        std::optional<simfil::Error> error_;
    };

    std::shared_ptr<StringPool> strings_;
    std::unique_ptr<simfil::Environment> environment_;
    std::map<ProgramKey, Program> programs_;
};

simfil::Value bindingValue(FeatureLayerFilterBinding const& binding)
{
    return std::visit(
        [](auto const& value) -> simfil::Value
        {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::monostate>) {
                return simfil::Value::null();
            }
            else if constexpr (std::is_same_v<Value, std::string>) {
                return simfil::Value::make(std::string(value));
            }
            else {
                return simfil::Value::make(value);
            }
        },
        binding);
}

struct BindingField
{
    simfil::StringId id_ = simfil::StringPool::Empty;
    simfil::Value value_ = simfil::Value::null();
};

struct EvaluatorContext
{
    std::unique_ptr<FilterEvaluator> evaluator_;
    std::vector<BindingField> bindingFields_;
};

tl::expected<EvaluatorContext, simfil::Error> makeFilterEvaluator(
    TileFeatureLayer const& sourceLayer,
    std::map<std::string, FeatureLayerFilterBinding> const& bindings)
{
    auto sourceStrings = std::dynamic_pointer_cast<StringPool>(sourceLayer.strings());
    if (!sourceStrings) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InternalError,
            fmt::format(
                "Feature layer '{}' does not use a mapget StringPool.",
                sourceLayer.stringPoolId()),
        });
    }

    auto strings = std::make_shared<StringPool>(*sourceStrings);
    auto environment = makeEnvironment(strings);
    installCompletionLayerSchema(*environment, sourceLayer.layerSchema(), strings);

    std::vector<BindingField> fields;
    fields.reserve(bindings.size());
    for (auto const& [name, binding] : bindings) {
        if (name.empty()) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InvalidArguments,
                "Filter binding names must not be empty.",
            });
        }
        auto id = strings->emplace(name);
        if (!id) {
            return tl::unexpected(id.error());
        }
        auto value = bindingValue(binding);
        environment->constants.insert_or_assign(name, value);
        fields.push_back(BindingField{*id, std::move(value)});
    }

    return EvaluatorContext{
        std::make_unique<FilterEvaluator>(std::move(strings), std::move(environment)),
        std::move(fields),
    };
}

simfil::ModelNode::Ptr
contextWithBindings(simfil::ModelNode const& root, std::span<BindingField const> bindings)
{
    if (bindings.empty()) {
        return simfil::ModelNode::Ptr(root);
    }
    auto context = simfil::model_ptr<simfil::OverlayNode>::make(simfil::Value::field(root));
    for (auto const& binding : bindings) {
        context->set(binding.id_, binding.value_);
    }
    return context;
}

void addBindings(
    simfil::model_ptr<simfil::OverlayNode>& context,
    std::span<BindingField const> bindings)
{
    for (auto const& binding : bindings) {
        context->set(binding.id_, binding.value_);
    }
}

LayerSchema::SearchQueryRequestedScope requestedScopeForNormalization(FeatureLayerFilterScope scope)
{
    switch (scope) {
    case FeatureLayerFilterScope::Feature: return LayerSchema::SearchQueryRequestedScope::Feature;
    case FeatureLayerFilterScope::Attribute:
        return LayerSchema::SearchQueryRequestedScope::Attribute;
    case FeatureLayerFilterScope::Auto: return LayerSchema::SearchQueryRequestedScope::Auto;
    case FeatureLayerFilterScope::Relation: break;
    }
    return LayerSchema::SearchQueryRequestedScope::Feature;
}

FeatureLayerFilterScope concreteFilterScope(LayerSchema::SearchQueryConcreteScope scope)
{
    return scope == LayerSchema::SearchQueryConcreteScope::Attribute ?
        FeatureLayerFilterScope::Attribute :
        FeatureLayerFilterScope::Feature;
}

Scope terminalScope(FeatureLayerFilterChannel const& channel)
{
    if (channel.group_) {
        return Scope::Group;
    }
    switch (channel.scope_) {
    case FeatureLayerFilterScope::Feature:
    case FeatureLayerFilterScope::Auto: return Scope::Feature;
    case FeatureLayerFilterScope::Attribute: return Scope::Attribute;
    case FeatureLayerFilterScope::Relation: return Scope::Relation;
    }
    return Scope::Feature;
}

tl::expected<void, simfil::Error>
validateRequest(TileFeatureLayer const& sourceLayer, FeatureLayerFilterRequest const& request)
{
    if (request.channels_.empty()) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InvalidArguments,
            "A filter request requires at least one channel.",
        });
    }

    std::set<std::string> channelIds;
    auto const layerInfo = sourceLayer.layerInfo();
    auto const canValidateTypes = layerInfo && !layerInfo->featureTypes_.empty();
    for (auto const& channel : request.channels_) {
        if (channel.channelId_.empty()) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InvalidArguments,
                "Filter channelId must not be empty.",
            });
        }
        if (!channelIds.insert(channel.channelId_).second) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InvalidArguments,
                fmt::format("Duplicate filter channelId '{}'.", channel.channelId_),
            });
        }
        if (channel.geometryName_ && channel.geometryName_->empty()) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InvalidArguments,
                fmt::format(
                    "Filter channel '{}' has an empty concrete geometry name.",
                    channel.channelId_),
            });
        }
        if (!channel.group_ && channel.scope_ == FeatureLayerFilterScope::Feature &&
            !channel.entryFields_.empty())
        {
            return tl::unexpected(simfil::Error{
                simfil::Error::InvalidArguments,
                fmt::format(
                    "Feature channel '{}' must put every projection in featureFields.",
                    channel.channelId_),
            });
        }
        if (channel.scope_ == FeatureLayerFilterScope::Relation) {
            if (!channel.relation_) {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InvalidArguments,
                    fmt::format(
                        "Relation channel '{}' requires relation options.",
                        channel.channelId_),
                });
            }
            if (channel.rewrite_) {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InvalidArguments,
                    fmt::format(
                        "Relation channel '{}' does not support search-query normalization.",
                        channel.channelId_),
                });
            }
        }
        else if (channel.relation_) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InvalidArguments,
                fmt::format(
                    "Non-relation channel '{}' must not carry relation options.",
                    channel.channelId_),
            });
        }
        if (channel.group_) {
            if (channel.scope_ != FeatureLayerFilterScope::Feature ||
                !channel.featureFields_.empty() || channel.entryFilter_)
            {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InvalidArguments,
                    fmt::format(
                        "Point-grid channel '{}' requires feature scope, empty featureFields, and "
                        "no entryFilter.",
                        channel.channelId_),
                });
            }
            auto const& origin = channel.group_->origin_;
            auto const& size = channel.group_->cellSize_;
            if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
                !std::isfinite(size.x) || !std::isfinite(size.y) || !std::isfinite(size.z) ||
                size.x <= 0.0 || size.y <= 0.0 || size.z <= 0.0)
            {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InvalidArguments,
                    fmt::format(
                        "Point-grid channel '{}' requires finite origin and positive finite "
                        "cellSize components.",
                        channel.channelId_),
                });
            }
        }
        for (auto const& featureType : channel.featureTypes_) {
            if (featureType.empty()) {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InvalidArguments,
                    fmt::format(
                        "Filter channel '{}' has an empty featureTypes entry.",
                        channel.channelId_),
                });
            }
            if (canValidateTypes && !layerInfo->getTypeInfo(featureType, false)) {
                return tl::unexpected(simfil::Error{
                    simfil::Error::InvalidArguments,
                    fmt::format(
                        "Filter channel '{}' requested unknown feature type '{}' for layer '{}'.",
                        channel.channelId_,
                        featureType,
                        layerInfo->layerId_),
                });
            }
        }
    }
    return {};
}

/** Native SIMFIL truth: only false, null, undefined, and no result reject. */
bool filterMatches(std::vector<simfil::Value> const& values)
{
    if (values.empty()) {
        return false;
    }
    auto const& value = values.front();
    if (value.isa(simfil::ValueType::Undef) || value.isa(simfil::ValueType::Null)) {
        return false;
    }
    if (value.isa(simfil::ValueType::Bool)) {
        return value.as<simfil::ValueType::Bool>();
    }
    return true;
}

void mergeTraces(
    std::map<std::string, simfil::Trace>& target,
    std::map<std::string, simfil::Trace> source)
{
    for (auto&& [name, trace] : source) {
        target[name].append(std::move(trace));
    }
}

struct IssueKey
{
    std::string channelId_;
    std::string expression_;
    Scope scope_ = Scope::Feature;
    std::string message_;

    auto operator<=>(IssueKey const&) const = default;
};

class IssueAccumulator
{
public:
    void
    add(std::string_view channelId,
        std::string_view expression,
        Scope scope,
        std::string message,
        uint64_t occurrences = 1)
    {
        auto& count = issues_[IssueKey{
            std::string(channelId),
            std::string(expression),
            scope,
            std::move(message),
        }];
        if (std::numeric_limits<uint64_t>::max() - count < occurrences) {
            count = std::numeric_limits<uint64_t>::max();
        }
        else {
            count += occurrences;
        }
    }

    void install(TileSubsetLayer& layer) const
    {
        for (auto const& [issue, count] : issues_) {
            layer.addIssue(FilterIssue{
                issue.channelId_,
                issue.expression_,
                issue.scope_,
                issue.message_,
                count,
            });
        }
    }

    [[nodiscard]] std::vector<FilterIssue> values() const
    {
        std::vector<FilterIssue> result;
        result.reserve(issues_.size());
        for (auto const& [issue, count] : issues_) {
            result.push_back(FilterIssue{
                issue.channelId_,
                issue.expression_,
                issue.scope_,
                issue.message_,
                count,
            });
        }
        return result;
    }

private:
    std::map<IssueKey, uint64_t> issues_;
};

struct FeatureCandidate
{
    model_ptr<Feature> feature_;
    std::vector<simfil::Value> featureValues_;
};

struct AttributeCandidate
{
    model_ptr<Feature> feature_;
    model_ptr<Attribute> attribute_;
    model_ptr<Validity> validity_;
    std::string attributeLayer_;
    uint32_t attributeIndex_ = AttributeValidityEntry::InvalidAttributeIndex;
    bool hasValidity_ = false;
    uint32_t validityIndex_ = 0;
    uint32_t validityCount_ = 1;
    std::vector<simfil::Value> hostValues_;
    std::vector<simfil::Value> entryValues_;
};

struct RelationRoot
{
    model_ptr<Feature> feature_;
    size_t rootOrdinal_ = 0;
    bool exact_ = false;
};

struct ChannelState
{
    FeatureLayerFilterChannel definition_;
    Scope terminalScope_ = Scope::Feature;
    model_ptr<TileSubsetChannel> output_;
    std::set<std::string> featureTypes_;
    bool filterCompilationFailed_ = false;
    std::vector<FeatureCandidate> featureCandidates_;
    std::vector<AttributeCandidate> attributeCandidates_;
    std::vector<FeatureLayerPointGroupMember> pointGroupMembers_;
    std::vector<RelationRoot> relationRoots_;
    std::vector<FeatureLayerRelationDescriptor> relationDescriptors_;
};

bool featureTypeAllowed(model_ptr<Feature> const& feature, ChannelState const& channel)
{
    return channel.featureTypes_.empty() ||
        channel.featureTypes_.contains(std::string(feature->typeId()));
}

tl::expected<bool, EvaluatorFailure> evaluateFilter(
    FilterEvaluator& evaluator,
    std::optional<std::string> const& expression,
    simfil::ModelNode const& context,
    simfil::SchemaId schema,
    std::map<std::string, simfil::Trace>& traces,
    simfil::Diagnostics* diagnostics = nullptr,
    FilterEvaluator::EvaluationCache* valueCache = nullptr)
{
    if (!expression) {
        return true;
    }
    auto result =
        evaluator.evaluate(*expression, context, true, schema, valueCache, diagnostics != nullptr);
    if (!result) {
        return tl::unexpected(result.error());
    }
    mergeTraces(traces, std::move(result->traces_));
    // Raw SIMFIL diagnostics can only be merged when evaluation reused the
    // same compiled AST. Schema-specific compilation may give one textual
    // entryFilter a different expression index; those evaluations are still
    // represented by channel-qualified FilterIssues and must not corrupt the
    // AST-indexed diagnostic aggregate.
    if (diagnostics &&
        (diagnostics->exprIndex_.empty() ||
         std::ranges::equal(diagnostics->exprIndex_, result->diagnostics_.exprIndex_)))
    {
        diagnostics->append(result->diagnostics_);
    }
    return filterMatches(result->values_);
}

simfil::Value scalarProjection(
    std::vector<simfil::Value> values,
    ChannelState const& channel,
    std::string_view expression,
    Scope scope,
    IssueAccumulator& issues)
{
    if (values.empty() || values.front().isa(simfil::ValueType::Undef) ||
        values.front().isa(simfil::ValueType::Null))
    {
        return simfil::Value::null();
    }

    auto value = std::move(values.front());
    if (value.isa(simfil::ValueType::Bool) || value.isa(simfil::ValueType::Int) ||
        value.isa(simfil::ValueType::Float))
    {
        return value;
    }
    if (value.isa(simfil::ValueType::String)) {
        return simfil::Value::make(value.as<simfil::ValueType::String>());
    }

    issues.add(
        channel.definition_.channelId_,
        expression,
        scope,
        fmt::format(
            "Projected expression returned unsupported {} value; stored null.",
            simfil::valueType2String(value.type)));
    return simfil::Value::null();
}

std::vector<simfil::Value> evaluateFields(
    FilterEvaluator& evaluator,
    ChannelState const& channel,
    simfil::ModelNode const& context,
    simfil::SchemaId schema,
    std::span<std::string const> expressions,
    Scope scope,
    std::map<std::string, simfil::Trace>& traces,
    IssueAccumulator& issues,
    FilterEvaluator::EvaluationCache* valueCache = nullptr)
{
    std::vector<simfil::Value> values;
    values.reserve(expressions.size());
    for (auto const& expression : expressions) {
        auto result = evaluator.evaluate(expression, context, false, schema, valueCache);
        if (!result) {
            issues.add(
                channel.definition_.channelId_,
                expression,
                scope,
                fmt::format(
                    "{} failure: {}; stored null.",
                    result.error().compilation_ ?
                        "Expression compilation" :
                        "Expression evaluation",
                    result.error().error_.message));
            values.push_back(simfil::Value::null());
            continue;
        }
        mergeTraces(traces, std::move(result->traces_));
        values.push_back(
            scalarProjection(std::move(result->values_), channel, expression, scope, issues));
    }
    return values;
}

void rejectFilterCandidate(
    ChannelState& channel,
    std::optional<std::string> const& expression,
    Scope scope,
    EvaluatorFailure const& failure,
    IssueAccumulator& issues)
{
    issues.add(
        channel.definition_.channelId_,
        expression.value_or("true"),
        scope,
        fmt::format(
            "{} failure: {}",
            failure.compilation_ ? "Filter compilation" : "Filter evaluation",
            failure.error_.message));
    if (failure.compilation_) {
        channel.filterCompilationFailed_ = true;
        channel.featureCandidates_.clear();
        channel.attributeCandidates_.clear();
        channel.pointGroupMembers_.clear();
        channel.relationRoots_.clear();
        channel.relationDescriptors_.clear();
    }
}

simfil::model_ptr<simfil::OverlayNode> makeAttributeContext(
    model_ptr<Attribute> const& attribute,
    model_ptr<Feature> const& feature,
    std::string_view layerName,
    uint32_t attributeIndex,
    bool hasValidity,
    uint32_t validityIndex,
    uint32_t validityCount,
    std::span<BindingField const> bindings)
{
    auto context = simfil::model_ptr<simfil::OverlayNode>::make(simfil::Value::field(*attribute));
    context->set(StringPool::OverlayNameStr, simfil::Value::make(std::string(attribute->name())));
    context->set(StringPool::OverlayFeatureStr, simfil::Value::field(*feature));
    context->set(StringPool::OverlayLayerStr, simfil::Value::make(std::string(layerName)));
    context->set(
        StringPool::OverlayAttributeIndexStr,
        simfil::Value::make(static_cast<int64_t>(attributeIndex)));
    context->set(
        StringPool::OverlayValidityIndexStr,
        simfil::Value::make(static_cast<int64_t>(validityIndex)));
    context->set(
        StringPool::OverlayValidityCountStr,
        simfil::Value::make(static_cast<int64_t>(validityCount)));
    context->set(StringPool::OverlayHasValidityStr, simfil::Value::make(hasValidity));
    addBindings(context, bindings);
    return context;
}

bool geometrySelected(
    model_ptr<Geometry> const& geometry,
    uint32_t geometryTypes,
    std::optional<std::string> const& geometryName)
{
    auto const typeIndex = static_cast<std::underlying_type_t<GeomType>>(geometry->geomType());
    if (typeIndex >= 32 || (geometryTypes & (uint32_t{1} << typeIndex)) == 0) {
        return false;
    }
    if (!geometryName) {
        return true;
    }
    auto const candidateName = geometry->name();
    return candidateName && *candidateName == *geometryName;
}

std::optional<int64_t> pointGroupCoordinate(double coordinate, double origin, double cellSize)
{
    auto const value = std::floor((coordinate - origin) / cellSize);
    if (!std::isfinite(value) || value < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
        value > static_cast<double>(std::numeric_limits<int64_t>::max()))
    {
        return std::nullopt;
    }
    return static_cast<int64_t>(value);
}

void collectPointGroupMembers(
    model_ptr<Feature> const& feature,
    size_t channelIndex,
    ChannelState& channel,
    IssueAccumulator& issues)
{
    auto const& definition = channel.definition_;
    auto const& group = *definition.group_;
    std::map<FeatureLayerPointGroupKey, FeatureLayerPointGroupMember> membersByCell;

    auto geometries = feature->geomOrNull();
    if (!geometries) {
        return;
    }
    uint32_t geometryOrdinal = 0;
    geometries->forEachGeometry(
        [&](model_ptr<Geometry> const& geometry)
        {
            auto const thisGeometryOrdinal = geometryOrdinal++;
            if (geometry->geomType() != GeomType::Points ||
                !geometrySelected(geometry, definition.geometryTypes_, definition.geometryName_))
            {
                return true;
            }

            for (uint32_t pointOrdinal = 0; pointOrdinal < geometry->numPoints(); ++pointOrdinal) {
                auto const point = geometry->pointAt(pointOrdinal);
                auto x = pointGroupCoordinate(point.x, group.origin_.x, group.cellSize_.x);
                auto y = pointGroupCoordinate(point.y, group.origin_.y, group.cellSize_.y);
                auto z = pointGroupCoordinate(point.z, group.origin_.z, group.cellSize_.z);
                if (!x || !y || !z) {
                    issues.add(
                        definition.channelId_,
                        "<point-grid-key>",
                        Scope::Group,
                        "Point-grid coordinate exceeds signed 64-bit range.");
                    continue;
                }
                FeatureLayerPointGroupKey key{
                    *x,
                    *y,
                    *z,
                };
                // Geometry and point traversal are ordinal, so the first
                // insertion is the fixed representative candidate for this
                // feature in this cell.
                membersByCell.try_emplace(
                    key,
                    FeatureLayerPointGroupMember{
                        channelIndex,
                        key,
                        feature,
                        point,
                        thisGeometryOrdinal,
                        pointOrdinal,
                        geometry->name() ? std::optional<std::string>{*geometry->name()} :
                                           std::nullopt,
                    });
            }
            return true;
        });

    for (auto& [_, member] : membersByCell) {
        channel.pointGroupMembers_.push_back(std::move(member));
    }
}

void collectStoredRelationDescriptors(
    TileFeatureLayer const& sourceLayer,
    size_t channelIndex,
    ChannelState& channel,
    IssueAccumulator& issues)
{
    if (!channel.definition_.relation_) {
        return;
    }

    std::optional<std::regex> namePattern;
    if (auto const& pattern = channel.definition_.relation_->relationNamePattern_) {
        try {
            namePattern.emplace(*pattern, std::regex::ECMAScript);
        }
        catch (std::regex_error const& error) {
            issues.add(
                channel.definition_.channelId_,
                *pattern,
                Scope::Relation,
                fmt::format(
                    "Relation-name regular expression compilation failed: {}",
                    error.what()));
            return;
        }
    }

    std::deque<RelationRoot> pending(channel.relationRoots_.begin(), channel.relationRoots_.end());
    std::set<std::string> visitedFeatures;
    while (!pending.empty()) {
        auto current = std::move(pending.front());
        pending.pop_front();
        if (!current.feature_) {
            continue;
        }
        auto const sourceIdentity = current.feature_->id()->toString();
        if (!visitedFeatures.insert(sourceIdentity).second) {
            continue;
        }

        for (uint32_t relationOrdinal = 0; relationOrdinal < current.feature_->numRelations();
             ++relationOrdinal)
        {
            auto relation = current.feature_->getRelation(relationOrdinal);
            if (!relation ||
                (namePattern &&
                 !std::regex_match(relation->name().begin(), relation->name().end(), *namePattern)))
            {
                continue;
            }
            auto targetId = relation->target();
            if (!targetId) {
                issues.add(
                    channel.definition_.channelId_,
                    "<relation-target>",
                    Scope::Relation,
                    fmt::format(
                        "Relation {} on feature '{}' has no target identity.",
                        relationOrdinal,
                        sourceIdentity));
                continue;
            }

            auto localTarget = sourceLayer.find(targetId->typeId(), targetId->keyValuePairs());
            channel.relationDescriptors_.push_back(FeatureLayerRelationDescriptor{
                .channelIndex_ = channelIndex,
                .source_ = current.feature_,
                .relation_ = relation,
                .relationOrdinal_ = relationOrdinal,
                .targetTypeId_ = std::string(targetId->typeId()),
                .targetFeatureId_ = castToKeyValue(targetId->keyValuePairs()),
                .target_ = localTarget,
                .targetTileKey_ = localTarget ?
                    std::optional<MapTileKey>{MapTileKey(localTarget->model())} :
                    std::nullopt,
                .rootOrdinal_ = current.rootOrdinal_,
                .exactRoot_ = current.exact_,
            });

            if (channel.definition_.relation_->recursive_ && localTarget) {
                pending.push_back(RelationRoot{
                    localTarget,
                    current.rootOrdinal_,
                    current.exact_,
                });
            }
        }
    }
}

model_ptr<Geometry> copyGeometry(
    TileSubsetLayer& target,
    model_ptr<Geometry> const& source,
    bool preserveGltfNodeIndex = true,
    bool* downgradedGltfNodeIndex = nullptr)
{
    auto const targetType = source->geomType() == GeomType::GltfNodeIndex &&
            !preserveGltfNodeIndex ?
        GeomType::AABB :
        source->geomType();
    auto copied = target.newGeometry(targetType, std::max<size_t>(1, source->numPoints()), false);
    switch (source->geomType()) {
    case GeomType::AABB: copied->setAabb(source->aabbOrigin(), source->aabbSize()); break;
    case GeomType::GltfNodeIndex:
        if (preserveGltfNodeIndex) {
            copied->setGltfNodeIndex(source->gltfNodeIndex());
            copied->setGltfNodeBounds(source->gltfNodeAabbOrigin(), source->gltfNodeAabbSize());
        }
        else {
            copied->setAabb(source->gltfNodeAabbOrigin(), source->gltfNodeAabbSize());
            if (downgradedGltfNodeIndex) {
                *downgradedGltfNodeIndex = true;
            }
        }
        break;
    default:
        source->forEachPoint(
            [&](Point const& point)
            {
                copied->append(point);
                return true;
            });
        if (source->geomType() == GeomType::Polygon && source->numPolygonRings() > 1) {
            std::vector<uint32_t> ringStarts;
            ringStarts.reserve(source->numPolygonRings());
            for (uint32_t index = 0; index < source->numPolygonRings(); ++index) {
                ringStarts.push_back(source->polygonRingStart(index));
            }
            copied->setPolygonRingStarts(ringStarts);
        }
        break;
    }
    copied->setName(source->name());
    return copied;
}

model_ptr<GeometryCollection> copyGeometryCollection(
    TileSubsetLayer& target,
    model_ptr<GeometryCollection> const& source,
    uint32_t geometryTypes,
    std::optional<std::string> const& geometryName,
    bool preserveGltfNodeIndex = true,
    bool* downgradedGltfNodeIndex = nullptr)
{
    auto copied = target.newGeometryCollection(
        source ? std::max<size_t>(1, source->numGeometries()) : 1,
        false);
    if (!source) {
        return copied;
    }
    source->forEachGeometry(
        [&](model_ptr<Geometry> const& geometry)
        {
            if (geometrySelected(geometry, geometryTypes, geometryName)) {
                copied->addGeometry(
                    copyGeometry(target, geometry, preserveGltfNodeIndex, downgradedGltfNodeIndex));
            }
            return true;
        });
    return copied;
}

model_ptr<GeometryCollection> copySelfContainedGeometry(
    TileSubsetLayer& target,
    SelfContainedGeometry const& source,
    std::optional<std::string_view> sourceName,
    uint32_t geometryTypes,
    std::optional<std::string> const& geometryName)
{
    auto copied = target.newGeometryCollection(1, false);
    auto const typeIndex = static_cast<std::underlying_type_t<GeomType>>(source.geomType_);
    if (source.points_.empty() || typeIndex >= 32 ||
        (geometryTypes & (uint32_t{1} << typeIndex)) == 0 ||
        (geometryName && (!sourceName || *sourceName != *geometryName)))
    {
        return copied;
    }

    auto geometry =
        target.newGeometry(source.geomType_, std::max<size_t>(1, source.points_.size()), false);
    switch (source.geomType_) {
    case GeomType::AABB:
        if (source.points_.size() >= 2) {
            geometry->setAabb(source.points_[0], source.points_[1]);
        }
        break;
    case GeomType::GltfNodeIndex: return copied;
    default:
        for (auto const& point : source.points_) {
            geometry->append(point);
        }
        if (source.geomType_ == GeomType::Polygon && source.polygonRingStarts_.size() > 1) {
            geometry->setPolygonRingStarts(source.polygonRingStarts_);
        }
        break;
    }
    geometry->setName(sourceName);
    copied->addGeometry(geometry);
    return copied;
}

model_ptr<GeometryCollection> copyAttributeGeometry(
    TileSubsetLayer& target,
    AttributeCandidate const& candidate,
    ChannelState const& channel,
    IssueAccumulator& issues,
    uint32_t* transitionPivotIndex)
{
    if (transitionPivotIndex) {
        *transitionPivotIndex = AttributeValidityEntry::InvalidTransitionPivotIndex;
    }
    if (!candidate.hasValidity_ || !candidate.validity_) {
        return copyGeometryCollection(
            target,
            candidate.feature_->geomOrNull(),
            channel.definition_.geometryTypes_,
            channel.definition_.geometryName_);
    }

    std::string error;
    try {
        uint32_t computedTransitionPivotIndex = AttributeValidityEntry::InvalidTransitionPivotIndex;
        auto computed = candidate.validity_->computeGeometry(
            candidate.feature_->geomOrNull(),
            &error,
            &computedTransitionPivotIndex);
        if (!error.empty()) {
            issues.add(channel.definition_.channelId_, "<geometry>", Scope::Attribute, error);
        }
        auto copied = copySelfContainedGeometry(
            target,
            computed,
            candidate.validity_->geometryName(),
            channel.definition_.geometryTypes_,
            channel.definition_.geometryName_);
        bool retainedTransitionLine = false;
        copied->forEachGeometry(
            [&](auto const& geometry)
            {
                retainedTransitionLine = geometry && geometry->geomType() == GeomType::Line;
                return !retainedTransitionLine;
            });
        if (transitionPivotIndex && retainedTransitionLine) {
            *transitionPivotIndex = computedTransitionPivotIndex;
        }
        return copied;
    }
    catch (std::exception const& exception) {
        issues.add(
            channel.definition_.channelId_,
            "<geometry>",
            Scope::Attribute,
            fmt::format("Could not compute validity geometry: {}", exception.what()));
        return target.newGeometryCollection(1, false);
    }
}

model_ptr<FeatureId> copyFeatureId(TileSubsetLayer& target, model_ptr<FeatureId> const& source)
{
    return target.newFeatureId(source->typeId(), source->keyValuePairs(), source->externalMapId());
}

std::vector<simfil::ModelNode::Ptr>
materializeValues(TileSubsetLayer& target, std::vector<simfil::Value> const& values)
{
    std::vector<simfil::ModelNode::Ptr> result;
    result.reserve(values.size());
    for (auto const& value : values) {
        result.push_back(target.materializeValue(value));
    }
    return result;
}

void materializeChannel(TileSubsetLayer& target, ChannelState& channel, IssueAccumulator& issues)
{
    if (channel.filterCompilationFailed_) {
        return;
    }

    for (auto const& candidate : channel.featureCandidates_) {
        auto values = materializeValues(target, candidate.featureValues_);
        channel.output_->newFeatureEntry(
            copyFeatureId(target, candidate.feature_->id()),
            copyGeometryCollection(
                target,
                candidate.feature_->geomOrNull(),
                channel.definition_.geometryTypes_,
                channel.definition_.geometryName_),
            values);
    }

    for (auto const& candidate : channel.attributeCandidates_) {
        auto hostValues = materializeValues(target, candidate.hostValues_);
        auto entryValues = materializeValues(target, candidate.entryValues_);
        uint32_t transitionPivotIndex = AttributeValidityEntry::InvalidTransitionPivotIndex;
        auto geometry =
            copyAttributeGeometry(target, candidate, channel, issues, &transitionPivotIndex);
        auto geometryDescriptionType = candidate.hasValidity_ && candidate.validity_ ?
            candidate.validity_->geometryDescriptionType() :
            ValidityData::NoGeometry;
        model_ptr<FeatureId> transitionFromFeatureId;
        model_ptr<FeatureId> transitionToFeatureId;
        auto transitionFromConnectedEnd = ValidityData::Start;
        auto transitionToConnectedEnd = ValidityData::Start;
        if (geometryDescriptionType == ValidityData::FeatureTransition) {
            if (transitionPivotIndex != AttributeValidityEntry::InvalidTransitionPivotIndex) {
                auto const from = candidate.validity_->transitionFromFeatureId();
                auto const to = candidate.validity_->transitionToFeatureId();
                auto const fromEnd = candidate.validity_->transitionFromConnectedEnd();
                auto const toEnd = candidate.validity_->transitionToConnectedEnd();
                if (from && to && fromEnd && toEnd) {
                    transitionFromFeatureId = copyFeatureId(target, from);
                    transitionToFeatureId = copyFeatureId(target, to);
                    transitionFromConnectedEnd = *fromEnd;
                    transitionToConnectedEnd = *toEnd;
                }
            }
            if (!transitionFromFeatureId || !transitionToFeatureId) {
                geometryDescriptionType = ValidityData::NoGeometry;
                transitionPivotIndex = AttributeValidityEntry::InvalidTransitionPivotIndex;
            }
        }
        channel.output_->newAttributeValidityEntry(
            copyFeatureId(target, candidate.feature_->id()),
            geometry,
            candidate.attributeIndex_,
            candidate.hasValidity_,
            candidate.validityIndex_,
            candidate.validityCount_,
            hostValues,
            entryValues,
            candidate.attributeLayer_,
            candidate.attribute_->name(),
            geometryDescriptionType,
            transitionFromFeatureId,
            transitionFromConnectedEnd,
            transitionToFeatureId,
            transitionToConnectedEnd,
            transitionPivotIndex);
    }
}

std::string stableFeatureIdentity(model_ptr<Feature> const& feature)
{
    return fmt::format("{}:{}", MapTileKey(feature->model()).toString(), feature->id()->toString());
}

std::string directedRelationIdentity(FeatureLayerRelationDescriptor const& descriptor)
{
    return fmt::format(
        "{}#{}",
        stableFeatureIdentity(descriptor.source_),
        descriptor.relationOrdinal_);
}

simfil::model_ptr<simfil::OverlayNode> makeRelationContext(
    FeatureLayerRelationDescriptor const& descriptor,
    bool twoway,
    std::span<BindingField const> bindings)
{
    auto context =
        simfil::model_ptr<simfil::OverlayNode>::make(simfil::Value::field(*descriptor.relation_));
    context->set(StringPool::OverlaySourceStr, simfil::Value::field(*descriptor.source_));
    context->set(StringPool::OverlayTargetStr, simfil::Value::field(*descriptor.target_));
    context->set(StringPool::OverlayTwowayStr, simfil::Value::make(twoway));
    context->set(
        StringPool::OverlayRelationIndexStr,
        simfil::Value::make(static_cast<int64_t>(descriptor.relationOrdinal_)));
    addBindings(context, bindings);
    return context;
}

model_ptr<GeometryCollection> copyRelationEffectiveGeometry(
    TileSubsetLayer& target,
    model_ptr<Feature> const& feature,
    model_ptr<MultiValidity> const& validities,
    ChannelState const& channel,
    std::string_view endpoint,
    IssueAccumulator& issues,
    model_ptr<GeometryCollection> const& fallback)
{
    if (!validities || validities->size() == 0) {
        return fallback;
    }

    auto result = target.newGeometryCollection(std::max<uint32_t>(1, validities->size()), false);
    for (uint32_t index = 0; index < validities->size(); ++index) {
        auto node = validities->at(index);
        if (!node) {
            continue;
        }
        auto owner = std::dynamic_pointer_cast<TileFeatureLayer const>(node->owningModel());
        auto validity = owner ? owner->resolve<Validity>(*node) : model_ptr<Validity>{};
        if (!validity) {
            continue;
        }
        try {
            std::string error;
            auto computed = validity->computeGeometry(feature->geomOrNull(), &error);
            if (!error.empty()) {
                issues.add(
                    channel.definition_.channelId_,
                    "<relation-geometry>",
                    Scope::Relation,
                    fmt::format("{} endpoint validity: {}", endpoint, error));
            }
            auto copied = copySelfContainedGeometry(
                target,
                computed,
                validity->geometryName(),
                channel.definition_.geometryTypes_,
                channel.definition_.geometryName_);
            copied->forEachGeometry(
                [&](model_ptr<Geometry> const& geometry)
                {
                    result->addGeometry(geometry);
                    return true;
                });
        }
        catch (std::exception const& exception) {
            issues.add(
                channel.definition_.channelId_,
                "<relation-geometry>",
                Scope::Relation,
                fmt::format(
                    "Could not compute {} endpoint validity geometry: {}",
                    endpoint,
                    exception.what()));
        }
    }
    return result;
}

bool southWestOwnerLess(
    MapTileKey const& left,
    std::string_view leftFeatureIdentity,
    MapTileKey const& right,
    std::string_view rightFeatureIdentity)
{
    auto const [leftLongitude, leftLatitude] = left.tileId_.southWestWgs84();
    auto const [rightLongitude, rightLatitude] = right.tileId_.southWestWgs84();
    return std::make_tuple(
               leftLatitude,
               leftLongitude,
               left.tileId_.level(),
               left.mapId_,
               left.layerId_,
               std::string(leftFeatureIdentity)) <
        std::make_tuple(
               rightLatitude,
               rightLongitude,
               right.tileId_.level(),
               right.mapId_,
               right.layerId_,
               std::string(rightFeatureIdentity));
}

std::optional<size_t> exactRootOrdinal(
    model_ptr<Feature> const& feature,
    TileSubsetLayer const& outputLayer,
    std::span<FeatureLayerFilterRoot const> exactRoots)
{
    if (!feature) {
        return std::nullopt;
    }
    auto const featureKey = MapTileKey(feature->model());
    if (featureKey.mapId_ != outputLayer.id().mapId_ ||
        featureKey.layerId_ != outputLayer.id().layerId_)
    {
        return std::nullopt;
    }
    auto const featureId = feature->id();
    if (!featureId) {
        return std::nullopt;
    }
    auto const featureIdParts = castToKeyValue(featureId->keyValuePairs());
    std::optional<size_t> firstOrdinal;
    for (auto const& root : exactRoots) {
        auto const identityMatches = !root.canonicalFeatureId_.empty() ?
            root.canonicalFeatureId_ == featureId->toString() :
            root.typeId_ == featureId->typeId() && root.featureId_ == featureIdParts;
        if (root.tileId_ == featureKey.tileId_ && identityMatches &&
            (!firstOrdinal || root.requestOrdinal_ < *firstOrdinal))
        {
            firstOrdinal = root.requestOrdinal_;
        }
    }
    return firstOrdinal;
}

}  // namespace

tl::expected<FeatureLayerFilterSourceResult, simfil::Error> FeatureLayerFilterRequest::filterSource(
    TileFeatureLayer const& sourceLayer,
    bool materializeOutput,
    std::span<FeatureLayerFilterRoot const> exactRoots,
    FeatureLayerFilterCancellationCheck const& cancellationCheck) const
{
    if (auto valid = validateRequest(sourceLayer, *this); !valid) {
        return tl::unexpected(valid.error());
    }
    if (sourceLayer.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InvalidArguments,
            "Source feature count exceeds the subset dependency representation.",
        });
    }

    auto evaluatorContext = makeFilterEvaluator(sourceLayer, bindings_);
    if (!evaluatorContext) {
        return tl::unexpected(evaluatorContext.error());
    }
    auto& evaluator = *evaluatorContext->evaluator_;

    TileSubsetLayer::Ptr resultLayer;
    if (materializeOutput) {
        resultLayer = std::make_shared<TileSubsetLayer>(
            sourceLayer.tileId(),
            sourceLayer.stringPoolId(),
            sourceLayer.mapId(),
            sourceLayer.layerInfo(),
            sourceLayer.strings(),
            filterId_,
            generation_,
            deliveryEpoch_);
        resultLayer->setGeometryAnchor(sourceLayer.geometryAnchor());
        resultLayer->adoptSourceInfo(sourceLayer);
        resultLayer
            ->addDependency(MapTileKey(sourceLayer), static_cast<uint32_t>(sourceLayer.size()));
        if (auto const& attachmentName = sourceLayer.glbAttachmentName()) {
            resultLayer->setGlbAttachmentName(*attachmentName);
        }
    }

    IssueAccumulator issues;
    std::vector<ChannelState> channels;
    channels.reserve(channels_.size());
    for (auto const& requested : channels_) {
        auto effective = requested;
        bool normalizationFailed = false;
        if (requested.rewrite_ || requested.scope_ == FeatureLayerFilterScope::Auto) {
            auto registry = sourceLayer.layerSchema();
            if (registry) {
                auto normalized = registry->normalizeSearchQuery(
                    requested.entryFilter_.value_or("true"),
                    requestedScopeForNormalization(requested.scope_));
                if (!normalized) {
                    normalizationFailed = true;
                    issues.add(
                        requested.channelId_,
                        requested.entryFilter_.value_or("true"),
                        requested.scope_ == FeatureLayerFilterScope::Attribute ?
                            Scope::Attribute :
                            Scope::Feature,
                        fmt::format(
                            "Search-query normalization failed: {}",
                            normalized.error().message));
                }
                else {
                    effective.entryFilter_ = std::move(normalized->normalizedQuery_);
                    effective.scope_ = concreteFilterScope(normalized->concreteScope_);
                }
            }
            else if (effective.scope_ == FeatureLayerFilterScope::Auto) {
                effective.scope_ = FeatureLayerFilterScope::Feature;
            }
        }

        auto scope = terminalScope(effective);
        model_ptr<TileSubsetChannel> output;
        if (resultLayer) {
            output = resultLayer->newChannel(
                effective.channelId_,
                scope,
                effective.geometryTypes_,
                effective.geometryName_ ?
                    std::optional<std::string_view>{*effective.geometryName_} :
                    std::nullopt,
                effective.featureFields_,
                effective.entryFields_);
        }
        channels.push_back(ChannelState{
            std::move(effective),
            scope,
            output,
            std::set<std::string>(requested.featureTypes_.begin(), requested.featureTypes_.end()),
            normalizationFailed,
        });
    }

    std::map<std::string, simfil::Trace> traces;
    // A raw SIMFIL Diagnostics object is tied to one compiled AST. Preserve
    // the legacy /search contract only for a single channel's entry filter;
    // failures and diagnostics from other independent expressions are
    // delivered through channel-qualified FilterIssue records.
    simfil::Diagnostics diagnostics;
    auto const collectEntryFilterDiagnostics = channels_.size() == 1;
    CancellationProbe cancellation(cancellationCheck);

    // Source-major traversal: every feature is visited once and then offered
    // to every applicable channel in stable request order.
    size_t sourceFeatureOrdinal = 0;
    for (auto const& feature : sourceLayer) {
        if (cancellation.boundary()) {
            break;
        }
        auto const thisFeatureOrdinal = sourceFeatureOrdinal++;
        auto const featureSchema = static_cast<simfil::ModelNode const&>(*feature).schema();
        auto featureContext = contextWithBindings(*feature, evaluatorContext->bindingFields_);
        FilterEvaluator::EvaluationCache featureEvaluations;

        for (size_t channelIndex = 0; channelIndex < channels.size(); ++channelIndex) {
            auto& channel = channels[channelIndex];
            if (channel.filterCompilationFailed_ || !featureTypeAllowed(feature, channel)) {
                continue;
            }
            if (!materializeOutput && !channel.definition_.group_) {
                continue;
            }
            if (channel.definition_.scope_ == FeatureLayerFilterScope::Relation &&
                !exactRoots.empty()) {
                continue;
            }

            auto hostMatch = evaluateFilter(
                evaluator,
                channel.definition_.featureFilter_,
                *featureContext,
                featureSchema,
                traces,
                nullptr,
                &featureEvaluations);
            if (!hostMatch) {
                rejectFilterCandidate(
                    channel,
                    channel.definition_.featureFilter_,
                    Scope::Feature,
                    hostMatch.error(),
                    issues);
                continue;
            }
            if (!*hostMatch) {
                continue;
            }

            if (channel.definition_.group_) {
                collectPointGroupMembers(feature, channelIndex, channel, issues);
                continue;
            }

            if (channel.definition_.scope_ == FeatureLayerFilterScope::Relation) {
                channel.relationRoots_.push_back(RelationRoot{
                    feature,
                    thisFeatureOrdinal,
                    false,
                });
                continue;
            }

            if (channel.definition_.scope_ == FeatureLayerFilterScope::Feature) {
                auto entryMatch = evaluateFilter(
                    evaluator,
                    channel.definition_.entryFilter_,
                    *featureContext,
                    featureSchema,
                    traces,
                    collectEntryFilterDiagnostics ? &diagnostics : nullptr,
                    &featureEvaluations);
                if (!entryMatch) {
                    rejectFilterCandidate(
                        channel,
                        channel.definition_.entryFilter_,
                        Scope::Feature,
                        entryMatch.error(),
                        issues);
                    continue;
                }
                if (!*entryMatch) {
                    continue;
                }
                channel.featureCandidates_.push_back(FeatureCandidate{
                    feature,
                    evaluateFields(
                        evaluator,
                        channel,
                        *featureContext,
                        featureSchema,
                        channel.definition_.featureFields_,
                        Scope::Feature,
                        traces,
                        issues,
                        &featureEvaluations),
                });
                continue;
            }

            auto attributeLayers = feature->attributeLayersOrNull();
            if (!attributeLayers) {
                continue;
            }
            std::optional<std::vector<simfil::Value>> hostValues;
            uint32_t attributeIndex = 0;
            attributeLayers->forEachLayer(
                [&](std::string_view layerName, model_ptr<AttributeLayer> const& attributeLayer)
                {
                    return attributeLayer->forEachAttribute(
                        [&](model_ptr<Attribute> const& attribute)
                        {
                            if (cancellation.periodic()) {
                                return false;
                            }
                            auto const thisAttributeIndex = attributeIndex++;
                            auto const attributeSchema =
                                static_cast<simfil::ModelNode const&>(*attribute).schema();

                            auto evaluateAttributeCandidate =
                                [&](model_ptr<Validity> const& validity,
                                    bool hasValidity,
                                    uint32_t validityIndex,
                                    uint32_t validityCount)
                            {
                                if (channel.filterCompilationFailed_) {
                                    return;
                                }
                                auto context = makeAttributeContext(
                                    attribute,
                                    feature,
                                    layerName,
                                    thisAttributeIndex,
                                    hasValidity,
                                    validityIndex,
                                    validityCount,
                                    evaluatorContext->bindingFields_);
                                FilterEvaluator::EvaluationCache attributeEvaluations;
                                auto entryMatch = evaluateFilter(
                                    evaluator,
                                    channel.definition_.entryFilter_,
                                    *context,
                                    attributeSchema,
                                    traces,
                                    collectEntryFilterDiagnostics ? &diagnostics : nullptr,
                                    &attributeEvaluations);
                                if (!entryMatch) {
                                    rejectFilterCandidate(
                                        channel,
                                        channel.definition_.entryFilter_,
                                        Scope::Attribute,
                                        entryMatch.error(),
                                        issues);
                                    return;
                                }
                                if (!*entryMatch) {
                                    return;
                                }
                                if (!hostValues) {
                                    hostValues = evaluateFields(
                                        evaluator,
                                        channel,
                                        *featureContext,
                                        featureSchema,
                                        channel.definition_.featureFields_,
                                        Scope::Feature,
                                        traces,
                                        issues,
                                        &featureEvaluations);
                                }
                                channel.attributeCandidates_.push_back(AttributeCandidate{
                                    feature,
                                    attribute,
                                    validity,
                                    std::string(layerName),
                                    thisAttributeIndex,
                                    hasValidity,
                                    validityIndex,
                                    validityCount,
                                    *hostValues,
                                    evaluateFields(
                                        evaluator,
                                        channel,
                                        *context,
                                        attributeSchema,
                                        channel.definition_.entryFields_,
                                        Scope::Attribute,
                                        traces,
                                        issues,
                                        &attributeEvaluations),
                                });
                            };

                            auto validities = attribute->validityOrNull();
                            if (validities && validities->size() > 0) {
                                auto const count = validities->size();
                                for (uint32_t index = 0; index < count; ++index) {
                                    if (cancellation.periodic()) {
                                        break;
                                    }
                                    auto node = validities->at(index);
                                    evaluateAttributeCandidate(
                                        node ? attribute->model().resolve<Validity>(node) :
                                               model_ptr<Validity>{},
                                        true,
                                        index,
                                        count);
                                }
                            }
                            else {
                                evaluateAttributeCandidate({}, false, 0, 1);
                            }
                            return !channel.filterCompilationFailed_;
                        });
                });
        }
    }

    if (materializeOutput && !exactRoots.empty()) {
        for (auto const& root : exactRoots) {
            if (cancellation.periodic()) {
                break;
            }
            if (root.tileId_ != sourceLayer.tileId()) {
                continue;
            }
            auto feature = root.canonicalFeatureId_.empty() ?
                sourceLayer.find(root.typeId_, root.featureId_) :
                sourceLayer.find(root.canonicalFeatureId_);
            for (auto& channel : channels) {
                if (channel.definition_.scope_ != FeatureLayerFilterScope::Relation) {
                    continue;
                }
                if (!feature) {
                    issues.add(
                        channel.definition_.channelId_,
                        "<exact-root>",
                        Scope::Relation,
                        fmt::format(
                            "Exact relation root '{}' was not found in tile {}.",
                            root.canonicalFeatureId_.empty() ?
                                root.typeId_ :
                                root.canonicalFeatureId_,
                            sourceLayer.tileId().value()));
                    continue;
                }
                if (!featureTypeAllowed(feature, channel)) {
                    continue;
                }
                auto context = contextWithBindings(*feature, evaluatorContext->bindingFields_);
                auto rootMatch = evaluateFilter(
                    evaluator,
                    channel.definition_.featureFilter_,
                    *context,
                    static_cast<simfil::ModelNode const&>(*feature).schema(),
                    traces);
                if (!rootMatch) {
                    rejectFilterCandidate(
                        channel,
                        channel.definition_.featureFilter_,
                        Scope::Feature,
                        rootMatch.error(),
                        issues);
                    continue;
                }
                if (*rootMatch) {
                    channel.relationRoots_.push_back(RelationRoot{
                        feature,
                        root.requestOrdinal_,
                        true,
                    });
                }
            }
        }
    }

    std::vector<FeatureLayerPointGroupMember> pointGroupMembers;
    std::vector<FeatureLayerRelationDescriptor> relationDescriptors;
    for (size_t channelIndex = 0; channelIndex < channels.size(); ++channelIndex) {
        auto& channel = channels[channelIndex];
        if (materializeOutput && channel.definition_.scope_ == FeatureLayerFilterScope::Relation) {
            collectStoredRelationDescriptors(sourceLayer, channelIndex, channel, issues);
        }
        if (resultLayer) {
            materializeChannel(*resultLayer, channel, issues);
        }
        pointGroupMembers.insert(
            pointGroupMembers.end(),
            std::make_move_iterator(channel.pointGroupMembers_.begin()),
            std::make_move_iterator(channel.pointGroupMembers_.end()));
        relationDescriptors.insert(
            relationDescriptors.end(),
            std::make_move_iterator(channel.relationDescriptors_.begin()),
            std::make_move_iterator(channel.relationDescriptors_.end()));
    }
    return FeatureLayerFilterSourceResult{
        std::move(resultLayer),
        std::move(pointGroupMembers),
        std::move(relationDescriptors),
        issues.values(),
        std::move(traces),
        std::move(diagnostics),
        static_cast<uint32_t>(sourceLayer.size()),
    };
}

tl::expected<FeatureLayerPointGroupCompletion, simfil::Error>
FeatureLayerFilterRequest::completePointGroups(
    TileSubsetLayer& outputLayer,
    std::span<FeatureLayerPointGroupMember const> members,
    FeatureLayerFilterCancellationCheck const& cancellationCheck) const
{
    using GroupId = std::pair<size_t, FeatureLayerPointGroupKey>;
    std::map<GroupId, std::vector<FeatureLayerPointGroupMember>> groups;
    CancellationProbe cancellation(cancellationCheck);
    for (auto const& member : members) {
        if (cancellation.periodic()) {
            return FeatureLayerPointGroupCompletion{
                {},
                {},
                0,
            };
        }
        if (member.channelIndex_ >= channels_.size() || !channels_[member.channelIndex_].group_) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InternalError,
                "Point-group contribution references a non-group channel.",
            });
        }
        groups[{member.channelIndex_, member.key_}].push_back(member);
    }

    IssueAccumulator issues;
    std::map<std::string, simfil::Trace> traces;
    // All source tiles in one filter namespace use compatible StringIds and
    // one layer schema. Reuse the schema-installed evaluator across halo
    // tiles as well as across groups; keying by model address would still
    // rebuild it up to nine times for every output tile.
    std::map<std::string, EvaluatorContext> evaluatorsByStringPool;
    size_t entriesAdded = 0;
    auto stableFeatureIdentity = [](model_ptr<Feature> const& feature)
    {
        return fmt::format("{}:{}", feature->model().mapId(), feature->id()->toString());
    };

    for (auto& [groupId, groupMembers] : groups) {
        if (cancellation.boundary()) {
            return FeatureLayerPointGroupCompletion{
                issues.values(),
                std::move(traces),
                entriesAdded,
            };
        }
        auto const channelIndex = groupId.first;
        auto const& key = groupId.second;
        auto const& definition = channels_[channelIndex];
        if (groupMembers.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InvalidArguments,
                fmt::format(
                    "Point-grid channel '{}' produced too many members in one cell.",
                    definition.channelId_),
            });
        }

        std::ranges::sort(
            groupMembers,
            [&](auto const& left, auto const& right)
            {
                auto leftIdentity = stableFeatureIdentity(left.feature_);
                auto rightIdentity = stableFeatureIdentity(right.feature_);
                return std::tie(leftIdentity, left.geometryOrdinal_, left.pointOrdinal_) <
                    std::tie(rightIdentity, right.geometryOrdinal_, right.pointOrdinal_);
            });

        auto const& representative = groupMembers.front();
        auto const& representativeLayer = representative.feature_->model();
        auto const evaluatorKey = representativeLayer.stringPoolId();
        auto evaluatorContext = evaluatorsByStringPool.find(evaluatorKey);
        if (evaluatorContext == evaluatorsByStringPool.end()) {
            auto created = makeFilterEvaluator(representativeLayer, bindings_);
            if (!created) {
                return tl::unexpected(created.error());
            }
            evaluatorContext =
                evaluatorsByStringPool.emplace(evaluatorKey, std::move(*created)).first;
        }
        auto& evaluator = *evaluatorContext->second.evaluator_;

        std::vector<model_ptr<Feature>> features;
        features.reserve(groupMembers.size());
        for (auto const& member : groupMembers) {
            features.push_back(member.feature_);
        }
        auto featureArray = simfil::model_ptr<FeatureArrayView>::make(features);
        auto context = simfil::model_ptr<simfil::OverlayNode>::make(
            simfil::Value::field(*representative.feature_));
        addBindings(context, evaluatorContext->second.bindingFields_);
        context->set(StringPool::OverlayFeaturesStr, simfil::Value::field(featureArray));

        auto outputChannel = outputLayer.at(channelIndex);
        if (!outputChannel || outputChannel->scope() != Scope::Group ||
            outputChannel->channelId() != definition.channelId_)
        {
            return tl::unexpected(simfil::Error{
                simfil::Error::InternalError,
                "Point-group output channel does not match its request definition.",
            });
        }
        ChannelState channel;
        channel.definition_ = definition;
        channel.terminalScope_ = Scope::Group;
        channel.output_ = outputChannel;

        auto const representativeSchema =
            static_cast<simfil::ModelNode const&>(*representative.feature_).schema();
        auto values = evaluateFields(
            evaluator,
            channel,
            *context,
            representativeSchema,
            definition.entryFields_,
            Scope::Group,
            traces,
            issues);
        auto groupKey = outputLayer.newArray(3, true);
        groupKey->append(key.x_);
        groupKey->append(key.y_);
        groupKey->append(key.z_);

        auto geometry = outputLayer.newGeometryCollection(1, true);
        auto pointGeometry = outputLayer.newGeometry(GeomType::Points, 1, true);
        pointGeometry->append(representative.representativePoint_);
        pointGeometry->setName(
            representative.geometryName_ ?
                std::optional<std::string_view>{*representative.geometryName_} :
                std::nullopt);
        geometry->addGeometry(pointGeometry);

        auto representativeId = copyFeatureId(outputLayer, representative.feature_->id());
        std::vector<model_ptr<FeatureId>> memberIds;
        memberIds.reserve(groupMembers.size());
        for (size_t memberIndex = 0; memberIndex < groupMembers.size(); ++memberIndex) {
            // The representative is also the first group member. Reuse its
            // model node instead of serializing the same FeatureId twice for
            // every group (especially costly for singleton point groups).
            memberIds.push_back(
                memberIndex == 0 ?
                    representativeId :
                    copyFeatureId(outputLayer, groupMembers[memberIndex].feature_->id()));
        }
        outputChannel->newGroupEntry(
            groupKey,
            representativeId,
            geometry,
            materializeValues(outputLayer, values),
            memberIds);
        ++entriesAdded;
    }

    return FeatureLayerPointGroupCompletion{
        issues.values(),
        std::move(traces),
        entriesAdded,
    };
}

tl::expected<FeatureLayerRelationCompletion, simfil::Error>
FeatureLayerFilterRequest::completeRelations(
    TileSubsetLayer& outputLayer,
    std::span<FeatureLayerRelationDescriptor const> descriptors,
    std::span<MapTileKey const> requestedOutputKeys,
    std::span<FeatureLayerFilterRoot const> exactRoots,
    FeatureLayerFilterCancellationCheck const& cancellationCheck) const
{
    IssueAccumulator issues;
    std::map<std::string, simfil::Trace> traces;
    size_t entriesAdded = 0;
    size_t skippedOwnerOutsideCoverage = 0;
    CancellationProbe cancellation(cancellationCheck);

    for (size_t channelIndex = 0; channelIndex < channels_.size(); ++channelIndex) {
        if (cancellation.boundary()) {
            return FeatureLayerRelationCompletion{
                issues.values(),
                std::move(traces),
                entriesAdded,
                skippedOwnerOutsideCoverage,
            };
        }
        auto const& definition = channels_[channelIndex];
        if (definition.scope_ != FeatureLayerFilterScope::Relation) {
            continue;
        }
        if (!definition.relation_) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InternalError,
                "Relation completion received a channel without relation options.",
            });
        }
        auto outputChannel = outputLayer.at(channelIndex);
        if (!outputChannel || outputChannel->scope() != Scope::Relation ||
            outputChannel->channelId() != definition.channelId_)
        {
            return tl::unexpected(simfil::Error{
                simfil::Error::InternalError,
                "Relation output channel does not match its request definition.",
            });
        }

        std::optional<std::regex> namePattern;
        if (definition.relation_->relationNamePattern_) {
            try {
                namePattern
                    .emplace(*definition.relation_->relationNamePattern_, std::regex::ECMAScript);
            }
            catch (std::regex_error const& error) {
                issues.add(
                    definition.channelId_,
                    *definition.relation_->relationNamePattern_,
                    Scope::Relation,
                    fmt::format(
                        "Relation-name regular expression compilation failed: {}",
                        error.what()));
                continue;
            }
        }
        auto relationNameAccepted = [&](model_ptr<Relation> const& relation)
        {
            return relation &&
                (!namePattern ||
                 std::regex_match(relation->name().begin(), relation->name().end(), *namePattern));
        };

        std::vector<FeatureLayerRelationDescriptor const*> channelDescriptors;
        for (auto const& descriptor : descriptors) {
            if (cancellation.periodic()) {
                return FeatureLayerRelationCompletion{
                    issues.values(),
                    std::move(traces),
                    entriesAdded,
                    skippedOwnerOutsideCoverage,
                };
            }
            if (descriptor.channelIndex_ == channelIndex && descriptor.source_ &&
                descriptor.relation_ && descriptor.target_)
            {
                channelDescriptors.push_back(&descriptor);
            }
        }
        std::ranges::sort(
            channelDescriptors,
            [](auto const* left, auto const* right)
            { return directedRelationIdentity(*left) < directedRelationIdentity(*right); });

        struct Emission
        {
            FeatureLayerRelationDescriptor const* descriptor_ = nullptr;
            std::string relationId_;
            bool twoway_ = false;
        };
        std::vector<Emission> emissions;
        std::set<std::string> consumedRelations;

        for (auto const* descriptor : channelDescriptors) {
            if (cancellation.periodic()) {
                return FeatureLayerRelationCompletion{
                    issues.values(),
                    std::move(traces),
                    entriesAdded,
                    skippedOwnerOutsideCoverage,
                };
            }
            auto const directedId = directedRelationIdentity(*descriptor);
            if (consumedRelations.contains(directedId)) {
                continue;
            }
            consumedRelations.insert(directedId);

            bool twoway = false;
            std::string reverseId;
            if (definition.relation_->mergeTwoway_) {
                auto const sourceIdentity = stableFeatureIdentity(descriptor->source_);
                auto const targetIdentity = stableFeatureIdentity(descriptor->target_);

                for (auto const* candidate : channelDescriptors) {
                    if (cancellation.periodic()) {
                        return FeatureLayerRelationCompletion{
                            issues.values(),
                            std::move(traces),
                            entriesAdded,
                            skippedOwnerOutsideCoverage,
                        };
                    }
                    auto const candidateId = directedRelationIdentity(*candidate);
                    if (consumedRelations.contains(candidateId) ||
                        stableFeatureIdentity(candidate->source_) != targetIdentity ||
                        stableFeatureIdentity(candidate->target_) != sourceIdentity)
                    {
                        continue;
                    }
                    reverseId = candidateId;
                    consumedRelations.insert(candidateId);
                    twoway = true;
                    break;
                }

                if (!twoway) {
                    for (uint32_t ordinal = 0; ordinal < descriptor->target_->numRelations();
                         ++ordinal) {
                        auto reverse = descriptor->target_->getRelation(ordinal);
                        if (!relationNameAccepted(reverse)) {
                            continue;
                        }
                        auto reverseTarget = reverse->target();
                        if (!reverseTarget ||
                            reverseTarget->toString() != descriptor->source_->id()->toString()) {
                            continue;
                        }
                        auto candidateId = fmt::format("{}#{}", targetIdentity, ordinal);
                        if (!consumedRelations.insert(candidateId).second) {
                            continue;
                        }
                        reverseId = std::move(candidateId);
                        twoway = true;
                        break;
                    }
                }
            }

            auto relationId = directedId;
            if (twoway) {
                if (reverseId < relationId) {
                    relationId = fmt::format("{}<->{}", reverseId, relationId);
                }
                else {
                    relationId = fmt::format("{}<->{}", relationId, reverseId);
                }
            }

            if (twoway && exactRoots.empty()) {
                auto const sourceKey = MapTileKey(descriptor->source_->model());
                auto const targetKey =
                    descriptor->targetTileKey_.value_or(MapTileKey(descriptor->target_->model()));
                auto const sourceFeatureIdentity = descriptor->source_->id()->toString();
                auto const targetFeatureIdentity = descriptor->target_->id()->toString();
                auto const& owner =
                    southWestOwnerLess(
                        sourceKey,
                        sourceFeatureIdentity,
                        targetKey,
                        targetFeatureIdentity) ?
                    sourceKey :
                    targetKey;
                auto const ownerRequested = std::ranges::find(requestedOutputKeys, owner) !=
                    requestedOutputKeys.end();
                if (!ownerRequested) {
                    ++skippedOwnerOutsideCoverage;
                    continue;
                }
                if (owner != outputLayer.id()) {
                    continue;
                }
            }
            else if (twoway) {
                auto ownerRootOrdinal = descriptor->rootOrdinal_;
                if (auto sourceRoot =
                        exactRootOrdinal(descriptor->source_, outputLayer, exactRoots)) {
                    ownerRootOrdinal = std::min(ownerRootOrdinal, *sourceRoot);
                }
                if (auto targetRoot =
                        exactRootOrdinal(descriptor->target_, outputLayer, exactRoots)) {
                    ownerRootOrdinal = std::min(ownerRootOrdinal, *targetRoot);
                }
                if (descriptor->rootOrdinal_ != ownerRootOrdinal) {
                    continue;
                }
            }

            emissions.push_back(Emission{
                descriptor,
                std::move(relationId),
                twoway,
            });
        }

        std::ranges::sort(emissions, {}, &Emission::relationId_);
        ChannelState channel;
        channel.definition_ = definition;
        channel.terminalScope_ = Scope::Relation;
        channel.output_ = outputChannel;

        std::map<std::string, model_ptr<FeatureEntry>> endpointEntries;
        auto endpointEntry = [&](model_ptr<Feature> const& feature)
            -> tl::expected<model_ptr<FeatureEntry>, simfil::Error>
        {
            auto const identity = stableFeatureIdentity(feature);
            if (auto found = endpointEntries.find(identity); found != endpointEntries.end()) {
                return found->second;
            }

            auto evaluatorContext = makeFilterEvaluator(feature->model(), bindings_);
            if (!evaluatorContext) {
                return tl::unexpected(evaluatorContext.error());
            }
            auto context = contextWithBindings(*feature, evaluatorContext->bindingFields_);
            auto values = evaluateFields(
                *evaluatorContext->evaluator_,
                channel,
                *context,
                static_cast<simfil::ModelNode const&>(*feature).schema(),
                definition.featureFields_,
                Scope::Feature,
                traces,
                issues);
            bool downgradedGltfNodeIndex = false;
            auto entry = outputChannel->newFeatureEntry(
                copyFeatureId(outputLayer, feature->id()),
                copyGeometryCollection(
                    outputLayer,
                    feature->geomOrNull(),
                    definition.geometryTypes_,
                    definition.geometryName_,
                    MapTileKey(feature->model()) == outputLayer.id(),
                    &downgradedGltfNodeIndex),
                materializeValues(outputLayer, values));
            if (downgradedGltfNodeIndex) {
                issues.add(
                    definition.channelId_,
                    "<relation-geometry>",
                    Scope::Relation,
                    "Foreign GLTF node geometry was downgraded to its AABB because tile "
                    "attachments are tile-local.");
            }
            endpointEntries.emplace(identity, entry);
            return entry;
        };

        for (auto const& emission : emissions) {
            if (cancellation.periodic()) {
                return FeatureLayerRelationCompletion{
                    issues.values(),
                    std::move(traces),
                    entriesAdded,
                    skippedOwnerOutsideCoverage,
                };
            }
            auto const& descriptor = *emission.descriptor_;
            auto evaluatorContext = makeFilterEvaluator(descriptor.source_->model(), bindings_);
            if (!evaluatorContext) {
                return tl::unexpected(evaluatorContext.error());
            }
            auto context =
                makeRelationContext(descriptor, emission.twoway_, evaluatorContext->bindingFields_);
            auto const relationSchema =
                static_cast<simfil::ModelNode const&>(*descriptor.relation_).schema();
            auto entryMatch = evaluateFilter(
                *evaluatorContext->evaluator_,
                definition.entryFilter_,
                *context,
                relationSchema,
                traces);
            if (!entryMatch) {
                rejectFilterCandidate(
                    channel,
                    definition.entryFilter_,
                    Scope::Relation,
                    entryMatch.error(),
                    issues);
                continue;
            }
            if (!*entryMatch) {
                continue;
            }

            auto sourceEntry = endpointEntry(descriptor.source_);
            if (!sourceEntry) {
                return tl::unexpected(sourceEntry.error());
            }
            auto targetEntry = endpointEntry(descriptor.target_);
            if (!targetEntry) {
                return tl::unexpected(targetEntry.error());
            }
            auto relationValues = evaluateFields(
                *evaluatorContext->evaluator_,
                channel,
                *context,
                relationSchema,
                definition.entryFields_,
                Scope::Relation,
                traces,
                issues);
            auto sourceGeometry = copyRelationEffectiveGeometry(
                outputLayer,
                descriptor.source_,
                descriptor.relation_->sourceValidityOrNull(),
                channel,
                "source",
                issues,
                (*sourceEntry)->geometry());
            auto targetGeometry = copyRelationEffectiveGeometry(
                outputLayer,
                descriptor.target_,
                descriptor.relation_->targetValidityOrNull(),
                channel,
                "target",
                issues,
                (*targetEntry)->geometry());
            auto sourceData = descriptor.relation_->sourceDataReferences();
            outputChannel->newRelationEntry(
                emission.relationId_,
                descriptor.relation_->name(),
                sourceData ? sourceData->toJson().dump() : std::string{},
                RelationDirection::Forward,
                emission.twoway_,
                *sourceEntry,
                *targetEntry,
                sourceGeometry,
                targetGeometry,
                materializeValues(outputLayer, relationValues));
            ++entriesAdded;
        }
    }

    return FeatureLayerRelationCompletion{
        issues.values(),
        std::move(traces),
        entriesAdded,
        skippedOwnerOutsideCoverage,
    };
}

tl::expected<FeatureLayerFilterResult, simfil::Error>
FeatureLayerFilterRequest::filter(TileFeatureLayer const& sourceLayer) const
{
    if (std::ranges::any_of(
            channels_,
            [](auto const& channel) {
                return channel.group_.has_value() ||
                    channel.scope_ == FeatureLayerFilterScope::Relation;
            }))
    {
        return tl::unexpected(simfil::Error{
            simfil::Error::Unimplemented,
            "FeatureLayerFilterRequest::filter() is source-local; point groups and stored "
            "relations require the service executor.",
        });
    }

    auto sourceResult = filterSource(sourceLayer, true);
    if (!sourceResult) {
        return tl::unexpected(sourceResult.error());
    }
    for (auto const& issue : sourceResult->issues_) {
        sourceResult->layer_->addIssue(issue);
    }
    sourceResult->layer_->setTraces(std::move(sourceResult->traces_));
    sourceResult->layer_->setDiagnostics(sourceResult->diagnostics_);
    return FeatureLayerFilterResult{std::move(sourceResult->layer_)};
}

tl::expected<std::vector<model_ptr<Feature>>, simfil::Error>
TileFeatureLayer::find(FeatureLayerSelector const& selector) const
{
    if (selector.canonicalFeatureId_) {
        if (!selector.typeId_.empty() || selector.featureFilter_ || !selector.bindings_.empty()) {
            return tl::unexpected(simfil::Error{
                simfil::Error::InvalidArguments,
                "An exact feature selector cannot also contain a type, filter, or bindings.",
            });
        }
        auto feature = find(*selector.canonicalFeatureId_);
        if (!feature) {
            return std::vector<model_ptr<Feature>>{};
        }
        return std::vector<model_ptr<Feature>>{std::move(feature)};
    }

    if (selector.typeId_.empty() || !selector.featureFilter_) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InvalidArguments,
            "A filtered feature selector requires both typeId and featureFilter.",
        });
    }

    auto evaluatorContext = makeFilterEvaluator(*this, selector.bindings_);
    if (!evaluatorContext) {
        return tl::unexpected(evaluatorContext.error());
    }

    std::vector<model_ptr<Feature>> allFeatures;
    allFeatures.reserve(size());
    for (auto const& feature : *this) {
        allFeatures.push_back(feature);
    }
    auto featureArray = simfil::model_ptr<FeatureArrayView>::make(std::move(allFeatures));

    std::vector<model_ptr<Feature>> selected;
    std::map<std::string, simfil::Trace> ignoredTraces;
    for (auto const& feature : *this) {
        if (feature->typeId() != selector.typeId_) {
            continue;
        }
        auto context = simfil::model_ptr<simfil::OverlayNode>::make(simfil::Value::field(*feature));
        addBindings(context, evaluatorContext->bindingFields_);
        context->set(StringPool::OverlayFeaturesStr, simfil::Value::field(featureArray));
        auto matches = evaluateFilter(
            *evaluatorContext->evaluator_,
            selector.featureFilter_,
            *context,
            static_cast<simfil::ModelNode const&>(*feature).schema(),
            ignoredTraces);
        if (!matches) {
            return tl::unexpected(matches.error().error_);
        }
        if (*matches) {
            selected.push_back(feature);
        }
    }
    return selected;
}

}  // namespace mapget
