#include "mcp-mapget-call.h"
#include "mcp-mapget-extraction.h"
#include "mcp-mapget-query.h"

#include "mapget/model/layerschema.h"
#include "mapget/model/mapschema.h"
#include "ndsmath/wgs84.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

namespace mapget::detail
{
using Json = nlohmann::json;

McpMapgetTools::Call::
    Call(McpMapgetTools& owner, Principal caller, std::string action, Json args, Complete callback)
    : principal(std::move(caller)),
      name(std::move(action)),
      arguments(std::move(args)),
      complete(std::move(callback)),
      owner_(owner),
      deadline_(std::chrono::steady_clock::now() + owner.config_.limits.timeout),
      remainingBytes_(std::min<size_t>(owner.config_.limits.resultBytes / 3, 1024 * 1024))
{
    deadline_ = std::min(
        deadline_,
        std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                principal.expiresAt - std::chrono::system_clock::now()));
    remainingWork_ = arguments.value(
        "maxWork",
        name == "mapget_extract_source_data" ? size_t{1000000} : size_t{100000});
    limit_ = arguments.value(
        "limit",
        (name == "mapget_docs" || name == "mapget_query_schema") ? size_t{8} : size_t{100});
    if (name == "mapget_query_schema" && !arguments.contains("query"))
        limit_ = std::min<size_t>(limit_, 32);
    query_ = std::make_unique<Query>(*this);
    if (name == "mapget_extract_features" || name == "mapget_extract_source_data")
        extraction_ = std::make_unique<Extraction>(*this);
}

McpMapgetTools::Call::~Call() = default;

void McpMapgetTools::Call::cancel()
{
    cancelled->store(true);
    if (extraction_)
        extraction_->cancel();
}

bool McpMapgetTools::Call::step(size_t count)
{
    if (cancelled->load() || !principal.valid(std::chrono::system_clock::now()))
        truncate("cancelled_or_authority_expired");
    else if (std::chrono::steady_clock::now() >= deadline_)
        truncate("timeout");
    else if (count > remainingWork_)
        truncate("work_limit");
    if (!incomplete_.empty())
        return false;
    remainingWork_ -= count;
    return true;
}

void McpMapgetTools::Call::truncate(std::string reason)
{
    if (incomplete_.empty())
        incomplete_ = std::move(reason);
}

void McpMapgetTools::Call::fail(std::string code, std::string message)
{
    if (finished_.exchange(true))
        return;
    try {
        complete({{"error", {{"code", std::move(code)}, {"message", std::move(message)}}}});
    }
    catch (...) { /* A closed transport must still relinquish the admission slot. */
    }
    owner_.retire(this);
}

void McpMapgetTools::Call::finish(Json result)
{
    if (finished_)
        return;
    // Text fallback duplicates structuredContent and escapes it once more. Validate the actual
    // serialized representation rather than relying solely on the model walk's allocation budget.
    auto serialized = result.dump();
    if (result.contains("guidance") &&
        serialized.size() + Json(serialized).dump().size() + 1024 >
            owner_.config_.limits.resultBytes)
    {
        // Optional workflow advice must not crowd actual bounded results off the wire.
        result.erase("guidance");
        serialized = result.dump();
    }
    if (serialized.size() + Json(serialized).dump().size() + 1024 >
        owner_.config_.limits.resultBytes) {
        fail(
            "result_too_large",
            "Result exceeds the wire budget; narrow the query. A mutation may already have "
            "applied.");
        return;
    }
    if (!owner_.acceptsResult(name, result)) {
        fail("internal_error", "Native result failed its advertised schema.");
        return;
    }
    if (schemaCatalogRevision_ &&
        *schemaCatalogRevision_ != owner_.service_.sourceCatalogRevision()) {
        fail("schema_unavailable", "The schema catalog changed; retry discovery.");
        return;
    }
    if (layer_ && (name == "mapget_query_schema" || name == "mapget_validate_expression")) {
        // A pinned graph preserves memory safety, not permission after removal/rebinding.
        // Match the binary/inspector endpoint's final current-provider check.
        auto const& schema = layer_->schemaOwner();
        auto current = owner_.service_.getSchema(
            arguments.contains("mapId") ? source_.descriptor.sourceId : std::string{},
            schema->schemaId(),
            principal.datasourceAuthorization);
        if (current != schema) {
            fail("schema_unavailable", "The selected schema binding changed; retry discovery.");
            return;
        }
    }
    if (finished_.exchange(true))
        return;
    try {
        complete({{"result", std::move(result)}});
    }
    catch (...) { /* The caller can disappear independently of the work. */
    }
    owner_.retire(this);
}

