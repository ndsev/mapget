#include "featurelayer-search.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <tuple>

#include "fmt/format.h"
#include "mapget/log.h"
#include "mapget/model/simfilutil.h"
#include "simfil/overlay.h"
#include "simfil/simfil.h"

namespace mapget
{
namespace
{
/** Clone a mapget string pool so search can evaluate without mutating datasource-owned state. */
tl::expected<std::shared_ptr<StringPool>, simfil::Error> copyStringPool(TileFeatureLayer const& sourceLayer)
{
    auto sourceStrings = std::dynamic_pointer_cast<StringPool>(sourceLayer.strings());
    if (!sourceStrings) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InternalError,
            fmt::format("Feature layer '{}' does not use a mapget StringPool.", sourceLayer.nodeId())});
    }
    return std::make_shared<StringPool>(*sourceStrings);
}

/** Cache and evaluate SIMFIL expressions without touching the source layer's expression cache. */
class SearchEvaluator
{
public:
    explicit SearchEvaluator(std::unique_ptr<simfil::Environment> env)
        : env_(std::move(env))
    {}

    /** Evaluate one expression against a source-layer node using search-local mutable state. */
    tl::expected<TileFeatureLayer::QueryResult, simfil::Error> evaluate(
        std::string_view query,
        simfil::ModelNode const& node,
        bool anyMode,
        bool autoWildcard)
    {
        auto key = std::make_tuple(std::string(query), anyMode, autoWildcard);
        auto astIt = cache_.find(key);
        if (astIt == cache_.end()) {
            auto ast = simfil::compile(*env_, query, anyMode, autoWildcard);
            if (!ast) {
                return tl::unexpected<simfil::Error>(std::move(ast.error()));
            }
            astIt = cache_.emplace(std::move(key), std::move(*ast)).first;
        }

        env_->warnings.clear();
        env_->traces.clear();

        TileFeatureLayer::QueryResult result;
        auto values = simfil::eval(*env_, *astIt->second, node, &result.diagnostics);
        if (!values) {
            env_->traces.clear();
            return tl::unexpected<simfil::Error>(std::move(values.error()));
        }

        result.values = std::move(*values);
        result.traces = std::move(env_->traces);
        env_->traces.clear();
        return result;
    }

private:
    std::unique_ptr<simfil::Environment> env_;
    std::map<std::tuple<std::string, bool, bool>, simfil::ASTPtr> cache_;
};

/** Build a search-local evaluator whose string IDs match the source layer. */
tl::expected<std::unique_ptr<SearchEvaluator>, simfil::Error> makeSearchEvaluator(TileFeatureLayer const& sourceLayer)
{
    auto copiedStrings = copyStringPool(sourceLayer);
    if (!copiedStrings) {
        return tl::unexpected(copiedStrings.error());
    }
    auto env = makeEnvironment(*copiedStrings);
    installSchemaRegistry(*env, sourceLayer.schemaRegistry(), *copiedStrings);
    return std::make_unique<SearchEvaluator>(std::move(env));
}

/** Convert a SIMFIL value into a node owned by the search-result layer. */
simfil::ModelNode::Ptr materializeResultValue(TileSearchResultLayer& layer, simfil::Value const& value)
{
    switch (value.type) {
    case simfil::ValueType::Undef:
    case simfil::ValueType::Null:
        return layer.resolve<simfil::ModelNode>(
            simfil::ModelNodeAddress{simfil::Model::Null, 1},
            simfil::ScalarValueType{});
    case simfil::ValueType::Bool:
        return layer.newSmallValue(value.as<simfil::ValueType::Bool>());
    case simfil::ValueType::Int:
        return layer.newValue(value.as<simfil::ValueType::Int>());
    case simfil::ValueType::Float:
        return layer.newValue(value.as<simfil::ValueType::Float>());
    case simfil::ValueType::String:
        return layer.newValue(value.as<simfil::ValueType::String>());
    case simfil::ValueType::Bytes:
        return layer.newValue(value.as<simfil::ValueType::Bytes>());
    case simfil::ValueType::TransientObject:
    case simfil::ValueType::Object:
    case simfil::ValueType::Array:
        // Cross-model object cloning is intentionally conservative here. Most
        // withFields expressions are scalar labels or style keys; structured
        // expression results remain debuggable through their string rendering.
        return layer.newValue(value.toString());
    }
    return layer.newValue(value.toString());
}

