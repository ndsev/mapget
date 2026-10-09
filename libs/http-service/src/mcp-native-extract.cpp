#include "mcp-native-call.h"

#include "mapget/model/attrlayer.h"
#include "mapget/model/featureid.h"
#include "mapget/model/layerschema.h"
#include "mapget/model/relation.h"
#include "mapget/model/sourcedata.h"
#include "mapget/service/locate.h"

#include <charconv>
#include <set>

namespace mapget::detail
{
using Json = nlohmann::json;

bool McpNativeTools::Call::resumeExtraction()
{
    std::shared_ptr<Continuation> checkpoint;
    {
        std::lock_guard lock(owner_.mutex_);
        auto it = owner_.continuations_.find(arguments.at("cursor").get<std::string>());
        if (it != owner_.continuations_.end())
            checkpoint = it->second;
    }
    if (!checkpoint || checkpoint->expires <= std::chrono::steady_clock::now() ||
        !checkpoint->principal.sameUser(principal) ||
        checkpoint->principal.datasourceHeaders != principal.datasourceHeaders)
    {
        fail(
            "cursor_unavailable",
            "Extraction cursor expired, was evicted, or is unavailable to this caller. Restart the "
            "original extraction.");
        return false;
    }
    if (checkpoint->catalogRevision != owner_.service_.sourceCatalogRevision()) {
        fail("cursor_stale", "The source catalog changed. Restart the original extraction.");
        return false;
    }
    auto request = std::move(arguments);
    arguments = checkpoint->arguments;
    for (auto key : {"limit", "maxWork"})
        if (request.contains(key))
            arguments[key] = request[key];
    limit_ = arguments.value("limit", size_t{100});
    remainingWork_ = arguments.value("maxWork", size_t{100000});
    // Reauthorize even retained data. The ordinary scheduler independently checks new tiles.
    selectLayer();
    if (finished_)
        return false;
    scan_ = std::make_shared<Continuation>(*checkpoint);
    initialProgress_ = scan_->progress;
    return true;
}

void McpNativeTools::Call::continueExtraction(Json& result)
{
    if ((incomplete_ != "item_limit" && incomplete_ != "byte_limit" &&
         incomplete_ != "work_limit" && incomplete_ != "evaluation_limit" &&
         incomplete_ != "expression_result_limit") ||
        scan_->progress == initialProgress_)
        return;
    if (scan_->catalogRevision != owner_.service_.sourceCatalogRevision())
        return;
    auto checkpoint = std::make_shared<Continuation>(*scan_);
    checkpoint->principal = principal;
    checkpoint->arguments = arguments;
    checkpoint->arguments["offset"] = arguments.value("offset", size_t{0}) + completeRows_;
    checkpoint->arguments["sourceId"] = source_.descriptor.sourceId;
    auto now = std::chrono::steady_clock::now();
    checkpoint->expires = std::min(
        now + std::chrono::minutes(2),
        now +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                principal.expiresAt - std::chrono::system_clock::now()));
    // Include input/selector overhead and model capacities; no serialized tile cache is added.
    checkpoint->retainedBytes = 512 * 1024 +
        checkpoint->selected.capacity() * sizeof(model_ptr<Feature>);
    checkpoint->retainedBytes += jsonMemoryUsage(checkpoint->arguments).allocatedBytes;
    if (checkpoint->tile && !checkpoint->retainedTileBytes) {
        // The service already owns the shared schema and dictionary. Charging the entire
        // Classic schema per tile would reject ordinary pages. Account for pinned tile
        // storage here; catalog changes invalidate and retire its old metadata references.
        checkpoint->retainedTileBytes = 2 * checkpoint->tile->memoryUsage().total().allocatedBytes;
    }
    checkpoint->retainedBytes += checkpoint->retainedTileBytes;
    for (auto const& [_, candidates] : checkpoint->locateCandidates) {
        checkpoint->retainedBytes += candidates.capacity() *
            sizeof(std::pair<std::string, LocateCandidate>);
        for (auto const& [requested, candidate] : candidates) {
            auto const& selector = candidate.selector_;
            checkpoint->retainedBytes += requested.capacity() +
                candidate.tileKey_.mapId_.capacity() + candidate.tileKey_.layerId_.capacity() +
                selector.typeId_.capacity();
            for (auto const* text :
                 {&selector.canonicalFeatureId_,
                  &selector.featureFilter_,
                  &selector.featureIdExpression_})
                if (*text)
                    checkpoint->retainedBytes += (*text)->capacity();
            for (auto const& [name, value] : selector.bindings_) {
                checkpoint->retainedBytes += 128 + name.capacity();
                if (auto text = std::get_if<std::string>(&value))
                    checkpoint->retainedBytes += text->capacity();
            }
        }
    }
    for (auto const& id : checkpoint->resolvedFeatureIds)
        checkpoint->retainedBytes += 128 + id.capacity();
    auto cursor = owner_.retain(std::move(checkpoint));
    if (!cursor.empty())
        result["nextCursor"] = std::move(cursor);
    else if (result["issues"].size() < 100)
        result["issues"].push_back(
            {{"phase", "budget"},
             {"message",
              "Continuation storage is unavailable. Narrow the extraction or use the legacy "
              "nextOffset if present."}});
}

