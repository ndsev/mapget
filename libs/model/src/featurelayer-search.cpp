#include "featurelayer-search.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "fmt/format.h"
#include "mapget/log.h"
#include "simfil/overlay.h"

namespace mapget
{
namespace
{
/** Produce a stable result string-pool id for one logical search refresh. */
std::string makeDefaultResultNodeId(TileFeatureLayer const& sourceLayer, FeatureLayerSearchRequest const& request)
{
    auto const refreshKey = request.refresh_
        ? std::to_string(*request.refresh_)
        : std::to_string(std::hash<std::string>{}(request.requestKey_));
    return fmt::format(
        "__mapget_search__:{}:{}:{}:{}",
        sourceLayer.nodeId(),
        request.searchId_,
        sourceLayer.layerInfo() ? sourceLayer.layerInfo()->layerId_ : std::string{},
        refreshKey);
}

/** Serialize/parse a feature layer with a copied string pool for side-effect-free SIMFIL evaluation. */
tl::expected<TileFeatureLayer::Ptr, simfil::Error> makeEvaluationLayerCopy(TileFeatureLayer& sourceLayer)
{
    std::ostringstream serialized;
    if (auto writeResult = sourceLayer.write(serialized); !writeResult) {
        return tl::unexpected(writeResult.error());
    }

    auto bytesString = serialized.str();
    std::vector<uint8_t> bytes(bytesString.begin(), bytesString.end());
    auto copiedStrings = std::make_shared<simfil::StringPool>(*sourceLayer.strings());
    auto sourceInfo = sourceLayer.layerInfo();
    auto sourceMapId = sourceLayer.mapId();
    auto sourceLayerId = sourceInfo ? sourceInfo->layerId_ : std::string{};

    try {
        return std::make_shared<TileFeatureLayer>(
            bytes,
            [sourceInfo = std::move(sourceInfo),
             sourceMapId = std::move(sourceMapId),
             sourceLayerId = std::move(sourceLayerId)](
                std::string_view mapId,
                std::string_view layerId) -> std::shared_ptr<LayerInfo> {
                if (mapId == sourceMapId && layerId == sourceLayerId) {
                    return sourceInfo;
                }
                return {};
            },
            [copiedStrings = std::move(copiedStrings)](std::string_view) {
                return copiedStrings;
            });
    } catch (std::exception const& e) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InternalError,
            fmt::format("Failed to create side-effect-free search evaluation layer: {}", e.what())});
    }
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
    TileSearchResultLayer& resultLayer,
    simfil::ModelNode const& context,
    std::vector<std::string> const& expressions,
    std::map<std::string, simfil::Trace>& traces,
    std::set<std::string>& reportedFieldFailures)
{
    std::vector<simfil::ModelNode::Ptr> values;
    values.reserve(expressions.size());
    for (auto const& expression : expressions) {
        auto evalResult = sourceLayer.evaluate(expression, context, false, false);
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
    std::string attributePath_;
    uint32_t validityIndex_ = SearchResult::InvalidAttributeIndex;
    uint32_t validityCount_ = 0;
};

/** Add a result for one matched feature/context pair. */
tl::expected<void, simfil::Error> addSearchResult(
    TileFeatureLayer& sourceLayer,
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
        resultLayer,
        context,
        request.withFields_,
        traces,
        reportedFieldFailures);

    auto sourceGeometry = feature->geomOrNull();
    if (!sourceGeometry) {
        return {};
    }
    resultLayer.newSearchResult(
        copyFeatureId(resultLayer, feature->id()),
        copyGeometryCollection(resultLayer, sourceGeometry),
        values,
        attributeMatch ? std::optional<uint32_t>(attributeMatch->attributeIndex_) : std::nullopt,
        attributeMatch && !attributeMatch->attributePath_.empty()
            ? std::optional<std::string_view>(attributeMatch->attributePath_)
            : std::nullopt,
        attributeMatch ? std::optional<uint32_t>(attributeMatch->validityIndex_) : std::nullopt,
        attributeMatch ? std::optional<uint32_t>(attributeMatch->validityCount_) : std::nullopt);
    return {};
}

} // namespace

