#include "mcp-mapget-extraction.h"
#include "mcp-mapget-query.h"

#include "mapget/model/attrlayer.h"
#include "mapget/model/featureid.h"
#include "mapget/model/layerschema.h"
#include "mapget/model/mapschema.h"
#include "mapget/model/relation.h"
#include "mapget/model/sourcedata.h"
#include "mapget/service/locate.h"

#include <charconv>
#include <set>

namespace mapget::detail
{
using Json = nlohmann::json;

void McpMapgetTools::Call::Extraction::run()
{
    if (call_.arguments.contains("cursor")) {
        if (!resumeExtraction())
            return;
    }
    else {
        scan_->catalogRevision = call_.owner_.service_.sourceCatalogRevision();
        scan_->offsetRemaining = call_.arguments.value("offset", size_t{0});
        prepareExtraction();
    }
    if (!call_.finished_)
        loadNext();
}

void McpMapgetTools::Call::Extraction::cancel()
{
    // Call has already published cancellation; copy the child under its own lock.
    // Abort may invoke child callbacks; their execution mutex serializes result mutation.
    LayerTilesRequest::Ptr request;
    {
        std::lock_guard lock(requestMutex_);
        request = request_;
    }
    if (request) {
        auto* service = &call_.owner_.service_;
        // abort may synchronously complete a request; never enter its callback mutex on an I/O
        // thread.
        (void)service->scheduleTask([service, request](bool) { service->abort(request); });
    }
}

void McpMapgetTools::Call::Extraction::reportMissingFeatures()
{
    // A fully evaluated lookup with no matching identity is distinct from an empty query result.
    if (call_.name == "mapget_extract_features" && call_.incomplete_.empty()) {
        for (auto const& id : scan_->featureIds) {
            if (scan_->resolvedFeatureIds.contains(id))
                continue;
            if (call_.issues_.size() >= 100) {
                call_.truncate("diagnostic_limit");
                break;
            }
            try {
                call_.issues_.push_back(call_.query_->boundedJson(
                    {{"phase", "locate"},
                     {"featureId", id},
                     {"message",
                      "Requested identity did not resolve in the selected partitions. "
                      "Verify the target's owning layer and partition; identifier metadata "
                      "may include reference-only types."}}));
            }
            catch (std::length_error const&) {
                break;
            }
        }
    }
}

bool McpMapgetTools::Call::Extraction::resumeExtraction()
{
    std::shared_ptr<Continuation> checkpoint;
    {
        std::lock_guard lock(call_.owner_.mutex_);
        auto it = call_.owner_.continuations_.find(call_.arguments.at("cursor").get<std::string>());
        if (it != call_.owner_.continuations_.end())
            checkpoint = it->second;
    }
    if (!checkpoint || checkpoint->expires <= std::chrono::steady_clock::now() ||
        !checkpoint->principal.sameUser(call_.principal) ||
        checkpoint->principal.datasourceAuthorization != call_.principal.datasourceAuthorization)
    {
        call_.fail(
            "cursor_unavailable",
            "Extraction cursor expired, was evicted, or is unavailable to this caller. Restart the "
            "original extraction.");
        return false;
    }
    if (checkpoint->catalogRevision != call_.owner_.service_.sourceCatalogRevision()) {
        call_.fail("cursor_stale", "The source catalog changed. Restart the original extraction.");
        return false;
    }
    // A cursor fixes the query and source. Only page/work limits may change when
    // resuming; copy its immutable position so concurrent readers do not advance it.
    auto request = std::move(call_.arguments);
    call_.arguments = checkpoint->arguments;
    for (auto key : {"limit", "maxWork"})
        if (request.contains(key))
            call_.arguments[key] = request[key];
    call_.limit_ = call_.arguments.value("limit", size_t{100});
    call_.remainingWork_ = call_.arguments.value("maxWork", size_t{100000});
    // Reauthorize even retained data. The ordinary scheduler independently checks new tiles.
    call_.selectLayer();
    if (call_.finished_)
        return false;
    scan_ = std::make_shared<Continuation>(*checkpoint);
    initialProgress_ = scan_->progress;
    return true;
}

void McpMapgetTools::Call::Extraction::continueExtraction(Json& result)
{
    if ((call_.incomplete_ != "item_limit" && call_.incomplete_ != "byte_limit" &&
         call_.incomplete_ != "work_limit" && call_.incomplete_ != "evaluation_limit" &&
         call_.incomplete_ != "expression_result_limit") ||
        scan_->progress == initialProgress_)
        return;
    if (scan_->catalogRevision != call_.owner_.service_.sourceCatalogRevision())
        return;
    auto checkpoint = std::make_shared<Continuation>(*scan_);
    checkpoint->principal = call_.principal;
    checkpoint->arguments = call_.arguments;
    checkpoint->arguments["offset"] = call_.arguments.value("offset", size_t{0}) +
        call_.completeRows_;
    checkpoint->arguments["sourceId"] = call_.source_.descriptor.sourceId;
    auto now = std::chrono::steady_clock::now();
    checkpoint->expires = std::min(
        now + std::chrono::minutes(2),
        now +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                call_.principal.expiresAt - std::chrono::system_clock::now()));
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
    auto cursor = call_.owner_.retain(std::move(checkpoint));
    if (!cursor.empty())
        result["nextCursor"] = std::move(cursor);
    else if (result["issues"].size() < 100)
        result["issues"].push_back(
            {{"phase", "budget"},
             {"message",
              "Continuation storage is unavailable. Narrow the extraction or use the legacy "
              "nextOffset if present."}});
}

