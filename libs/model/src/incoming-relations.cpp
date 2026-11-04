#include "mapget/model/incoming-relations.h"
#include "mapget/model/feature.h"
#include "mapget/model/featureid.h"

#include <set>
#include <stdexcept>

namespace mapget
{

nlohmann::json IncomingRelationCollection::toJson() const
{
    auto result = nlohmann::json{{"type", "mapget.incoming.relations"}, {"version", 1},
        {"target", target_.toString()}, {"featureId", featureId_}, {"complete", complete_},
        {"issues", issues_}, {"snapshots", nlohmann::json::array()},
        {"relations", nlohmann::json::array()}};
    for (auto const& snapshot : snapshots_) result["snapshots"].push_back(snapshot.toString());
    for (auto const& relation : relations_) {
        result["relations"].push_back({{"source", relation.source_.toString()},
            {"featureId", relation.featureId_}, {"relationIndex", relation.relationIndex_},
            {"name", relation.name_}, {"targetFeatureId", relation.targetFeatureId_}});
    }
    if (expiresAtMs_) result["expiresAtMs"] = *expiresAtMs_;
    return result;
}

IncomingRelationCollection IncomingRelationCollection::fromJson(nlohmann::json const& json)
{
    if (json.at("type") != "mapget.incoming.relations" || json.at("version") != 1)
        throw std::invalid_argument("Unsupported incoming relation manifest");
    IncomingRelationCollection result;
    result.target_ = MapPartitionKey(json.at("target").get<std::string>());
    result.featureId_ = json.at("featureId").get<std::string>();
    result.complete_ = json.at("complete").get<bool>();
    result.issues_ = json.at("issues").get<std::vector<std::string>>();
    if (json.contains("expiresAtMs")) result.expiresAtMs_ = json.at("expiresAtMs").get<int64_t>();
    if (!json.at("snapshots").is_array() || !json.at("relations").is_array() ||
        json.at("snapshots").size() > MaxSnapshots || json.at("relations").size() > MaxRelations)
        throw std::invalid_argument("Incoming relation manifest exceeds bounds");
    for (auto const& key : json.at("snapshots"))
        result.snapshots_.emplace_back(key.get<std::string>());
    for (auto const& entry : json.at("relations")) {
        auto ordinal = entry.at("relationIndex").get<int64_t>();
        if (ordinal < 0 || ordinal >= Relation::InvalidFeatureRelationIndex)
            throw std::invalid_argument("Invalid incoming relation ordinal");
        result.relations_.push_back({MapPartitionKey(entry.at("source").get<std::string>()),
            entry.at("featureId").get<std::string>(), static_cast<uint32_t>(ordinal),
            entry.at("name").get<std::string>(), entry.at("targetFeatureId").get<std::string>()});
    }
    return result;
}

bool relationTargetsFeature(Relation const& relation, Feature const& target)
{
    auto reference = relation.target();
    if (!reference || reference->mapId() != target.model().mapId() ||
        reference->typeId() != target.id()->typeId()) return false;
    auto composition = target.model().layerInfo()->matchingFeatureIdCompositionIndex(
        reference->typeId(), reference->keyValuePairs(), false);
    if (!composition || *composition != 0) return false;
    auto found = target.model().find(reference->typeId(), reference->keyValuePairs());
    return found && found->id()->toString() == target.id()->toString();
}

PartitionFeatureLayer::Ptr cloneInspectionFeatures(
    PartitionFeatureLayer::Ptr const& source, std::span<std::string const> featureIds,
    size_t byteBudget, std::function<bool()> const& cancelled)
{
    auto result = std::make_shared<PartitionFeatureLayer>(source->partitionId(),
        source->stringPoolId(), source->mapId(), source->layerInfo(), source->strings());
    result->setInfo(source->info());
    result->setTimestamp(source->timestamp());
    result->setTtl(source->ttl());
    result->setWarnings(source->warnings());
    result->setError(source->error());
    result->setErrorCode(source->errorCode());
    result->setGeometryAnchor(source->geometryAnchor());
    if (auto legalInfo = source->legalInfo()) result->setLegalInfo(*legalInfo);
    result->setGlbAttachmentName(source->glbAttachmentName());
    PartitionFeatureLayer::CloneCache cache;
    // One source model bounds one feature clone; guard each additional clone because
    // destination-relative geometry sharing may otherwise multiply the snapshot size.
    const auto featureUpperBound = source->memoryUsage().total().allocatedBytes;
    for (auto const& id : featureIds) {
        if (cancelled && cancelled()) throw std::runtime_error("Incoming inspection cancelled");
        auto currentBytes = result->memoryUsage().total().allocatedBytes;
        if (currentBytes > byteBudget || featureUpperBound > byteBudget - currentBytes) {
            result->addWarning("Incoming snapshot reached its memory limit.");
            break;
        }
        auto feature = source->find(id);
        if (feature) result->clone(cache, source, *feature, feature->id()->typeId(),
                                   feature->id()->keyValuePairs());
    }
    return result;
}

FeatureInspectionView::FeatureInspectionView(IncomingRelationCollection collection,
    std::vector<PartitionFeatureLayer::Ptr> snapshots)
    : collection_(std::move(collection)), snapshots_(std::move(snapshots))
{
    std::map<MapPartitionKey, PartitionFeatureLayer::Ptr> layers;
    if (collection_.snapshots_.size() > IncomingRelationCollection::MaxSnapshots ||
        collection_.relations_.size() > IncomingRelationCollection::MaxRelations ||
        snapshots_.size() > IncomingRelationCollection::MaxSnapshots)
        throw std::invalid_argument("Incoming inspection exceeds bounds");
    std::set<MapPartitionKey> declared(collection_.snapshots_.begin(), collection_.snapshots_.end());
    if (declared.size() != collection_.snapshots_.size() || !declared.contains(collection_.target_))
        throw std::invalid_argument("Invalid incoming snapshot manifest");
    for (auto const& layer : snapshots_) {
        if (!layer || !layers.emplace(layer->id(), layer).second)
            throw std::invalid_argument("Duplicate or missing incoming snapshot");
    }
    for (auto const& key : collection_.snapshots_) {
        if (key.mapId_ != collection_.target_.mapId_ ||
            key.partitionId_ != collection_.target_.partitionId_ || !layers.contains(key) ||
            key.layer_ != LayerType::Features)
            throw std::invalid_argument("Missing or out-of-partition incoming snapshot");
    }
    auto targetLayer = layers.find(collection_.target_);
    if (targetLayer == layers.end() ||
        !(target_ = targetLayer->second->find(collection_.featureId_)))
        throw std::invalid_argument("Incoming target snapshot is missing the selected feature");
    std::set<std::tuple<MapPartitionKey, std::string, uint32_t>> seen;
    for (auto const& ref : collection_.relations_) {
        if (ref.source_.mapId_ != collection_.target_.mapId_ ||
            ref.source_.partitionId_ != collection_.target_.partitionId_ ||
            !layers.contains(ref.source_) || !declared.contains(ref.source_) ||
            !seen.emplace(ref.source_, ref.featureId_, ref.relationIndex_).second)
            throw std::invalid_argument("Invalid incoming source association");
        auto source = layers.at(ref.source_)->find(ref.featureId_);
        if (!source || ref.relationIndex_ >= source->numRelations())
            throw std::invalid_argument("Incoming source relation is missing");
        auto relation = source->getRelation(ref.relationIndex_);
        auto target = relation->target();
        auto composition = target_->model().layerInfo()->matchingFeatureIdCompositionIndex(
            target->typeId(), target->keyValuePairs(), false);
        if (relation->name() != ref.name_ || ref.targetFeatureId_ != collection_.featureId_ ||
            target->mapId() != collection_.target_.mapId_ || target->typeId() != target_->typeId() ||
            !composition || (*composition == 0 && !relationTargetsFeature(*relation, *target_)))
            throw std::invalid_argument("Incoming relation does not point to the selected feature");
        incoming_.push_back({source, relation, ref.source_, ref.relationIndex_});
    }
}

} // namespace mapget