void McpMapgetTools::Call::finishItems()
{
    if (extraction_)
        extraction_->reportMissingFeatures();
    Json result{
        {"items", std::move(items_)},
        {"complete", incomplete_.empty()},
        {"reason", incomplete_.empty() ? Json(nullptr) : Json(incomplete_)},
        {"issues", std::move(issues_)},
        {"traces", std::move(traces_)}};
    if (layer_ && (name == "mapget_query_schema" || name == "mapget_validate_expression")) {
        if (auto schema = layer_->schemaOwner())
            result["mapSchemaId"] = schema->schemaId();
    }
    if (name == "mapget_list_sources" && !incomplete_.empty() && result["issues"].size() < 100) {
        result["issues"].push_back(
            {{"phase", "budget"},
             {"message",
              "Partial source inventory. Follow nextOffset when present; otherwise narrow "
              "the source/layer filters or use details:false."}});
    }
    if (name == "mapget_query_schema" && !arguments.contains("query") &&
        !arguments.contains("find")) {
        result["schemaView"] = "shallow_overview";
        result["guidance"] =
            "complete covers this overview only. Open childrenOmitted compounds through "
            "$ref/typeId, or use find to search descendant declarations.";
    }
    if (name == "mapget_query_schema" && arguments.contains("find") && result["items"].empty() &&
        incomplete_.empty() && arguments.value("offset", 0) == 0)
        result["guidance"] =
            "No declared name matched. Try one shorter name fragment; this is not a fuzzy "
            "concept search or evidence of absent map data.";
    if (name == "mapget_query_schema" && incomplete_ == "work_limit" &&
        result["issues"].size() < 100)
        result["issues"].push_back(
            {{"phase", "budget"},
             {"message",
              "Schema discovery exceeded maxWork. Restrict featureType/typeId or increase "
              "maxWork; an incomplete result cannot prove absence."}});
    if (name == "mapget_extract_features" &&
        (incomplete_ == "byte_limit" || incomplete_ == "work_limit") &&
        result["issues"].size() < 100)
    {
        // Fixed repair guidance uses envelope headroom, never an already exhausted model budget.
        result["issues"].push_back(
            {{"phase", "budget"},
             {"message",
              "Retry fewer features/partitions or narrower query/expressions. Omit projections "
              "in feature scope for summaries. limit bounds rows, not one row's size."}});
        auto const wire = result.dump();
        if (wire.size() + Json(wire).dump().size() + 1024 > owner_.config_.limits.resultBytes)
            result["issues"].erase(result["issues"].end() - 1);
    }
    if (name == "mapget_list_sources" || name == "mapget_query_schema") {
        auto const wire = result.dump();
        if (wire.size() + Json(wire).dump().size() + 1024 > owner_.config_.limits.resultBytes &&
            !result["issues"].empty() && result["issues"].back().value("phase", "") == "budget")
            result["issues"].erase(result["issues"].end() - 1);
    }
    // Never advance past a partly projected row. With no complete row, the caller
    // must narrow the projection or raise its work budget instead of looping a cursor.
    if (name == "mapget_extract_features" && completeRows_ &&
        (incomplete_ == "item_limit" || incomplete_ == "byte_limit" || incomplete_ == "work_limit"))
        nextOffset_ = arguments.value("offset", size_t{0}) + completeRows_;
    if (nextOffset_)
        result["nextOffset"] = *nextOffset_;
    if (name == "mapget_extract_features")
        extraction_->continueExtraction(result);
    if (name == "mapget_query_schema" && arguments.contains("find")) {
        result["schemaView"] = "matches";
        result["discovery"] = std::move(discovery_);
        result["narrowing"] = std::move(narrowing_);
    }
    if (name == "mapget_docs")
        result["revision"] = std::move(helpRevision_);
    finish(std::move(result));
}