/** Copy one geometry node into the result layer's geometry storage. */
model_ptr<Geometry> copyGeometry(TileSearchResultLayer& target, model_ptr<Geometry> const& source)
{
    auto copied = target.newGeometry(source->geomType(), std::max<size_t>(1, source->numPoints()), false);
    switch (source->geomType()) {
    case GeomType::AABB:
        copied->setAabb(source->aabbOrigin(), source->aabbSize());
        break;
    case GeomType::GltfNodeIndex:
        copied->setGltfNodeIndex(source->gltfNodeIndex());
        copied->setGltfNodeBounds(source->gltfNodeAabbOrigin(), source->gltfNodeAabbSize());
        break;
    default:
        source->forEachPoint([&](Point const& point) {
            copied->append(point);
            return true;
        });
        break;
    }
    copied->setStage(source->stage());
    return copied;
}

/** Copy a feature's primary high-fidelity geometry collection into a result layer. */
model_ptr<GeometryCollection> copyGeometryCollection(
    TileSearchResultLayer& target,
    model_ptr<GeometryCollection> const& source)
{
    auto copied = target.newGeometryCollection(source ? source->numGeometries() : 0, false);
    if (!source) {
        return copied;
    }
    auto const highFidelityStage = target.layerInfo()
        ? std::optional<uint32_t>(target.layerInfo()->highFidelityStage_)
        : std::nullopt;
    source->forEachGeometry([&](model_ptr<Geometry> const& geometry) {
        if (highFidelityStage) {
            auto geometryStage = geometry->stage();
            if (geometryStage && *geometryStage != *highFidelityStage) {
                return true;
            }
        }
        copied->addGeometry(copyGeometry(target, geometry));
        return true;
    });
    if (copied->numGeometries() == 0) {
        source->forEachGeometry([&](model_ptr<Geometry> const& geometry) {
            copied->addGeometry(copyGeometry(target, geometry));
            return false;
        });
    }
    return copied;
}

/** Materialize a computed validity geometry as a single-geometry collection. */
model_ptr<GeometryCollection> copySelfContainedGeometryCollection(
    TileSearchResultLayer& target,
    SelfContainedGeometry const& source,
    std::optional<uint32_t> stage)
{
    auto copied = target.newGeometryCollection(source.points_.empty() ? 0 : 1, false);
    if (source.points_.empty()) {
        return copied;
    }

    auto geometry = target.newGeometry(source.geomType_, std::max<size_t>(1, source.points_.size()), false);
    switch (source.geomType_) {
    case GeomType::AABB:
        if (source.points_.size() >= 2) {
            geometry->setAabb(source.points_[0], source.points_[1]);
        }
        break;
    case GeomType::GltfNodeIndex:
        // Computed validity geometries are spatial subsets, not GLTF node references.
        return copied;
    default:
        for (auto const& point : source.points_) {
            geometry->append(point);
        }
        break;
    }
    geometry->setStage(stage);
    copied->addGeometry(geometry);
    return copied;
}

/** Resolve and copy the geometry that should visually represent one attribute validity match. */
model_ptr<GeometryCollection> copyValidityGeometryCollection(
    TileSearchResultLayer& target,
    model_ptr<Feature> const& feature,
    model_ptr<Validity> const& validity)
{
    if (!feature || !validity) {
        return {};
    }
    auto featureGeometry = feature->geomOrNull();
    if (!featureGeometry) {
        return {};
    }

    auto const fallbackStage = target.layerInfo()
        ? std::optional<uint32_t>(target.layerInfo()->highFidelityStage_)
        : std::nullopt;
    std::string error;
    auto computed = validity->computeGeometry(featureGeometry, &error, fallbackStage);
    if (computed.points_.empty()) {
        if (!error.empty()) {
            log().warn(
                "Search result validity geometry failed for {}: {}",
                feature->id() ? feature->id()->toString() : std::string{"<unknown>"},
                error);
        }
        return {};
    }
    auto resultStage = validity->geometryStage();
    if (!resultStage) {
        resultStage = fallbackStage;
    }
    return copySelfContainedGeometryCollection(target, computed, resultStage);
}