void McpMapgetTools::Call::Extraction::prepareExtraction()
{
    auto const feature = call_.name == "mapget_extract_features";
    if (call_.arguments.contains("query") && call_.arguments.contains("expressions"))
        throw std::invalid_argument("Choose query or expressions");
    if (call_.arguments.contains("reference")) {
        if (call_.arguments.contains("partitions") || !call_.arguments.contains("partition"))
            throw std::invalid_argument("A reference needs its owning partition");
        auto const& reference = call_.arguments.at("reference");
        if (call_.arguments.contains("layerId") &&
            call_.arguments["layerId"] != reference["layerId"])
            throw std::invalid_argument("Reference layer differs from target layer");
        call_.arguments["layerId"] = reference.at("layerId");
        call_.arguments["partitions"] = Json::array({call_.arguments.at("partition")});
    }
    else if (call_.arguments.contains("partition")) {
        if (!feature || call_.arguments.contains("partitions"))
            throw std::invalid_argument("Choose one feature partition representation");
        call_.arguments["partitions"] = Json::array({call_.arguments.at("partition")});
        call_.arguments.erase("partition");
    }
    call_.selectLayer();
    if (call_.layer_->type_ != (feature ? LayerType::Features : LayerType::SourceData))
        throw std::invalid_argument("Layer payload type mismatch");
    if (call_.arguments.contains("featureIds"))
        scan_->featureIds = call_.arguments["featureIds"].get<std::vector<std::string>>();
    std::set<PartitionId> seen;
    if (call_.arguments.contains("partitions")) {
        for (auto const& raw : call_.arguments.at("partitions")) {
            auto id = PartitionId::fromJson(raw);
            if (id.kind() != call_.layer_->partitionKind_)
                throw std::invalid_argument("Partition kind mismatch");
            if (seen.insert(id).second)
                scan_->partitions.push_back(id);
        }
        if (scan_->partitions.empty())
            throw std::invalid_argument("Empty partitions");
    }
    if (!call_.arguments.contains("partitions") && (!feature || scan_->featureIds.empty()))
        throw std::invalid_argument("Partitions or feature IDs required");
    if (feature && !scan_->featureIds.empty()) {
        for (auto const& id : scan_->featureIds) {
            if (!call_.step())
                break;
            ParsedFeatureId parsed;
            std::string error;
            if (!parseFeatureIdString(id, *call_.layer_, parsed, &error)) {
                std::string types;
                for (auto const& type : call_.layer_->layerSchema()->featureTypes()) {
                    if (types.size() >= 512)
                        break;
                    if (!types.empty())
                        types += ", ";
                    types += type;
                }
                call_.fail(
                    "invalid_feature_id",
                    error.substr(0, 1024) +
                        ". Expected a full type-prefixed feature ID, not a local/numeric ID. "
                        "Known types in this layer: " +
                        types +
                        ". mapget_list_sources with this mapId/layerId returns the ordered "
                        "featureTypes[].uniqueIdCompositions.");
                return;
            }
            LocateRequest
                request(call_.arguments.at("mapId"), parsed.typeId_, parsed.keyValuePairs_);
            request.layerId_ = call_.layer_->layerId_;
            // Datasource locate produces cheap routing candidates. Never use Service::locate(),
            // whose synchronous wait would deadlock a single-worker service.
            for (auto const& candidate : call_.source_.dataSource->locate(request)) {
                if (!call_.step())
                    break;
                auto const& key = candidate.tileKey_;
                if (key.mapId_ != call_.source_.info->mapId_ ||
                    key.layerId_ != call_.layer_->layerId_ || key.layer_ != LayerType::Features ||
                    key.partitionId_.kind() != call_.layer_->partitionKind_)
                    continue;
                if (call_.arguments.contains("partitions") && !seen.contains(key.partitionId_))
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
}

void McpMapgetTools::Call::Extraction::loadNext()
{
    if (scan_->tile && call_.incomplete_.empty()) {
        try {
            // Keep a local owner: successful exhaustion releases the checkpoint's reference.
            auto tile = scan_->tile;
            extractFeatures(tile);
        }
        catch (std::length_error const&) {
            if (call_.incomplete_.empty())
                call_.truncate("size_limit");
        }
    }
    if (!call_.step() || scan_->nextPartition >= scan_->partitions.size()) {
        call_.finishItems();
        return;
    }
    if (call_.name != "mapget_extract_features" && call_.items_.size() >= call_.limit_) {
        call_.truncate("item_limit");
        call_.finishItems();
        return;
    }
    // Retain just one outstanding partition. Its terminal callback schedules the
    // next load after this partition's rows have consumed their shared budgets.
    auto child = std::make_shared<LayerTilesRequest>(
        call_.source_.info->mapId_,
        call_.layer_->layerId_,
        std::vector<PartitionId>{scan_->partitions[scan_->nextPartition++]});
    child->sourceId_ = call_.source_.descriptor.sourceId;
    auto weak = call_.weak_from_this();
    child->onFeatureLayer(
        [weak](PartitionFeatureLayer::Ptr tile)
        {
            if (auto self = weak.lock()) {
                std::lock_guard lock(self->executionMutex_);
                if (self->finished_ || !self->step())
                    return;
                try {
                    self->extraction_->extractFeatures(tile);
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
                    self->extraction_->extractSourceData(tile);
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
                std::lock_guard requestLock(self->extraction_->requestMutex_);
                self->extraction_->request_.reset();
            }
            if (status != RequestStatus::Success) {
                self->truncate(
                    status == RequestStatus::Unauthorized ?
                        "not_available" :
                        "load_failed_or_cancelled");
                self->finishItems();
            }
            else
                self->extraction_->loadNext();
        }
    };
    {
        std::lock_guard lock(requestMutex_);
        request_ = child;
    }
    // Every partition is independently admitted, even if the catalog changed since selection.
    (void)call_.owner_.service_.request({child}, call_.principal.datasourceAuthorization);
}

void McpMapgetTools::Call::Extraction::extractFeatures(PartitionFeatureLayer::Ptr const& tile)
{
    if (!tile || tile->error()) {
        call_.truncate("datasource_error");
        return;
    }
    scan_->tile = tile;
    call_.layer_ = tile->layerInfo();
    call_.query_->environment(tile->strings());
    auto selectedTypes = call_.arguments.contains("featureType") ?
        std::vector<std::string>{call_.arguments["featureType"].get<std::string>()} :
        call_.arguments.value("featureTypes", std::vector<std::string>{});
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
            if (!call_.step())
                return;
            resolved(id, tile->find(id));
        }
        if (auto candidates = scan_->locateCandidates.find(tile->partitionId());
            candidates != scan_->locateCandidates.end())
        {
            for (auto const& [requested, candidate] : candidates->second) {
                if (!call_.step())
                    return;
                auto selected =
                    resolveLocateCandidate(candidate, *tile, [this] { return !call_.step(); });
                if (!selected) {
                    call_.truncate("locate_resolution_failed");
                    return;
                }
                for (auto const& feature : *selected) {
                    if (!call_.step())
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
        if (!call_.step())
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
                {"layerId", call_.layer_->layerId_},
                {"partition", tile->partitionId().toJson()},
                {"sourceId", call_.source_.descriptor.sourceId},
                {"featureId", feature->id()->toString()}};
            if (call_.arguments.value("scope", "feature") == "feature") {
                if (!extractRow(
                        feature,
                        *feature,
                        static_cast<simfil::ModelNode const&>(*feature).schema(),
                        identity))
                    return;
            }
            else if (auto layers = feature->attributeLayersOrNull()) {
                while (scan_->attributeLayer < layers->size()) {
                    if (!call_.step())
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
                            if (!call_.step())
                                return;
                            auto node = attributes->at(scan_->attributeMember);
                            if (node &&
                                node->addr().column() ==
                                    PartitionFeatureLayer::ColumnId::Attributes) {
                                auto attribute = tile->resolve<Attribute>(*node);
                                auto index = scan_->attributeIndex;
                                if ((!call_.arguments.contains("attributeIndex") ||
                                     call_.arguments["attributeIndex"] == index) &&
                                    (!call_.arguments.contains("attributeName") ||
                                     std::string_view(call_.arguments["attributeName"]
                                                          .get_ref<std::string const&>()) ==
                                         attribute->name()) &&
                                    (!call_.arguments.contains("attributeLayer") ||
                                     std::string_view(call_.arguments["attributeLayer"]
                                                          .get_ref<std::string const&>()) == name))
                                {
                                    auto validities = attribute->validityOrNull();
                                    auto validityCount = validities ? validities->size() : 0;
                                    while (scan_->validity < std::max<uint32_t>(validityCount, 1)) {
                                        if (!call_.step())
                                            return;
                                        auto v = scan_->validity;
                                        auto context = attribute->queryContext(
                                            feature,
                                            name,
                                            index,
                                            v,
                                            validityCount);
                                        auto schema = tile->layerSchema() ?
                                            tile->layerSchema()->attributeQuerySchema(
                                                feature->typeId(),
                                                attribute->schema()) :
                                            simfil::NoTypeId;
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

bool McpMapgetTools::Call::Extraction::extractRow(
    model_ptr<Feature> const& feature,
    simfil::ModelNode const& context,
    simfil::TypeId schema,
    Json identity,
    model_ptr<Validity> const& validity)
{
    if (call_.arguments.contains("predicate")) {
        auto matches = call_.query_->evaluate(call_.arguments["predicate"], context, schema, true);
        if (!call_.incomplete_.empty())
            return false;
        if (matches.empty() || matches.front() != true)
            return true;
    }
    if (scan_->offsetRemaining) {
        --scan_->offsetRemaining;
        return true;
    }
    // Probe the next matching context before declaring a full page incomplete.
    if (call_.items_.size() >= call_.limit_) {
        call_.truncate("item_limit");
        return false;
    }
    auto row = call_.query_->boundedJson(identity);
    if (auto owner = call_.layer_->schemaOwner())
        row["mapSchemaId"] = call_.query_->boundedJson(owner->schemaId());
    if (!call_.arguments.contains("query") && !call_.arguments.contains("expressions") &&
        call_.arguments.value("scope", "feature") == "feature")
    {
        row["summary"] = featureSummary(feature);
    }
    else {
        row["values"] = Json::array();
        auto queries = call_.arguments.value(
            "expressions",
            std::vector<std::string>{call_.arguments.value("query", "_")});
        for (auto const& query : queries) {
            row["values"].push_back(call_.query_->evaluate(query, context, schema));
            if (!call_.incomplete_.empty())
                break;
        }
    }
    if (call_.arguments.value("geometry", false) && call_.incomplete_.empty()) {
        auto geometry = feature->geomOrNull();
        if (!validity)
            row["geometry"] = geometry ?
                call_.query_->valueJson(simfil::Value::field(simfil::ModelNode::Ptr(geometry))) :
                Json(nullptr);
        else {
            // Self-contained/attribute-point validities also work without primary geometry.
            // Geometry math is not preemptible. Bound the source vertex domain before asking it
            // to allocate an effective slice; serialized vertices still consume the normal budget.
            if (!call_.step(feature->model().numVertices()))
                throw std::length_error("geometry work");
            std::string error;
            auto computed = validity->computeGeometry(geometry, &error);
            if (!error.empty()) {
                row["geometry"] = nullptr;
                if (call_.issues_.size() >= 100)
                    call_.truncate("diagnostic_limit");
                else
                    call_.issues_.push_back(call_.query_->boundedJson(
                        {{"phase", "geometry"},
                         {"message", "Validity geometry could not be resolved."}}));
            }
            else {
                auto scratch = std::make_shared<PartitionFeatureLayer>(
                    feature->model().partitionId(),
                    feature->model().stringPoolId(),
                    feature->model().mapId(),
                    call_.layer_,
                    call_.query_->runtime()->strings());
                scratch->setGeometryAnchor(feature->model().geometryAnchor());
                auto shape = scratch->newGeometry(computed.geomType_, computed.points_.size());
                for (auto const& point : computed.points_)
                    shape->append(point);
                if (!computed.polygonRingStarts_.empty())
                    shape->setPolygonRingStarts(computed.polygonRingStarts_);
                row["geometry"] =
                    call_.query_->valueJson(simfil::Value::field(simfil::ModelNode::Ptr(shape)));
            }
        }
    }
    row["rowComplete"] = call_.incomplete_.empty();
    if (call_.incomplete_.empty())
        ++call_.completeRows_;
    return call_.append(std::move(row)) && call_.incomplete_.empty();
}

Json McpMapgetTools::Call::Extraction::fieldSummary(simfil::ModelNode const& node)
{
    Json result = Json::array();
    for (uint32_t i = 0; i < node.size(); ++i) {
        if (!call_.step())
            throw std::length_error("summary work");
        auto child = node.at(i);
        if (!child)
            continue;
        auto value = simfil::Value::at(node, *child, i);
        auto item = call_.query_->boundedJson(
            {{"name", call_.query_->runtime()->strings()->resolve(node.keyAt(i)).value_or("")},
             {"kind", simfil::valueType2String(value.type)}});
        if (value.type == simfil::ValueType::Object || value.type == simfil::ValueType::Array)
            item["childCount"] = child->size();
        else if (value.type == simfil::ValueType::Bytes)
            item["byteCount"] = value.as<simfil::ValueType::Bytes>().bytes.size();
        else if (value.type == simfil::ValueType::String) {
            auto text = value.stringView();
            if (text.size() > 512)
                item["stringBytes"] = text.size();
            else
                item["value"] = call_.query_->valueJson(value);
        }
        else
            item["value"] = call_.query_->valueJson(value);
        result.push_back(std::move(item));
    }
    return result;
}

Json McpMapgetTools::Call::Extraction::featureSummary(model_ptr<Feature> const& feature)
{
    Json result = call_.query_->boundedJson(
        {{"typeId", feature->typeId()},
         {"properties", Json::array()},
         {"attributeAssignments", Json::array()},
         {"geometries", Json::array()},
         {"relationNames", Json::array()},
         {"relationCount", feature->numRelations()}});
    result["idParts"] = Json::object();
    for (auto const& [key, value] : feature->id()->keyValuePairs()) {
        if (!call_.step())
            throw std::length_error("summary identity work");
        call_.query_->chargeString(key);
        result["idParts"][std::string(key)] = std::visit(
            [&](auto const& part) { return call_.query_->valueJson(simfil::Value::make(part)); },
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
                        if (!call_.step())
                            throw std::length_error("summary work");
                        auto validity = attribute->validityOrNull();
                        auto item = call_.query_->boundedJson(
                            {{"layer", name},
                             {"name", attribute->name()},
                             {"attributeIndex", index++},
                             {"validityCount", validity ? validity->size() : 0}});
                        if (layer->id())
                            item["layerInstanceId"] = call_.query_->boundedJson(*layer->id());
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
                if (!call_.step())
                    throw std::length_error("summary work");
                auto item = call_.query_->boundedJson({{"pointCount", shape->numPoints()}});
                if (auto name = shape->name())
                    item["name"] = call_.query_->boundedJson(*name);
                if (auto type =
                        static_cast<simfil::ModelNode const&>(*shape).get(StringPool::TypeStr))
                    item["type"] = call_.query_->valueJson(simfil::Value::field(type));
                result["geometries"].push_back(std::move(item));
                return true;
            });
    }
    std::map<std::string, size_t> relations;
    feature->forEachRelation(
        [&](model_ptr<Relation> const& relation)
        {
            if (!call_.step())
                throw std::length_error("summary work");
            auto name = relation->name();
            if (!relations.contains(std::string(name)))
                call_.query_->chargeString(name);
            ++relations[std::string(name)];
            return true;
        });
    for (auto const& [name, count] : relations)
        result["relationNames"]
            .push_back(call_.query_->boundedJson({{"name", name}, {"count", count}}));
    if (auto references = feature->sourceDataReferences())
        result["sourceDataReferences"] =
            call_.query_->valueJson(simfil::Value::field(simfil::ModelNode::Ptr(references)));
    return result;
}

void McpMapgetTools::Call::Extraction::extractSourceData(PartitionSourceDataLayer::Ptr const& tile)
{
    if (!tile || tile->error()) {
        call_.truncate("datasource_error");
        return;
    }
    call_.layer_ = tile->layerInfo();
    call_.query_->environment(tile->strings());
    size_t matchCount = 0;
    auto emit = [&](simfil::ModelNode::Ptr const& node)
    {
        if (!call_.step())
            return false;
        Json item{
            {"mapId", tile->mapId()},
            {"layerId", call_.layer_->layerId_},
            {"sourceId", call_.source_.descriptor.sourceId},
            {"partition", tile->partitionId().toJson()}};
        if (call_.arguments.contains("reference")) {
            item["resolution"] = {
                {"match", call_.arguments.value("match", "exact")},
                {"matchCount", matchCount},
                {"ambiguous", matchCount > 1},
                {"requestedReference", call_.arguments.at("reference")}};
            item["rootReadArguments"] = {
                {"mapId", tile->mapId()},
                {"layerId", call_.layer_->layerId_},
                {"sourceId", call_.source_.descriptor.sourceId},
                {"partitions", Json::array({tile->partitionId().toJson()})}};
            item["scopeNote"] =
                "Resolved source subrecord only. For enclosing source/SQL provenance, "
                "call mapget_extract_source_data with rootReadArguments and a focused query "
                "for that datasource. This reference is not necessarily a SQL row or column.";
        }
        if (node->addr().column() == PartitionSourceDataLayer::Compound) {
            auto compound = tile->resolve<SourceDataCompoundNode>(*node);
            item["reference"] = {
                {"layerId", call_.layer_->layerId_},
                {"address", std::to_string(compound->sourceDataAddress().u64())}};
            item["schemaName"] = compound->schemaName();
            item["addressFormat"] = tile->sourceDataAddressFormat() ==
                    PartitionSourceDataLayer::SourceDataAddressFormat::BitRange ?
                "bitRange" :
                "opaque";
        }
        item = call_.query_->boundedJson(item);
        if (auto owner = call_.layer_->schemaOwner())
            item["mapSchemaId"] = call_.query_->boundedJson(owner->schemaId());
        item["values"] =
            call_.query_->evaluate(call_.arguments.value("query", "_"), *node, node->schema());
        return call_.append(std::move(item)) && call_.incomplete_.empty();
    };
    if (call_.arguments.contains("reference")) {
        auto text = call_.arguments["reference"]["address"].get<std::string>();
        uint64_t address = 0;
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), address);
        if (error != std::errc{} || end != text.data() + text.size())
            throw std::invalid_argument("address");
        auto matches = tile->findSourceData(
            SourceDataAddress(address),
            call_.arguments.value("match", "exact") == "containing",
            call_.remainingWork_,
            call_.limit_,
            [&] { return !call_.step(); });
        if (!matches) {
            call_.issues_.push_back(
                {{"phase", "address"},
                 {"message",
                  "Address resolution failed or exceeded its traversal/ambiguity limit."}});
            call_.truncate("address_resolution_failed");
            return;
        }
        matchCount = matches->size();
        for (auto const& match : *matches) {
            if (!emit(match))
                break;
        }
        if (matches->empty())
            call_.issues_.push_back(
                {{"phase", "address"},
                 {"message", "No matching source address; the link may be stale."}});
    }
    else {
        for (size_t i = 0; i < tile->numRoots(); ++i) {
            auto root = tile->root(i);
            if (!root) {
                call_.truncate("invalid_model_root");
                break;
            }
            if (!emit(*root))
                break;
        }
    }
}

}  // namespace mapget::detail