void McpMapgetTools::Call::documentation()
{
    auto result = owner_.help_.query(
        arguments.value("query", std::string{}),
        arguments.value("title", std::string{}),
        limit_,
        [this] { return step(); },
        arguments.value("component", std::string{}),
        arguments.value("offset", size_t{0}));
    helpRevision_ = std::move(result["revision"]);
    issues_ = std::move(result["issues"]);
    for (auto const& item : result["items"]) {
        if (!step())
            break;
        // Help rows contain only trusted bounded Markdown/title/source strings. Charge their
        // actual JSON size, avoiding per-node model-walk overhead for trusted documents.
        charge(item.dump().size());
        // Include framing and the escaped text fallback before accepting a row. In particular,
        // small configured budgets must return a bounded result, not result_too_large.
        auto candidateItems = items_;
        candidateItems.push_back(item);
        Json candidate{
            {"items", std::move(candidateItems)},
            {"complete", false},
            {"reason", "byte_limit"},
            {"issues", issues_},
            {"traces", traces_},
            {"revision", helpRevision_}};
        auto serialized = candidate.dump();
        if (serialized.size() + Json(serialized).dump().size() + 1024 >
            owner_.config_.limits.resultBytes) {
            truncate("byte_limit");
            break;
        }
        if (!append(item))
            break;
    }
    if (incomplete_.empty() && result.contains("nextOffset"))
        nextOffset_ = result["nextOffset"].get<size_t>();
    // A failed rebuild may still return useful, explicitly identified last-good sections.
    if (!result["complete"].get<bool>())
        truncate(result["reason"]);
}

bool McpMapgetTools::Call::append(Json item)
{
    if (items_.size() >= limit_) {
        truncate("item_limit");
        return false;
    }
    items_.push_back(std::move(item));
    return true;
}

void McpMapgetTools::Call::charge(size_t bytes)
{
    if (bytes > remainingBytes_) {
        truncate("byte_limit");
        throw std::length_error("MCP result budget");
    }
    remainingBytes_ -= bytes;
}

