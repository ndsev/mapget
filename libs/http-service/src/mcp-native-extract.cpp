#include "mcp-native-call.h"

#include "mapget/model/attrlayer.h"
#include "mapget/model/featureid.h"
#include "mapget/model/layerschema.h"
#include "mapget/model/sourcedata.h"
#include "mapget/service/locate.h"

#include <charconv>
#include <set>

namespace mapget::detail
{
using Json = nlohmann::json;

void McpNativeTools::Call::prepareExtraction()
{
    if (arguments.contains("query") && arguments.contains("expressions"))
        throw std::invalid_argument("Choose query or expressions");
    if (arguments.contains("reference")) {
        if (arguments.contains("partitions") || !arguments.contains("partition"))
            throw std::invalid_argument("A reference needs its owning partition");
        auto const& reference = arguments.at("reference");
        if (arguments.contains("layerId") && arguments["layerId"] != reference["layerId"])
            throw std::invalid_argument("Reference layer differs from target layer");
        arguments["layerId"] = reference.at("layerId");
        arguments["partitions"] = Json::array({arguments.at("partition")});
    }
    else if (arguments.contains("partition"))
        throw std::invalid_argument("partition needs reference");
    selectLayer();
    auto feature = name == "mapget_extract_features";
    if (layer_->type_ != (feature ? LayerType::Features : LayerType::SourceData))
        throw std::invalid_argument("Layer payload type mismatch");
    if (arguments.contains("featureIds"))
        featureIds_ = arguments["featureIds"].get<std::vector<std::string>>();
    std::set<PartitionId> seen;
    if (arguments.contains("partitions")) {
        for (auto const& raw : arguments.at("partitions")) {
            auto id = PartitionId::fromJson(raw);
            if (id.kind() != layer_->partitionKind_)
                throw std::invalid_argument("Partition kind mismatch");
            if (seen.insert(id).second)
                partitions_.push_back(id);
        }
        if (partitions_.empty())
            throw std::invalid_argument("Empty partitions");
    }
    else {
        if (!feature || featureIds_.empty())
            throw std::invalid_argument("Partitions or feature IDs required");
        for (auto const& id : featureIds_) {
            if (!step())
                break;
            ParsedFeatureId parsed;
            if (!parseFeatureIdString(id, *layer_, parsed))
                throw std::invalid_argument("Feature ID");
            LocateRequest request(arguments.at("mapId"), parsed.typeId_, parsed.keyValuePairs_);
            request.layerId_ = layer_->layerId_;
            // Datasource locate produces cheap routing candidates. Never use Service::locate(),
            // whose synchronous wait would deadlock a single-worker service.
            for (auto const& candidate : source_.dataSource->locate(request)) {
                if (!step())
                    break;
                auto const& key = candidate.tileKey_;
                if (key.mapId_ != source_.info->mapId_ || key.layerId_ != layer_->layerId_ ||
                    key.layer_ != LayerType::Features ||
                    key.partitionId_.kind() != layer_->partitionKind_)
                    continue;
                if (seen.insert(key.partitionId_).second)
                    partitions_.push_back(key.partitionId_);
                if (partitions_.size() > 32)
                    throw std::invalid_argument(
                        "Locate exceeds partition limit; supply explicit partitions");
            }
        }
    }
    if (arguments.value("rewrite", false) && arguments.contains("predicate") &&
        layer_->layerSchema()) {
        auto scope = arguments.value("scope", "feature") == "attribute" ?
            LayerSchema::SearchQueryRequestedScope::Attribute :
            LayerSchema::SearchQueryRequestedScope::Feature;
        auto normal = layer_->layerSchema()
                          ->normalizeSearchQuery(arguments["predicate"].get<std::string>(), scope);
        if (!normal)
            throw std::invalid_argument("Invalid predicate normalization");
        arguments["predicate"] = normal->normalizedQuery_;
    }
}

void McpNativeTools::Call::loadNext()
{
    if (!step() || nextPartition_ >= partitions_.size()) {
        finishItems();
        return;
    }
    if (items_.size() >= limit_) {
        truncate("item_limit");
        finishItems();
        return;
    }
    auto child = std::make_shared<LayerTilesRequest>(
        source_.info->mapId_,
        layer_->layerId_,
        std::vector<PartitionId>{partitions_[nextPartition_++]});
    child->sourceId_ = source_.descriptor.sourceId;
    auto weak = weak_from_this();
    child->onFeatureLayer(
        [weak](PartitionFeatureLayer::Ptr tile)
        {
            if (auto self = weak.lock()) {
                std::lock_guard lock(self->executionMutex_);
                if (self->finished_ || !self->step())
                    return;
                try {
                    self->extractFeatures(tile);
                }
                catch (std::length_error const&) {
                    if (self->incomplete_.empty())
                        self->truncate("size_limit");
                }
                catch (...) {
                    self->truncate("extraction_failed");
                }
            }
        });
    child->onSourceDataLayer(
        [weak](PartitionSourceDataLayer::Ptr tile)
        {
            if (auto self = weak.lock()) {
                std::lock_guard lock(self->executionMutex_);
                if (self->finished_ || !self->step())
                    return;
                try {
                    self->extractSourceData(tile);
                }
                catch (std::length_error const&) {
                    if (self->incomplete_.empty())
                        self->truncate("size_limit");
                }
                catch (...) {
                    self->truncate("extraction_failed");
                }
            }
        });
    child->onDone_ = [weak](RequestStatus status)
    {
        if (auto self = weak.lock()) {
            std::lock_guard lock(self->executionMutex_);
            if (self->finished_)
                return;
            {
                std::lock_guard requestLock(self->requestMutex_);
                self->request_.reset();
            }
            if (status != RequestStatus::Success) {
                self->truncate(
                    status == RequestStatus::Unauthorized ?
                        "not_available" :
                        "load_failed_or_cancelled");
                self->finishItems();
            }
            else
                self->loadNext();
        }
    };
    {
        std::lock_guard lock(requestMutex_);
        request_ = child;
    }
    // Every partition is independently admitted, even if the catalog changed since selection.
    (void)owner_.service_.request({child}, principal.datasourceHeaders);
}

void McpNativeTools::Call::extractFeatures(PartitionFeatureLayer::Ptr const& tile)
{
    if (!tile || tile->error()) {
        truncate("datasource_error");
        return;
    }
    // A catalog reload can intervene between selection and delivery. Bind the schema that
    // actually describes this payload, not the earlier catalog snapshot.
    layer_ = tile->layerInfo();
    environment(tile->strings());
    auto selectedTypes = arguments.value("featureTypes", std::vector<std::string>{});
    auto visit = [&](model_ptr<Feature> const& feature)
    {
        if (!feature || !step())
            return;
        if (!selectedTypes.empty() &&
            std::find(selectedTypes.begin(), selectedTypes.end(), feature->typeId()) ==
                selectedTypes.end())
            return;
        Json identity{
            {"mapId", tile->mapId()},
            {"layerId", layer_->layerId_},
            {"partition", tile->partitionId().toJson()},
            {"sourceId", source_.descriptor.sourceId},
            {"featureId", feature->id()->toString()}};
        if (arguments.value("scope", "feature") == "feature") {
            extractRow(
                feature,
                *feature,
                static_cast<simfil::ModelNode const&>(*feature).schema(),
                std::move(identity));
            return;
        }
        auto layers = feature->attributeLayersOrNull();
        if (!layers)
            return;
        uint32_t attributeIndex = 0;
        layers->forEachLayer(
            [&](std::string_view name, model_ptr<AttributeLayer> const& attributes)
            {
                if (!step())
                    return false;
                return attributes->forEachAttribute(
                    [&](model_ptr<Attribute> const& attribute)
                    {
                        auto index = attributeIndex++;
                        auto validities = attribute->validityOrNull();
                        auto count = validities ? validities->size() : 0;
                        for (uint32_t v = 0; v < std::max<uint32_t>(count, 1); ++v) {
                            if (!step() || items_.size() >= limit_) {
                                if (items_.size() >= limit_)
                                    truncate("item_limit");
                                return false;
                            }
                            auto context = attribute->queryContext(feature, name, index, v, count);
                            auto rootSchema = layer_->layerSchema() ?
                                layer_->layerSchema()
                                    ->attributeQuerySchema(feature->typeId(), attribute->schema()) :
                                simfil::NoSchemaId;
                            auto row = identity;
                            row["attributeIndex"] = index;
                            row["attributeLayer"] = name;
                            row["attributeName"] = attribute->name();
                            row["validityIndex"] = count ? Json(v) : Json(nullptr);
                            auto validity = count ?
                                tile->resolve<Validity>(*validities->at(v)) :
                                model_ptr<Validity>{};
                            if (!extractRow(
                                    feature,
                                    *context,
                                    rootSchema,
                                    std::move(row),
                                    validity))
                                return false;
                        }
                        return incomplete_.empty();
                    });
            });
    };
    if (!featureIds_.empty()) {
        for (auto const& id : featureIds_) {
            if (!step())
                break;
            visit(tile->find(id));
        }
    }
    else {
        for (auto const& feature : *tile) {
            if (!step())
                break;
            if (items_.size() >= limit_) {
                truncate("item_limit");
                break;
            }
            visit(feature);
        }
    }
}

bool McpNativeTools::Call::extractRow(
    model_ptr<Feature> const& feature,
    simfil::ModelNode const& context,
    simfil::SchemaId schema,
    Json identity,
    model_ptr<Validity> const& validity)
{
    if (arguments.contains("predicate")) {
        auto matches = evaluate(arguments["predicate"], context, schema, true);
        if (!incomplete_.empty())
            return false;
        if (matches.empty() || matches.front() != true)
            return true;
    }
    auto row = boundedJson(identity);
    row["values"] = Json::array();
    auto queries =
        arguments.value("expressions", std::vector<std::string>{arguments.value("query", "_")});
    for (auto const& query : queries) {
        row["values"].push_back(evaluate(query, context, schema));
        if (!incomplete_.empty())
            break;
    }
    if (arguments.value("geometry", false) && incomplete_.empty()) {
        auto geometry = feature->geomOrNull();
        if (!validity)
            row["geometry"] = geometry ?
                valueJson(simfil::Value::field(simfil::ModelNode::Ptr(geometry))) :
                Json(nullptr);
        else {
            // Self-contained/attribute-point validities also work without primary geometry.
            // Geometry math is not preemptible. Bound the source vertex domain before asking it
            // to allocate an effective slice; serialized vertices still consume the normal budget.
            if (!step(feature->model().numVertices()))
                throw std::length_error("geometry work");
            std::string error;
            auto computed = validity->computeGeometry(geometry, &error);
            if (!error.empty()) {
                row["geometry"] = nullptr;
                if (issues_.size() >= 100)
                    truncate("diagnostic_limit");
                else
                    issues_.push_back(boundedJson(
                        {{"phase", "geometry"},
                         {"message", "Validity geometry could not be resolved."}}));
            }
            else {
                auto scratch = std::make_shared<PartitionFeatureLayer>(
                    feature->model().partitionId(),
                    feature->model().stringPoolId(),
                    feature->model().mapId(),
                    layer_,
                    env_->strings());
                scratch->setGeometryAnchor(feature->model().geometryAnchor());
                auto shape = scratch->newGeometry(computed.geomType_, computed.points_.size());
                for (auto const& point : computed.points_)
                    shape->append(point);
                if (!computed.polygonRingStarts_.empty())
                    shape->setPolygonRingStarts(computed.polygonRingStarts_);
                row["geometry"] = valueJson(simfil::Value::field(simfil::ModelNode::Ptr(shape)));
            }
        }
    }
    return append(std::move(row)) && incomplete_.empty();
}

void McpNativeTools::Call::extractSourceData(PartitionSourceDataLayer::Ptr const& tile)
{
    if (!tile || tile->error()) {
        truncate("datasource_error");
        return;
    }
    layer_ = tile->layerInfo();
    environment(tile->strings());
    size_t matchCount = 0;
    auto emit = [&](simfil::ModelNode::Ptr const& node)
    {
        if (!step())
            return false;
        Json item{
            {"mapId", tile->mapId()},
            {"layerId", layer_->layerId_},
            {"sourceId", source_.descriptor.sourceId},
            {"partition", tile->partitionId().toJson()}};
        if (arguments.contains("reference")) {
            item["resolution"] = {
                {"match", arguments.value("match", "exact")},
                {"matchCount", matchCount},
                {"ambiguous", matchCount > 1},
                {"requestedReference", arguments.at("reference")}};
        }
        if (node->addr().column() == PartitionSourceDataLayer::Compound) {
            auto compound = tile->resolve<SourceDataCompoundNode>(*node);
            item["reference"] = {
                {"layerId", layer_->layerId_},
                {"address", std::to_string(compound->sourceDataAddress().u64())}};
            item["schemaName"] = compound->schemaName();
            item["addressFormat"] = tile->sourceDataAddressFormat() ==
                    PartitionSourceDataLayer::SourceDataAddressFormat::BitRange ?
                "bitRange" :
                "opaque";
        }
        item = boundedJson(item);
        item["values"] = evaluate(arguments.value("query", "_"), *node, simfil::NoSchemaId);
        return append(std::move(item)) && incomplete_.empty();
    };
    if (arguments.contains("reference")) {
        auto text = arguments["reference"]["address"].get<std::string>();
        uint64_t address = 0;
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), address);
        if (error != std::errc{} || end != text.data() + text.size())
            throw std::invalid_argument("address");
        auto matches = tile->findSourceData(
            SourceDataAddress(address),
            arguments.value("match", "exact") == "containing",
            remainingWork_,
            limit_,
            [&] { return !step(); });
        if (!matches) {
            issues_.push_back(
                {{"phase", "address"},
                 {"message",
                  "Address resolution failed or exceeded its traversal/ambiguity limit."}});
            truncate("address_resolution_failed");
            return;
        }
        matchCount = matches->size();
        for (auto const& match : *matches) {
            if (!emit(match))
                break;
        }
        if (matches->empty())
            issues_.push_back(
                {{"phase", "address"},
                 {"message", "No matching source address; the link may be stale."}});
    }
    else {
        for (size_t i = 0; i < tile->numRoots(); ++i) {
            auto root = tile->root(i);
            if (!root) {
                truncate("invalid_model_root");
                break;
            }
            if (!emit(*root))
                break;
        }
    }
}

}  // namespace mapget::detail