tl::expected<TileFeatureLayer::Ptr, simfil::Error> assembleFeatureLayerStages(
    std::span<TileFeatureLayer::Ptr const> stages,
    std::string_view evaluationNodeId)
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

    auto const& base = orderedStages.front();
    auto strings = std::make_shared<StringPool>(evaluationNodeId);
    auto assembled = std::make_shared<TileFeatureLayer>(
        base->tileId(),
        std::string(evaluationNodeId),
        base->mapId(),
        base->layerInfo(),
        strings);
    assembled->setGeometryAnchor(base->geometryAnchor());
    assembled->setTimestamp(base->timestamp());
    assembled->setStage(std::nullopt);

    TileFeatureLayer::CloneCache clonedModelNodes;
    for (auto const& stageLayer : orderedStages) {
        for (auto const& feature : *stageLayer) {
            if (!feature || !feature->id()) {
                continue;
            }
            assembled->clone(
                clonedModelNodes,
                stageLayer,
                *feature,
                feature->id()->typeId(),
                feature->id()->keyValuePairs());
        }
    }

    return assembled;
}

tl::expected<FeatureLayerSearchResult, simfil::Error> searchFeatureLayerAsResultLayer(
    TileFeatureLayer& sourceLayer,
    FeatureLayerSearchRequest const& request)
{
    auto evaluationLayer = makeEvaluationLayerCopy(sourceLayer);
    if (!evaluationLayer) {
        return tl::unexpected(evaluationLayer.error());
    }
    auto& searchLayer = **evaluationLayer;

    auto resultNodeId = request.resultNodeId_.value_or(makeDefaultResultNodeId(searchLayer, request));
    auto resultStrings = std::make_shared<StringPool>(resultNodeId);
    auto resultLayer = std::make_shared<TileSearchResultLayer>(
        searchLayer.tileId(),
        resultNodeId,
        searchLayer.mapId(),
        searchLayer.layerInfo(),
        resultStrings);
    resultLayer->setGeometryAnchor(searchLayer.geometryAnchor());
    resultLayer->setTimestamp(searchLayer.timestamp());
    resultLayer->setStage(searchLayer.stage());
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
        auto evalResult = searchLayer.evaluate(request.query_, context, true);
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
        auto nameId = searchLayer.strings()->emplace("$name");
        auto featureId = searchLayer.strings()->emplace("$feature");
        auto layerId = searchLayer.strings()->emplace("$layer");
        auto validityIndexId = searchLayer.strings()->emplace("$validityIndex");
        auto validityCountId = searchLayer.strings()->emplace("$validityCount");
        if (!nameId || !featureId || !layerId || !validityIndexId || !validityCountId) {
            return tl::unexpected(simfil::Error{simfil::Error::InternalError, "Failed to allocate attribute search context keys."});
        }

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
                    auto const attributePath = fmt::format("{}.{}", layerName, attr->name());
                    auto makeContext = [&](uint32_t validityIndex, uint32_t validityCount) {
                        auto context = simfil::model_ptr<simfil::OverlayNode>::make(simfil::Value::field(*attr));
                        context->set(*nameId, simfil::Value(attr->name()));
                        context->set(*featureId, simfil::Value::field(*feature));
                        context->set(*layerId, simfil::Value(layerName));
                        context->set(*validityIndexId, simfil::Value(static_cast<int64_t>(validityIndex)));
                        context->set(*validityCountId, simfil::Value(static_cast<int64_t>(validityCount)));
                        return context;
                    };

                    if (auto validities = attr->validityOrNull(); validities && validities->size() > 0) {
                        auto const count = validities->size();
                        for (uint32_t validityIndex = 0; validityIndex < count; ++validityIndex) {
                            auto context = makeContext(validityIndex, count);
                            auto match = AttributeMatchInfo{
                                thisAttributeIndex,
                                attributePath,
                                validityIndex,
                                count};
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
                            attributePath,
                            0,
                            1};
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
    return FeatureLayerSearchResult{std::move(resultLayer), std::move(mergedDiagnostics)};
}

} // namespace mapget