/** Copy a feature id into result-layer storage without retaining source-layer addresses. */
model_ptr<FeatureId> copyFeatureId(TileSearchResultLayer& target, model_ptr<FeatureId> const& source)
{
    return target.newFeatureId(source->typeId(), source->keyValuePairs(), source->externalMapId());
}

/** Return true when an eval result has a first truthy boolean item. */
bool firstResultMatches(TileFeatureLayer::QueryResult const& result)
{
    if (result.values.empty()) {
        return false;
    }
    auto const& first = result.values.front();
    return first.isa(simfil::ValueType::Bool) && first.as<simfil::ValueType::Bool>();
}

/** Merge query traces into the per-tile aggregate. */
void mergeTraces(std::map<std::string, simfil::Trace>& merged, std::map<std::string, simfil::Trace> traces)
{
    for (auto&& [key, trace] : traces) {
        merged[key].append(std::move(trace));
    }
}

/** Export trace aggregates as compact JSON metadata on the result layer. */
nlohmann::json tracesToJson(std::map<std::string, simfil::Trace> const& traces)
{
    auto result = nlohmann::json::object();
    for (auto const& [key, trace] : traces) {
        auto values = nlohmann::json::array();
        for (auto const& value : trace.values) {
            values.push_back(value.toString());
        }
        result[key] = nlohmann::json::object({
            {"calls", trace.calls},
            {"totalus", trace.totalus.count()},
            {"values", std::move(values)},
        });
    }
    return result;
}

/** Evaluate withFields expressions in the same context as the match expression. */
std::vector<simfil::ModelNode::Ptr> evaluateWithFields(
    TileFeatureLayer& sourceLayer,
    SearchEvaluator& evaluator,
    TileSearchResultLayer& resultLayer,
    simfil::ModelNode const& context,
    std::vector<std::string> const& expressions,
    std::map<std::string, simfil::Trace>& traces,
    std::set<std::string>& reportedFieldFailures)
{
    std::vector<simfil::ModelNode::Ptr> values;
    values.reserve(expressions.size());
    for (auto const& expression : expressions) {
        auto evalResult = evaluator.evaluate(expression, context, false, false);
        if (!evalResult) {
            if (reportedFieldFailures.insert(expression).second) {
                log().warn(
                    "Search result field expression '{}' failed for {}: {}",
                    expression,
                    MapTileKey(sourceLayer).toString(),
                    evalResult.error().message);
            }
            values.push_back(materializeResultValue(resultLayer, simfil::Value::null()));
            continue;
        }
        mergeTraces(traces, std::move(evalResult->traces));
        // Diagnostics are kept for the main search expression only:
        // simfil::Diagnostics::append requires one AST/index layout.
        if (evalResult->values.empty()) {
            values.push_back(materializeResultValue(resultLayer, simfil::Value::null()));
        } else {
            values.push_back(materializeResultValue(resultLayer, evalResult->values.front()));
        }
    }
    return values;
}

struct AttributeMatchInfo
{
    uint32_t attributeIndex_ = SearchResult::InvalidAttributeIndex;
    uint32_t validityIndex_ = SearchResult::InvalidAttributeIndex;
    uint32_t validityCount_ = 0;
    model_ptr<Validity> validity_;
};