void McpMapgetTools::Call::run(bool admitted)
{
    std::lock_guard lock(executionMutex_);
    try {
        if (!admitted) {
            fail("busy", "Service is stopping or its task queue is full.");
            return;
        }
        if (!step()) {
            finishItems();
            return;
        }
        if (name == "mapget_docs")
            documentation();
        else if (name == "mapget_list_sources")
            listSources();
        else if (name == "mapget_list_schemas")
            listSchemas();
        else if (name == "mapget_get_coverage")
            getCoverage();
        else if (name == "mapget_query_schema")
            query_->querySchema();
        else if (name == "mapget_validate_expression")
            query_->validateExpression();
        else if (name == "mapget_extract_features" || name == "mapget_extract_source_data") {
            extraction_->run();
            return;
        }
        else if (name == "mapget_convert_tile_id")
            append(query_->boundedJson(convertTile()));
        else if (name == "mapget_convert_coordinates")
            append(query_->boundedJson(convertCoordinates()));
        else if (name == "mapget_lookup_place" || name == "mapget_get_place_geometry") {
            if (!owner_.location_) {
                fail("unavailable", "Location database unavailable.");
                return;
            }
            auto const max = std::min<size_t>(limit_, 50);
            std::vector<LocationMatch> matches;
            if (arguments.contains("id")) {
                if (auto match = owner_.location_->find(arguments.at("id").get<std::string>()))
                    matches.push_back(std::move(*match));
            }
            else
                matches = owner_.location_->search(arguments.at("name").get<std::string>(), max);
            for (auto const& match : matches) {
                if (!step())
                    break;
                auto item = match.serialize();
                Json boundary;
                if (auto geometry = item.find("geometry"); geometry != item.end()) {
                    boundary = std::move(*geometry);
                    item.erase(geometry);
                }
                if (!append(query_->boundedJson(item)))
                    break;
                // Keep the resolved identity/extent useful when the optional polygon exceeds
                // budgets. Assignment occurs only after complete conversion: never emit half a
                // ring.
                if (!boundary.is_null())
                    items_.back()["geometry"] = query_->boundedJson(boundary);
            }
            if (!arguments.contains("id") && matches.size() == max)
                truncate("item_limit");
        }
        else if (name == "mapget_get_config") {
            auto config = owner_.readConfig_();
            config["writeAvailable"] = owner_.permitted("mapget_set_config", principal);
            append(query_->boundedJson(config));
        }
        else if (name == "mapget_set_config") {
            // Recheck authority immediately before the irreversible file replacement.
            if (!step()) {
                finishItems();
                return;
            }
            append(query_->boundedJson(
                owner_.writeConfig_(arguments.at("model"), arguments.at("expectedRevision"))));
        }
        else if (name == "mapget_get_diagnostics") {
            auto snapshot = owner_.diagnostics_();
            auto sections = arguments.value(
                "sections",
                Json::array({"workers", "memory", "cache", "transport", "sources"}));
            for (auto const& section : sections) {
                auto key = section == "workers" ?
                    "service" :
                    section == "transport" ?
                    "tilesWebsocket" :
                    section.get<std::string>();
                if (section == "sources") {
                    listSources();
                }
                else if (snapshot.contains(key)) {
                    auto value = Json{
                        {"section", section},
                        {"timestampMs", snapshot.at("timestampMs")},
                        {"value", query_->boundedJson(snapshot.at(key))}};
                    if (section == "transport")
                        value["rest"] = query_->boundedJson(snapshot.at("tilesHttp"));
                    if (!append(std::move(value)))
                        break;
                }
            }
        }
        finishItems();
    }
    catch (std::length_error const&) {
        if (incomplete_.empty())
            truncate("size_limit");
        finishItems();
    }
    catch (std::system_error const& error) {
        if (error.code() == std::errc::state_not_recoverable)
            fail("conflict", "Configuration changed; read its current revision before retrying.");
        else if (error.code() == std::errc::resource_unavailable_try_again)
            fail("schema_unavailable", "The schema catalog changed; retry discovery.");
        else
            fail("execution_failed", "Native operation failed.");
    }
    catch (std::invalid_argument const&) {
        fail(
            "invalid_arguments",
            "Invalid arguments or unavailable/ambiguous authorized map layer. "
            "Use exact mapId/layerId values from mapget_list_sources and sourceId to "
            "disambiguate sources. A feature type is not a layer id.");
    }
    catch (...) {
        fail("execution_failed", "Native operation failed; consult protected server diagnostics.");
    }
}