void McpNativeTools::Call::prepareExtraction()
{
    auto const feature = name == "mapget_extract_features";
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
    else if (arguments.contains("partition")) {
        if (!feature || arguments.contains("partitions"))
            throw std::invalid_argument("Choose one feature partition representation");
        arguments["partitions"] = Json::array({arguments.at("partition")});
        arguments.erase("partition");
    }
    selectLayer();
    if (layer_->type_ != (feature ? LayerType::Features : LayerType::SourceData))
        throw std::invalid_argument("Layer payload type mismatch");
    if (arguments.contains("featureIds"))
        scan_->featureIds = arguments["featureIds"].get<std::vector<std::string>>();
    std::set<PartitionId> seen;
    if (arguments.contains("partitions")) {
        for (auto const& raw : arguments.at("partitions")) {
            auto id = PartitionId::fromJson(raw);
            if (id.kind() != layer_->partitionKind_)
                throw std::invalid_argument("Partition kind mismatch");
            if (seen.insert(id).second)
                scan_->partitions.push_back(id);
        }
        if (scan_->partitions.empty())
            throw std::invalid_argument("Empty partitions");
    }
    if (!arguments.contains("partitions") && (!feature || scan_->featureIds.empty()))
        throw std::invalid_argument("Partitions or feature IDs required");
    if (feature && !scan_->featureIds.empty()) {
        for (auto const& id : scan_->featureIds) {
            if (!step())
                break;
            ParsedFeatureId parsed;
            std::string error;
            if (!parseFeatureIdString(id, *layer_, parsed, &error)) {
                std::string types;
                for (auto const& type : layer_->featureTypes_) {
                    if (types.size() >= 512)
                        break;
                    if (!types.empty())
                        types += ", ";
                    types += type.name_;
                }
                fail(
                    "invalid_feature_id",
                    error.substr(0, 1024) +
                        ". Expected a full type-prefixed feature ID, not a local/numeric ID. "
                        "Known types in this layer: " +
                        types +
                        ". mapget_list_sources with this mapId/layerId returns the ordered "
                        "featureTypes[].uniqueIdCompositions.");
                return;
            }
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
                if (arguments.contains("partitions") && !seen.contains(key.partitionId_))
                    continue;
                scan_->locateCandidates[key.partitionId_].emplace_back(id, candidate);
                if (seen.insert(key.partitionId_).second)
                    scan_->partitions.push_back(key.partitionId_);
                if (scan_->partitions.size() > 32)
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
    if (scan_->tile && incomplete_.empty()) {
        try {
            // Keep a local owner: successful exhaustion releases the checkpoint's reference.
            auto tile = scan_->tile;
            extractFeatures(tile);
        }
        catch (std::length_error const&) {
            if (incomplete_.empty())
                truncate("size_limit");
        }
    }
    if (!step() || scan_->nextPartition >= scan_->partitions.size()) {
        finishItems();
        return;
    }
    if (name != "mapget_extract_features" && items_.size() >= limit_) {
        truncate("item_limit");
        finishItems();
        return;
    }
    auto child = std::make_shared<LayerTilesRequest>(
        source_.info->mapId_,
        layer_->layerId_,
        std::vector<PartitionId>{scan_->partitions[scan_->nextPartition++]});
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
    scan_->tile = tile;
    layer_ = tile->layerInfo();
    environment(tile->strings());
    auto selectedTypes = arguments.contains("featureType") ?
        std::vector<std::string>{arguments["featureType"].get<std::string>()} :
        arguments.value("featureTypes", std::vector<std::string>{});
    if (!scan_->selectionReady && !scan_->featureIds.empty()) {
        scan_->selected.clear();
        std::set<std::string> emitted;
        auto resolved = [&](std::string const& requested, model_ptr<Feature> const& feature)
        {
            if (!feature)
                return;
            scan_->resolvedFeatureIds.insert(requested);
            if (emitted.insert(feature->id()->toString()).second)
                scan_->selected.push_back(feature);
        };
        for (auto const& id : scan_->featureIds) {
            if (!step())
                return;
            resolved(id, tile->find(id));
        }
        if (auto candidates = scan_->locateCandidates.find(tile->partitionId());
            candidates != scan_->locateCandidates.end())
        {
            for (auto const& [requested, candidate] : candidates->second) {
                if (!step())
                    return;
                auto selected =
                    resolveLocateCandidate(candidate, *tile, [this] { return !step(); });
                if (!selected) {
                    truncate("locate_resolution_failed");
                    return;
                }
                for (auto const& feature : *selected) {
                    if (!step())
                        return;
                    resolved(requested, feature);
                }
            }
        }
    }
    scan_->selectionReady = true;
    auto count = scan_->featureIds.empty() ? tile->size() : scan_->selected.size();
    // Position indices address the pinned model directly. In particular, never replay the
    // predicates for earlier features, assignments or validities to reach the next page.
    while (scan_->feature < count) {
        if (!step())
            return;
        auto feature = scan_->featureIds.empty() ?
            tile->at(scan_->feature) :
            scan_->selected[scan_->feature];
        if (feature &&
            (selectedTypes.empty() ||
             std::find(selectedTypes.begin(), selectedTypes.end(), feature->typeId()) !=
                 selectedTypes.end()))
        {
            Json identity{
                {"mapId", tile->mapId()},
                {"layerId", layer_->layerId_},
                {"partition", tile->partitionId().toJson()},
                {"sourceId", source_.descriptor.sourceId},
                {"featureId", feature->id()->toString()}};
            if (arguments.value("scope", "feature") == "feature") {
                if (!extractRow(
                        feature,
                        *feature,
                        static_cast<simfil::ModelNode const&>(*feature).schema(),
                        identity))
                    return;
            }
            else if (auto layers = feature->attributeLayersOrNull()) {
                while (scan_->attributeLayer < layers->size()) {
                    if (!step())
                        return;
                    auto layerNode = layers->at(scan_->attributeLayer);
                    if (layerNode &&
                        layerNode->addr().column() ==
                            PartitionFeatureLayer::ColumnId::AttributeLayers) {
                        // Merged layers can belong to an add-on's dictionary/model.
                        auto name = layerNode->owningModel()
                                        ->lookupStringId(layers->keyAt(scan_->attributeLayer))
                                        .value_or("");
                        auto attributes = tile->resolve<AttributeLayer>(*layerNode);
                        while (scan_->attributeMember < attributes->size()) {
                            if (!step())
                                return;
                            auto node = attributes->at(scan_->attributeMember);
                            if (node &&
                                node->addr().column() ==
                                    PartitionFeatureLayer::ColumnId::Attributes) {
                                auto attribute = tile->resolve<Attribute>(*node);
                                auto index = scan_->attributeIndex;
                                if ((!arguments.contains("attributeIndex") ||
                                     arguments["attributeIndex"] == index) &&
                                    (!arguments.contains("attributeName") ||
                                     std::string_view(arguments["attributeName"]
                                                          .get_ref<std::string const&>()) ==
                                         attribute->name()) &&
                                    (!arguments.contains("attributeLayer") ||
                                     std::string_view(arguments["attributeLayer"]
                                                          .get_ref<std::string const&>()) == name))
                                {
                                    auto validities = attribute->validityOrNull();
                                    auto validityCount = validities ? validities->size() : 0;
                                    while (scan_->validity < std::max<uint32_t>(validityCount, 1)) {
                                        if (!step())
                                            return;
                                        auto v = scan_->validity;
                                        auto context = attribute->queryContext(
                                            feature,
                                            name,
                                            index,
                                            v,
                                            validityCount);
                                        auto schema = layer_->layerSchema() ?
                                            layer_->layerSchema()->attributeQuerySchema(
                                                feature->typeId(),
                                                attribute->schema()) :
                                            simfil::NoSchemaId;
                                        auto row = identity;
                                        row["attributeIndex"] = index;
                                        row["attributeLayer"] = name;
                                        row["attributeName"] = attribute->name();
                                        row["validityIndex"] = validityCount ?
                                            Json(v) :
                                            Json(nullptr);
                                        auto validity = validityCount ?
                                            tile->resolve<Validity>(*validities->at(v)) :
                                            model_ptr<Validity>{};
                                        if (!extractRow(
                                                feature,
                                                *context,
                                                schema,
                                                std::move(row),
                                                validity))
                                            return;
                                        ++scan_->validity;
                                        ++scan_->progress;
                                    }
                                }
                                ++scan_->attributeIndex;
                            }
                            ++scan_->attributeMember;
                            scan_->validity = 0;
                            ++scan_->progress;
                        }
                    }
                    ++scan_->attributeLayer;
                    scan_->attributeMember = 0;
                    ++scan_->progress;
                }
            }
        }
        ++scan_->feature;
        scan_->attributeLayer = scan_->attributeMember = scan_->attributeIndex = scan_->validity =
            0;
        ++scan_->progress;
    }
    ++scan_->progress;
    scan_->tile.reset();
    scan_->retainedTileBytes = 0;
    scan_->selected.clear();
    scan_->selectionReady = false;
    scan_->feature = 0;
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
    if (scan_->offsetRemaining) {
        --scan_->offsetRemaining;
        return true;
    }
    // Probe the next matching context before declaring a full page incomplete.
    if (items_.size() >= limit_) {
        truncate("item_limit");
        return false;
    }
    auto row = boundedJson(identity);
    if (!arguments.contains("query") && !arguments.contains("expressions") &&
        arguments.value("scope", "feature") == "feature")
    {
        row["summary"] = featureSummary(feature);
    }
    else {
        row["values"] = Json::array();
        auto queries =
            arguments.value("expressions", std::vector<std::string>{arguments.value("query", "_")});
        for (auto const& query : queries) {
            row["values"].push_back(evaluate(query, context, schema));
            if (!incomplete_.empty())
                break;
        }
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
    row["rowComplete"] = incomplete_.empty();
    if (incomplete_.empty())
        ++completeRows_;
    return append(std::move(row)) && incomplete_.empty();
}

Json McpNativeTools::Call::fieldSummary(simfil::ModelNode const& node)
{
    Json result = Json::array();
    for (uint32_t i = 0; i < node.size(); ++i) {
        if (!step())
            throw std::length_error("summary work");
        auto child = node.at(i);
        if (!child)
            continue;
        auto value = simfil::Value::field(child);
        auto item = boundedJson(
            {{"name", env_->strings()->resolve(node.keyAt(i)).value_or("")},
             {"kind", simfil::valueType2String(value.type)}});
        if (value.type == simfil::ValueType::Object || value.type == simfil::ValueType::Array)
            item["childCount"] = child->size();
        else if (value.type == simfil::ValueType::Bytes)
            item["byteCount"] = value.as<simfil::ValueType::Bytes>().bytes.size();
        else if (value.type == simfil::ValueType::String) {
            auto view = std::get_if<std::string_view>(&value.value);
            std::string_view text = view ?
                *view :
                std::string_view(std::get<std::string>(value.value));
            if (text.size() > 512)
                item["stringBytes"] = text.size();
            else
                item["value"] = valueJson(value);
        }
        else
            item["value"] = valueJson(value);
        result.push_back(std::move(item));
    }
    return result;
}

Json McpNativeTools::Call::featureSummary(model_ptr<Feature> const& feature)
{
    Json result = boundedJson(
        {{"typeId", feature->typeId()},
         {"properties", Json::array()},
         {"attributeAssignments", Json::array()},
         {"geometries", Json::array()},
         {"relationNames", Json::array()},
         {"relationCount", feature->numRelations()}});
    result["idParts"] = Json::object();
    for (auto const& [key, value] : feature->id()->keyValuePairs()) {
        if (!step())
            throw std::length_error("summary identity work");
        chargeString(key);
        result["idParts"][std::string(key)] = std::visit(
            [&](auto const& part) { return valueJson(simfil::Value::make(part)); },
            value);
    }
    if (auto properties = feature->mergedAttributesOrNull())
        result["properties"] = fieldSummary(*properties);
    if (auto layers = feature->attributeLayersOrNull()) {
        uint32_t index = 0;
        layers->forEachLayer(
            [&](std::string_view name, model_ptr<AttributeLayer> const& layer)
            {
                return layer->forEachAttribute(
                    [&](model_ptr<Attribute> const& attribute)
                    {
                        if (!step())
                            throw std::length_error("summary work");
                        auto validity = attribute->validityOrNull();
                        auto item = boundedJson(
                            {{"layer", name},
                             {"name", attribute->name()},
                             {"attributeIndex", index++},
                             {"validityCount", validity ? validity->size() : 0}});
                        if (layer->id())
                            item["layerInstanceId"] = boundedJson(*layer->id());
                        item["fields"] = fieldSummary(*attribute);
                        result["attributeAssignments"].push_back(std::move(item));
                        return true;
                    });
            });
    }
    if (auto geometry = feature->geomOrNull()) {
        geometry->forEachGeometry(
            [&](model_ptr<Geometry> const& shape)
            {
                if (!step())
                    throw std::length_error("summary work");
                auto item = boundedJson({{"pointCount", shape->numPoints()}});
                if (auto name = shape->name())
                    item["name"] = boundedJson(*name);
                if (auto type =
                        static_cast<simfil::ModelNode const&>(*shape).get(StringPool::TypeStr))
                    item["type"] = valueJson(simfil::Value::field(type));
                result["geometries"].push_back(std::move(item));
                return true;
            });
    }
    std::map<std::string, size_t> relations;
    feature->forEachRelation(
        [&](model_ptr<Relation> const& relation)
        {
            if (!step())
                throw std::length_error("summary work");
            auto name = relation->name();
            if (!relations.contains(std::string(name)))
                chargeString(name);
            ++relations[std::string(name)];
            return true;
        });
    for (auto const& [name, count] : relations)
        result["relationNames"].push_back(boundedJson({{"name", name}, {"count", count}}));
    if (auto references = feature->sourceDataReferences())
        result["sourceDataReferences"] =
            valueJson(simfil::Value::field(simfil::ModelNode::Ptr(references)));
    return result;
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
            item["rootReadArguments"] = {
                {"mapId", tile->mapId()},
                {"layerId", layer_->layerId_},
                {"sourceId", source_.descriptor.sourceId},
                {"partitions", Json::array({tile->partitionId().toJson()})}};
            item["scopeNote"] =
                "Resolved source subrecord only. For enclosing source/SQL provenance, "
                "call mapget_extract_source_data with rootReadArguments and a focused query "
                "for that datasource. This reference is not necessarily a SQL row or column.";
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