/** Add a result for one matched feature/context pair. */
tl::expected<void, simfil::Error> addSearchResult(
    TileFeatureLayer& sourceLayer,
    SearchEvaluator& evaluator,
    TileSearchResultLayer& resultLayer,
    FeatureLayerSearchRequest const& request,
    model_ptr<Feature> const& feature,
    simfil::ModelNode const& context,
    std::optional<AttributeMatchInfo> const& attributeMatch,
    std::map<std::string, simfil::Trace>& traces,
    std::set<std::string>& reportedFieldFailures)
{
    auto values = evaluateWithFields(
        sourceLayer,
        evaluator,
        resultLayer,
        context,
        request.withFields_,
        traces,
        reportedFieldFailures);

    model_ptr<GeometryCollection> resultGeometry;
    if (attributeMatch && attributeMatch->validity_) {
        resultGeometry = copyValidityGeometryCollection(resultLayer, feature, attributeMatch->validity_);
    }
    if (!resultGeometry || resultGeometry->numGeometries() == 0) {
        auto sourceGeometry = feature->geomOrNull();
        if (!sourceGeometry) {
            return {};
        }
        resultGeometry = copyGeometryCollection(resultLayer, sourceGeometry);
    }
    resultLayer.newSearchResult(
        copyFeatureId(resultLayer, feature->id()),
        resultGeometry,
        values,
        attributeMatch ? std::optional<uint32_t>(attributeMatch->attributeIndex_) : std::nullopt,
        attributeMatch ? std::optional<uint32_t>(attributeMatch->validityIndex_) : std::nullopt,
        attributeMatch ? std::optional<uint32_t>(attributeMatch->validityCount_) : std::nullopt);
    return {};
}

} // namespace

tl::expected<TileFeatureLayer::Ptr, simfil::Error> assembleFeatureLayerStages(
    std::span<TileFeatureLayer::Ptr const> stages)
{
    std::vector<TileFeatureLayer::Ptr> orderedStages;
    orderedStages.reserve(stages.size());
    for (auto const& stage : stages) {
        if (stage) {
            orderedStages.push_back(stage);
        }
    }
    if (orderedStages.empty()) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InternalError,
            "Cannot assemble a feature layer without stage payloads."});
    }

    std::sort(orderedStages.begin(), orderedStages.end(), [](auto const& lhs, auto const& rhs) {
        return lhs->stage().value_or(0U) < rhs->stage().value_or(0U);
    });

    auto assembled = orderedStages.front();

    for (auto const& stageLayer : std::span(orderedStages).subspan(1)) {
        assembled->attachOverlay(stageLayer);
    }

    return assembled;
}