void McpMapgetTools::Call::listSources()
{
    auto snapshot = owner_.service_.sourceCatalog(principal.datasourceAuthorization, false);
    // Focused discovery includes ID compositions; an explicit compact request still wins.
    auto const details =
        arguments.value("details", arguments.contains("sourceId") || arguments.contains("mapId"));
    auto const compactSourceData = !details && !arguments.contains("layerId");
    auto const offset = arguments.value("offset", size_t{0});
    size_t matched = 0;
    for (auto const& source : snapshot.sources) {
        if (!step())
            break;
        if (arguments.contains("sourceId") && arguments["sourceId"] != source.descriptor.sourceId)
            continue;
        if (arguments.contains("mapId") &&
            (!source.info || arguments["mapId"] != source.info->mapId_))
            continue;
        if (matched++ < offset)
            continue;
        if (items_.size() >= limit_) {
            nextOffset_ = matched - 1;
            truncate("item_limit");
            break;
        }
        // A later source may exceed the remaining byte budget. Resume it with a fresh page;
        // do not advertise a non-progressing cursor when even the first source cannot fit.
        if (!items_.empty())
            nextOffset_ = matched - 1;
        Json value{
            {"sourceId", source.descriptor.sourceId},
            {"revision", std::to_string(snapshot.revision)},
            {"status",
             source.status == DataSourceCatalogStatus::Ready ?
                 "ready" :
                 source.status == DataSourceCatalogStatus::Failed ?
                 "failed" :
                 "initializing"},
            {"progress", source.progress ? Json(*source.progress) : Json(nullptr)},
            {"layers", Json::array()}};
        if (compactSourceData)
            value["sourceDataLayers"] = Json::array();
        // Constructor errors/messages can contain upstream URLs/secrets. Keep those in protected
        // logs.
        if (source.info) {
            value["mapId"] = source.info->mapId_;
            value["addOn"] = source.info->isAddOn_;
            value["mapFormat"] = source.info->mapFormat_.toJson();
            value["schemaId"] = source.info->schemaId_;
            if (details) {
                value["stringPoolId"] = source.info->stringPoolId_;
                value["maxParallelJobs"] = source.info->maxParallelJobs_;
                value["protocolVersion"] = source.info->protocolVersion_.toJson();
            }
        }
        // Charge each piece once as it is attached, instead of first constructing an
        // unbounded source tree or charging nested copies again at every ancestor.
        value = query_->boundedJson(value);
        if (source.info) {
            std::map<std::string, std::shared_ptr<LayerInfo>>
                layers(source.info->layers_.begin(), source.info->layers_.end());
            for (auto const& [id, layer] : layers) {
                if (!step())
                    break;
                if (!layer || (arguments.contains("layerId") && arguments["layerId"] != id))
                    continue;
                // Raw-layer metadata is repetitive and can bury the renderable layers in
                // discovery. Retain every raw ID; a focused layerId call opens its metadata.
                if (compactSourceData && layer->type_ == LayerType::SourceData) {
                    value["sourceDataLayers"].push_back(query_->boundedJson(id));
                    continue;
                }
                Json metadata{
                    {"layerId", id},
                    {"type", layer->type_},
                    {"partitionKind",
                     layer->partitionKind_ == PartitionKind::Tile ? "tile" : "object"},
                    {"zoomLevels", layer->zoomLevels_},
                    {"canRead", layer->canRead_},
                    {"canWrite", layer->canWrite_},
                    {"hasSchema", bool(layer->layerSchema())},
                    {"featureTypes", Json::array()},
                    {"coverageRangeCount", layer->coverage_.size()}};
                auto schema = layer->layerSchema();
                if (details)
                    metadata["version"] = layer->version_.toJson();
                if (layer->tileAssociationLevel_)
                    metadata["tileAssociationLevel"] = *layer->tileAssociationLevel_;
                metadata = query_->boundedJson(metadata);
                for (auto const& name : schema->featureTypes()) {
                    if (!step())
                        break;
                    auto type = schema->getTypeInfo(name);
                    auto info = query_->boundedJson({{"name", type.name()}, {"produced", true}});
                    if (details)
                        info["uniqueIdCompositions"] = Json::array();
                    if (details)
                        for (auto composition : type.compositions()) {
                            if (!step())
                                break;
                            charge(24);
                            auto parts = Json::array();
                            for (auto part : composition.parts()) {
                                if (!step())
                                    break;
                                parts.push_back(query_->boundedJson(
                                    {{"partId", part.name()},
                                     {"description", part.description()},
                                     {"datatype", part.datatype()},
                                     {"isSynthetic", part.synthetic()},
                                     {"isOptional", part.optional()}}));
                            }
                            info["uniqueIdCompositions"].push_back(std::move(parts));
                        }
                    metadata["featureTypes"].push_back(std::move(info));
                }
                value["layers"].push_back(std::move(metadata));
            }
        }
        if (!step(0) || !append(std::move(value)))
            break;
        nextOffset_.reset();
    }
}

void McpMapgetTools::Call::getCoverage()
{
    selectLayer();
    Json result{
        {"sourceId", source_.descriptor.sourceId},
        {"mapId", source_.info->mapId_},
        {"layerId", layer_->layerId_},
        {"partitionKind", layer_->partitionKind_ == PartitionKind::Tile ? "tile" : "object"},
        {"coverageKnown", !layer_->coverage_.empty()},
        {"ranges", Json::array()}};
    if (layer_->tileAssociationLevel_)
        result["tileAssociationLevel"] = *layer_->tileAssociationLevel_;
    // Preserve completed ranges when a later bitmap exceeds the response budget.
    result = query_->boundedJson(result);
    try {
        for (auto const& coverage : layer_->coverage_) {
            if (!step())
                break;
            if (arguments.contains("level") && arguments["level"] != coverage.min_.level())
                continue;
            if (result["ranges"].size() >= limit_) {
                truncate("item_limit");
                break;
            }
            Json range{
                {"min", coverage.min_.value()},
                {"max", coverage.max_.value()},
                {"level", coverage.min_.level()}};
            if (arguments.value("format", std::string{"summary"}) == "raw") {
                range["filled"] = Json::array();
                range = query_->boundedJson(range);
                // A raw mask is all-or-nothing: omitting it would imply a full rectangle.
                for (bool filled : coverage.filled_)
                    range["filled"].push_back(query_->boundedJson(filled));
            }
            else {
                auto const width = uint64_t(coverage.max_.x()) - coverage.min_.x() + 1;
                auto const height = uint64_t(coverage.max_.y()) - coverage.min_.y() + 1;
                auto const sw = coverage.min_.southWestWgs84();
                auto const ne = coverage.max_.northEastWgs84();
                uint64_t count = coverage.filled_.empty() ? width * height : 0;
                auto samples = Json::array();
                auto sample = [&](uint64_t index)
                {
                    if (samples.size() < 8)
                        samples.push_back(
                            TileId::fromTileXY(
                                coverage.min_.x() + index % width,
                                coverage.min_.y() + index / width,
                                coverage.min_.level())
                                .value());
                };
                if (coverage.filled_.empty()) {
                    for (uint64_t i = 0; i < std::min(uint64_t{8}, count); ++i)
                        sample(i);
                }
                else {
                    for (size_t i = 0; i < coverage.filled_.size(); ++i) {
                        if (!step())
                            throw std::length_error("work");
                        if (coverage.filled_[i]) {
                            ++count;
                            sample(i);
                        }
                    }
                }
                range["coverageShape"] = coverage.filled_.empty() ? "rectangle" : "sparse";
                auto west = sw.first, east = ne.first;
                auto south = sw.second, north = ne.second;
                auto const columns = uint64_t{1} << (coverage.min_.level() + 1);
                auto const rows = uint64_t{1} << coverage.min_.level();
                // Equal longitude endpoints otherwise disguise a whole-world range.
                if (width == columns) {
                    west = -180;
                    east = 180;
                }
                // Unsigned NDS grid rows can cross the north/south pole discontinuity.
                // Their enclosing WGS84 latitude interval then spans both hemispheres.
                if (rows == 1 || (coverage.min_.y() < rows / 2 && coverage.max_.y() >= rows / 2)) {
                    south = -90;
                    north = 90;
                }
                range["bounds"] = {west, south, east, north};
                range["coveredTileCount"] = count;
                range["sampleTileIds"] = std::move(samples);
                range = query_->boundedJson(range);
            }
            result["ranges"].push_back(std::move(range));
        }
    }
    catch (std::length_error const&) { /* Retain only fully serialized coverage records. */
    }
    append(std::move(result));
}

void McpMapgetTools::Call::selectLayer()
{
    auto const map = arguments.at("mapId").get<std::string>();
    auto const layer = arguments.at("layerId").get<std::string>();
    size_t matches = 0;
    for (auto const& candidate :
         owner_.service_.sourceCatalog(principal.datasourceAuthorization, false).sources)
    {
        if (!step())
            throw std::length_error("work");
        if (!candidate.info || candidate.info->isAddOn_ || candidate.info->mapId_ != map ||
            (arguments.contains("sourceId") &&
             arguments["sourceId"] != candidate.descriptor.sourceId))
            continue;
        auto found = candidate.info->getLayer(layer, false);
        if (!found || !found->canRead_)
            continue;
        source_ = candidate;
        layer_ = std::move(found);
        ++matches;
    }
    if (matches != 1)
        throw std::invalid_argument("Layer unavailable or ambiguous");
}