tl::expected<FeatureLayerSearchResult, simfil::Error> searchFeatureLayerAsResultLayer(
    TileFeatureLayer& sourceLayer,
    FeatureLayerSearchRequest const& request)
{
    auto evaluator = makeSearchEvaluator(sourceLayer);
    if (!evaluator) {
        return tl::unexpected(evaluator.error());
    }
    auto& searchLayer = sourceLayer;

    auto resultLayer = std::make_shared<TileSearchResultLayer>(
        searchLayer.tileId(),
        searchLayer.nodeId(),
        searchLayer.mapId(),
        searchLayer.layerInfo(),
        sourceLayer.strings());
    resultLayer->setGeometryAnchor(searchLayer.geometryAnchor());
    resultLayer->setTimestamp(searchLayer.timestamp());
    resultLayer->setStage(request.sourceStageMask_.size() > 1 ? std::nullopt : searchLayer.stage());
    resultLayer->setResultFields(request.withFields_);
    resultLayer->setInfo("searchId", request.searchId_);
    resultLayer->setInfo("searchScope", request.scope_ == FeatureLayerSearchScope::Attribute ? "attribute" : "feature");
    resultLayer->setInfo("sourceNodeId", sourceLayer.nodeId());
    resultLayer->setInfo("sourceMapId", sourceLayer.mapId());
    resultLayer->setInfo("sourceLayerId", sourceLayer.layerInfo() ? sourceLayer.layerInfo()->layerId_ : std::string{});
    resultLayer->setInfo("sourceTileId", sourceLayer.tileId().value_);
    resultLayer->setInfo("chunkIndex", request.chunkIndex_);
    resultLayer->setInfo("resultCount", 0);
    resultLayer->setInfo("resultFields", request.withFields_);
    if (!request.sourceStageMask_.empty()) {
        resultLayer->setInfo("sourceStageMask", request.sourceStageMask_);
    }
    if (!request.requestKey_.empty()) {
        resultLayer->setInfo("searchRequestKey", request.requestKey_);
    }
    if (request.refresh_) {
        resultLayer->setInfo("refresh", *request.refresh_);
    }

    std::map<std::string, simfil::Trace> mergedTraces;
    simfil::Diagnostics mergedDiagnostics;
    std::set<std::string> reportedFieldFailures;

    auto evaluateCandidate = [&](model_ptr<Feature> const& feature,
                                 simfil::ModelNode const& context,
                                 std::optional<AttributeMatchInfo> const& attributeMatch)
        -> tl::expected<void, simfil::Error>
    {
        auto evalResult = (*evaluator)->evaluate(request.query_, context, true, true);
        if (!evalResult) {
            return tl::unexpected(evalResult.error());
        }
        mergeTraces(mergedTraces, std::move(evalResult->traces));
        mergedDiagnostics.append(evalResult->diagnostics);
        if (!firstResultMatches(*evalResult)) {
            return {};
        }
        return addSearchResult(
            searchLayer,
            **evaluator,
            *resultLayer,
            request,
            feature,
            context,
            attributeMatch,
            mergedTraces,
            reportedFieldFailures);
    };

    if (request.scope_ == FeatureLayerSearchScope::Feature) {
        for (auto const& feature : searchLayer) {
            if (auto result = evaluateCandidate(feature, *feature, std::nullopt); !result) {
                return tl::unexpected(result.error());
            }
        }
    } else {
        for (auto const& feature : searchLayer) {
            auto layers = feature->attributeLayersOrNull();
            if (!layers) {
                continue;
            }
            bool aborted = false;
            std::optional<simfil::Error> error;
            uint32_t attributeIndex = 0;
            layers->forEachLayer([&](std::string_view layerName, model_ptr<AttributeLayer> const& attrLayer) {
                return attrLayer->forEachAttribute([&](model_ptr<Attribute> const& attr) {
                    auto const thisAttributeIndex = attributeIndex++;
                    auto makeContext = [&](uint32_t validityIndex, uint32_t validityCount) {
                        auto context = simfil::model_ptr<simfil::OverlayNode>::make(simfil::Value::field(*attr));
                        context->set(StringPool::OverlayNameStr, simfil::Value(attr->name()));
                        context->set(StringPool::OverlayFeatureStr, simfil::Value::field(*feature));
                        context->set(StringPool::OverlayLayerStr, simfil::Value(layerName));
                        context->set(StringPool::OverlayValidityIndexStr, simfil::Value(static_cast<int64_t>(validityIndex)));
                        context->set(StringPool::OverlayValidityCountStr, simfil::Value(static_cast<int64_t>(validityCount)));
                        return context;
                    };

                    if (auto validities = attr->validityOrNull(); validities && validities->size() > 0) {
                        auto const count = validities->size();
                        for (uint32_t validityIndex = 0; validityIndex < count; ++validityIndex) {
                            auto context = makeContext(validityIndex, count);
                            auto match = AttributeMatchInfo{
                                thisAttributeIndex,
                                validityIndex,
                                count,
                                searchLayer.resolve<Validity>(validities->at(validityIndex))};
                            if (auto result = evaluateCandidate(feature, *context, match); !result) {
                                error = result.error();
                                aborted = true;
                                return false;
                            }
                        }
                    } else {
                        auto context = makeContext(0, 1);
                        auto match = AttributeMatchInfo{
                            thisAttributeIndex,
                            0,
                            1,
                            {}};
                        if (auto result = evaluateCandidate(feature, *context, match); !result) {
                            error = result.error();
                            aborted = true;
                            return false;
                        }
                    }
                    return true;
                });
            });
            if (aborted) {
                return tl::unexpected(*error);
            }
        }
    }

    resultLayer->setInfo("traces", tracesToJson(mergedTraces));
    resultLayer->setInfo("resultCount", resultLayer->size());
    resultLayer->setDiagnostics(mergedDiagnostics);
    return FeatureLayerSearchResult{std::move(resultLayer)};
}

} // namespace mapget