void McpMapgetTools::Call::listSchemas()
{
    auto format = arguments.contains("mapFormat") ?
        std::optional{MapFormat::fromJson(arguments["mapFormat"])} :
        std::nullopt;
    schemaCatalogRevision_ = owner_.service_.sourceCatalogRevision();
    auto inventory = owner_.service_.schemaInventory(
        principal.datasourceAuthorization,
        [&](MapSchema const& owner)
        {
            return (!format || owner.format() == *format) &&
                (!arguments.contains("mapSchemaId") ||
                 arguments["mapSchemaId"].get_ref<std::string const&>() == owner.schemaId());
        },
        [&]
        {
            if (!step())
                throw std::length_error("work");
        },
        [&](std::string_view sourceId, std::string_view mapId)
        {
            return (!arguments.contains("mapId") ||
                    arguments["mapId"].get_ref<std::string const&>() == mapId) &&
                (!arguments.contains("sourceId") ||
                 arguments["sourceId"].get_ref<std::string const&>() == sourceId);
        });
    for (auto item : inventory) {
        if (!step())
            break;
        if (!append(query_->boundedJson(item)))
            break;
    }
}

bool McpMapgetTools::Call::selectSchemaLayer()
{
    if (arguments.contains("mapId")) {
        selectLayer();
        if (arguments.contains("mapSchemaId") &&
            (!layer_->schemaOwner() ||
             arguments["mapSchemaId"].get_ref<std::string const&>() !=
                 layer_->schemaOwner()->schemaId()))
        {
            fail("schema_unavailable", "The map's current schema does not match mapSchemaId.");
            return false;
        }
        return true;
    }
    auto format = arguments.contains("mapFormat") ?
        std::optional{MapFormat::fromJson(arguments["mapFormat"])} :
        std::nullopt;
    auto const layerName = arguments.at("layerId").get<std::string>();
    MapSchema::ConstPtr owner;
    if (arguments.contains("mapSchemaId")) {
        // An exact reference needs no inventory expansion, even on a very large catalog.
        owner = owner_.service_.getSchema(
            {},
            arguments["mapSchemaId"].get<std::string>(),
            principal.datasourceAuthorization);
        if (owner && format && owner->format() != *format)
            owner.reset();
    }
    else {
        schemaCatalogRevision_ = owner_.service_.sourceCatalogRevision();
        auto choices = owner_.service_.schemaInventory(
            principal.datasourceAuthorization,
            [&](MapSchema const& candidate)
            { return candidate.format() == *format && candidate.layer(layerName); },
            [&]
            {
                if (!step())
                    throw std::length_error("work");
            });
        if (choices.size() > 1) {
            for (auto const& item : choices) {
                if (!append(query_->boundedJson(
                        {{"mapSchemaId", item["mapSchemaId"]},
                         {"mapFormat", item["mapFormat"]},
                         {"consumers", item["consumers"]},
                         {"selectArguments",
                          {{"mapSchemaId", item["mapSchemaId"]}, {"layerId", layerName}}}})))
                    break;
            }
            // Serialize useful narrowing choices while work is still admitted.
            // Marking the result incomplete earlier stops boundedJson's step checks.
            truncate("ambiguous_schema");
            finishItems();
            return false;
        }
        if (!choices.empty())
            owner = owner_.service_.getSchema(
                {},
                choices[0]["mapSchemaId"].get<std::string>(),
                principal.datasourceAuthorization);
    }
    auto schema = owner ? owner->layer(layerName) : simfil::model_ptr<LayerSchema>{};
    if (!schema) {
        fail("schema_unavailable", "The selected artifact/layer is unavailable.");
        return false;
    }
    // This is metadata-only query context, not a synthetic datasource registration.
    // Reuse the same binding validation as normal metadata and retain the immutable owner.
    DataSourceInfo metadata;
    auto layer = std::make_shared<LayerInfo>();
    layer->layerId_ = layerName;
    layer->type_ = schema->layerType();
    metadata.layers_.emplace(layerName, std::move(layer));
    metadata.setMapSchema(std::move(owner));
    layer_ = metadata.getLayer(layerName);
    return true;
}

Json McpMapgetTools::Call::convertCoordinates()
{
    double x = arguments.at("x"), y = arguments.at("y");
    if (!std::isfinite(x) || !std::isfinite(y))
        throw std::invalid_argument("finite coordinates");
    if (arguments["from"] == "wgs84") {
        if (x < -180 || x > 180 || y < -90 || y > 90)
            throw std::invalid_argument("range");
        int32_t nx, ny;
        ndsmath::HighPrecWgs84(x, y).toNdsCoordinates(nx, ny);
        return {{"system", "nds"}, {"x", nx}, {"y", ny}, {"axisOrder", "longitude,latitude"}};
    }
    if (x < INT32_MIN || x > INT32_MAX || y < -(1LL << 30) || y >= (1LL << 30) ||
        std::floor(x) != x || std::floor(y) != y)
        throw std::invalid_argument("NDS coordinates");
    auto point = ndsmath::HighPrecWgs84::fromNdsCoordinates(
        static_cast<int32_t>(x),
        static_cast<int32_t>(y));
    return {
        {"system", "wgs84"},
        {"longitude", point.longitude()},
        {"latitude", point.latitude()},
        {"unit", "degrees"}};
}

Json McpMapgetTools::Call::convertTile()
{
    auto const modes = arguments.contains("tileId") + arguments.contains("legacyTileId") +
        arguments.contains("x") + arguments.contains("longitude");
    if (modes != 1)
        throw std::invalid_argument("Choose one tile representation");
    std::set<std::string> fields = arguments.contains("tileId") ?
        std::set<std::string>{"tileId"} :
        arguments.contains("legacyTileId") ?
        std::set<std::string>{"legacyTileId"} :
        arguments.contains("x") ?
        std::set<std::string>{"x", "y", "level"} :
        std::set<std::string>{"longitude", "latitude", "level"};
    for (auto const& field : fields)
        if (!arguments.contains(field))
            throw std::invalid_argument("Incomplete tile representation");
    for (auto const& [field, _] : arguments.items())
        if (!fields.contains(field) && field != "limit" && field != "maxWork")
            throw std::invalid_argument("Mixed tile representations");
    TileId tile;
    if (arguments.contains("tileId"))
        tile = TileId::fromValue(arguments.at("tileId").get<int32_t>());
    else if (arguments.contains("legacyTileId")) {
        auto text = arguments.at("legacyTileId").get<std::string>();
        int64_t id = 0;
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
        if (error != std::errc{} || end != text.data() + text.size())
            throw std::invalid_argument("legacy ID");
        if (!isLegacyTileId(id)) {
            fail(
                "invalid_arguments",
                "legacyTileId is only for the removed mapget 0xXXXXYYYYZZZZ layout. For a signed "
                "NDS packed ID use tileId as an integer, including Classic maps.");
            return Json::object();
        }
        tile = legacyTileIdToPacked(id);
    }
    else if (arguments.contains("x")) {
        tile = TileId::fromTileXY(arguments.at("x"), arguments.at("y"), arguments.at("level"));
    }
    else {
        tile = TileId::fromWgs84(
            arguments.at("longitude"),
            arguments.at("latitude"),
            arguments.at("level"));
    }
    auto sw = tile.southWestWgs84(), ne = tile.northEastWgs84(), center = tile.centerWgs84();
    return {
        {"tileId", tile.value()},
        {"level", tile.level()},
        {"x", tile.x()},
        {"y", tile.y()},
        {"bounds", {sw.first, sw.second, ne.first, ne.second}},
        {"center", {center.first, center.second}}};
}

}  // namespace mapget::detail
