#include "mcp-action-catalog.h"
#include "mcp-native-call.h"

#include "cli.h"
#include "mapget/model/layerschema.h"
#include "ndsmath/wgs84.h"

#include <openssl/rand.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

namespace mapget::detail
{
using Json = nlohmann::json;

McpNativeTools::McpNativeTools(
    Service& service,
    McpConfig config,
    std::function<Json()> diagnostics,
    LocationLookup const* location,
    std::function<Json()> readConfig,
    std::function<Json(Json const&, std::string const&)> writeConfig)
    : service_(service),
      config_(std::move(config)),
      help_(
          McpHelp::defaultDirectory(),
          [this]
          {
              auto paths = config_.helpDocs;
              if (!config_.webHelpDirectory.empty())
                  paths.push_back(config_.webHelpDirectory);
              return paths;
          }()),
      diagnostics_(std::move(diagnostics)),
      location_(location),
      readConfig_(std::move(readConfig)),
      writeConfig_(std::move(writeConfig))
{
    buildCatalog();
}

McpNativeTools::~McpNativeTools()
{
    stop();
}

std::string McpNativeTools::retain(std::shared_ptr<Continuation> checkpoint)
{
    // Count pinned tile/query storage conservatively for every checkpoint. Bounds apply to
    // retained allocations, not total process RSS; active calls have separate admission limits.
    constexpr size_t maxBytes = 64 * 1024 * 1024;
    if (checkpoint->retainedBytes > maxBytes)
        return {};
    unsigned char random[24];
    if (RAND_bytes(random, sizeof(random)) != 1)
        return {};
    std::string token;
    for (auto byte : random) {
        token += "0123456789abcdef"[byte >> 4];
        token += "0123456789abcdef"[byte & 15];
    }
    auto revision = service_.sourceCatalogRevision();
    std::vector<std::shared_ptr<Continuation>> retired;
    std::lock_guard lock(mutex_);
    if (stopped_)
        return {};
    auto now = std::chrono::steady_clock::now();
    for (auto it = continuations_.begin(); it != continuations_.end();) {
        if (it->second->expires <= now || it->second->catalogRevision != revision) {
            retired.push_back(std::move(it->second));
            it = continuations_.erase(it);
        }
        else
            ++it;
    }
    for (;;) {
        size_t bytes = checkpoint->retainedBytes, owned = 0;
        for (auto const& [_, entry] : continuations_) {
            bytes += entry->retainedBytes;
            owned += entry->principal.sameUser(checkpoint->principal);
        }
        if (bytes <= maxBytes && owned < 4 && continuations_.size() < 16)
            break;
        auto oldest = continuations_.end();
        for (auto it = continuations_.begin(); it != continuations_.end(); ++it) {
            if (owned >= 4 && !it->second->principal.sameUser(checkpoint->principal))
                continue;
            if (oldest == continuations_.end() || it->second->expires < oldest->second->expires)
                oldest = it;
        }
        if (oldest == continuations_.end())
            return {};
        retired.push_back(std::move(oldest->second));
        continuations_.erase(oldest);
    }
    continuations_.emplace(token, std::move(checkpoint));
    return token;
}

bool McpNativeTools::permitted(std::string_view name, Principal const& principal) const
{
    if (!principal.valid(std::chrono::system_clock::now()))
        return false;
    if (name == "mapget_get_config")
        return config_.configReadEnabled && principal.configRead && isGetConfigEndpointEnabled();
    if (name == "mapget_set_config")
        return config_.configWriteEnabled && config_.directConfigPersistence &&
            principal.configWrite && isPostConfigEndpointEnabled();
    if (name == "mapget_get_diagnostics")
        return principal.diagnostics;
    return principal.read;
}

Json McpNativeTools::tools(Principal const& principal) const
{
    auto result = Json::array();
    for (auto const& [name, action] : actions_)
        if (permitted(name, principal))
            result.push_back(action.tool);
    return result;
}

bool McpNativeTools::contains(std::string_view name) const
{
    return actions_.contains(name);
}

bool McpNativeTools::acceptsArguments(std::string_view name, Json const& value, Json* issues) const
{
    try {
        auto const& action = actions_.at(std::string(name));
        return McpActionCatalog::validateArguments(
            action.input,
            action.tool.at("inputSchema"),
            value,
            issues);
    }
    catch (...) {
        return false;
    }
}

bool McpNativeTools::acceptsResult(std::string_view name, Json const& value) const
{
    try {
        actions_.at(std::string(name)).output.validate(value);
        return true;
    }
    catch (...) {
        return false;
    }
}

std::shared_ptr<std::atomic_bool>
McpNativeTools::invoke(Principal principal, std::string name, Json arguments, Complete complete)
{
    auto reject = [&](std::string code, std::string message)
    {
        complete({{"error", {{"code", code}, {"message", message}}}});
        return std::shared_ptr<std::atomic_bool>{};
    };
    if (!contains(name) || !permitted(name, principal))
        return reject(
            "not_available",
            "Native tool permission or deployment capability unavailable.");
    if (name == "mapget_extract_features" && arguments.is_object() &&
        !arguments.contains("cursor") && !arguments.contains("partition") &&
        !arguments.contains("partitions") && !arguments.contains("featureIds"))
        return reject(
            "invalid_arguments",
            "Supply partition, nonempty partitions, or full primary/secondary featureIds. "
            "Extraction does not "
            "scan a map; use viewer_start_search for a viewport search.");
    if (!acceptsArguments(name, arguments))
        return reject("invalid_arguments", "Arguments do not match the tool schema.");
    std::unique_lock lock(mutex_);
    auto owned = std::count_if(
        calls_.begin(),
        calls_.end(),
        [&](auto const& c) { return c->principal.sameUser(principal); });
    if (stopped_ || calls_.size() >= config_.limits.pendingCalls ||
        static_cast<size_t>(owned) >= config_.limits.callsPerPrincipal)
    {
        lock.unlock();
        return reject("busy", "Native tool admission limit reached.");
    }
    auto call = std::make_shared<Call>(
        *this,
        std::move(principal),
        std::move(name),
        std::move(arguments),
        std::move(complete));
    calls_.push_back(call);
    lock.unlock();
    if (!service_.scheduleTask([call](bool admitted) { call->run(admitted); }))
        call->run(false);
    return call->cancelled;
}

void McpNativeTools::retire(Call* call)
{
    std::lock_guard lock(mutex_);
    std::erase_if(calls_, [call](auto const& c) { return c.get() == call; });
    idle_.notify_all();
}

void McpNativeTools::cancel(std::shared_ptr<std::atomic_bool> const& token)
{
    std::shared_ptr<Call> call;
    {
        std::lock_guard lock(mutex_);
        auto found = std::find_if(
            calls_.begin(),
            calls_.end(),
            [&](auto const& c) { return c->cancelled == token; });
        if (found != calls_.end())
            call = *found;
    }
    if (call)
        call->cancel();
}

void McpNativeTools::stop()
{
    std::unique_lock lock(mutex_);
    stopped_ = true;
    auto continuations = std::move(continuations_);
    auto calls = calls_;
    lock.unlock();
    continuations.clear();
    for (auto const& call : calls)
        call->cancel();
    lock.lock();
    idle_.wait(lock, [this] { return calls_.empty() && !helpRefreshPending_; });
}

void McpNativeTools::refreshHelp()
{
    std::unique_lock lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    if (stopped_ || helpRefreshPending_ || now < nextHelpRefresh_)
        return;
    nextHelpRefresh_ = now + std::chrono::seconds(2);
    helpRefreshPending_ = true;
    // stop() drains this owner before the borrowed service or index can disappear.
    lock.unlock();
    if (service_.scheduleTask(
            [this](bool admitted)
            {
                try {
                    if (admitted) {
                        // Tile destruction runs on a worker and outside the admission mutex.
                        auto revision = service_.sourceCatalogRevision();
                        std::vector<std::shared_ptr<Continuation>> retired;
                        {
                            std::lock_guard cleanup(mutex_);
                            auto now = std::chrono::steady_clock::now();
                            for (auto it = continuations_.begin(); it != continuations_.end();) {
                                if (it->second->expires <= now ||
                                    it->second->catalogRevision != revision) {
                                    retired.push_back(std::move(it->second));
                                    it = continuations_.erase(it);
                                }
                                else
                                    ++it;
                            }
                        }
                        retired.clear();
                        help_.refresh();
                    }
                }
                catch (...) { /* Failed maintenance must still release the shutdown barrier. */
                }
                std::lock_guard done(mutex_);
                helpRefreshPending_ = false;
                idle_.notify_all();
            }))
        return;
    lock.lock();
    helpRefreshPending_ = false;
    idle_.notify_all();
}

McpNativeTools::Call::
    Call(McpNativeTools& owner, Principal caller, std::string action, Json args, Complete callback)
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
}

void McpNativeTools::Call::cancel()
{
    cancelled->store(true);
    // Only atomics cross the control/worker boundary. Child callbacks also run during abort;
    // they observe cancellation and never race a partially materialized result.
    LayerTilesRequest::Ptr request;
    {
        std::lock_guard lock(requestMutex_);
        request = request_;
    }
    if (request) {
        auto* service = &owner_.service_;
        // abort may synchronously complete a request; never enter its callback mutex on an I/O
        // thread.
        (void)service->scheduleTask([service, request](bool) { service->abort(request); });
    }
}

bool McpNativeTools::Call::step(size_t count)
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

void McpNativeTools::Call::truncate(std::string reason)
{
    if (incomplete_.empty())
        incomplete_ = std::move(reason);
}

void McpNativeTools::Call::fail(std::string code, std::string message)
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

void McpNativeTools::Call::finish(Json result)
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
    if (finished_.exchange(true))
        return;
    try {
        complete({{"result", std::move(result)}});
    }
    catch (...) { /* The caller can disappear independently of the work. */
    }
    owner_.retire(this);
}

void McpNativeTools::Call::finishItems()
{
    // A fully evaluated lookup with no matching identity is distinct from an empty query result.
    if (name == "mapget_extract_features" && incomplete_.empty()) {
        for (auto const& id : scan_->featureIds) {
            if (scan_->resolvedFeatureIds.contains(id))
                continue;
            if (issues_.size() >= 100) {
                truncate("diagnostic_limit");
                break;
            }
            try {
                issues_.push_back(boundedJson(
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
    Json result{
        {"items", std::move(items_)},
        {"complete", incomplete_.empty()},
        {"reason", incomplete_.empty() ? Json(nullptr) : Json(incomplete_)},
        {"issues", std::move(issues_)},
        {"traces", std::move(traces_)}};
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
            "$ref/schemaId, or use find to search descendant declarations.";
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
              "Schema discovery exceeded maxWork. Restrict featureType/schemaId or increase "
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
        continueExtraction(result);
    if (name == "mapget_query_schema" && arguments.contains("find")) {
        result["schemaView"] = "matches";
        result["discovery"] = std::move(discovery_);
        result["narrowing"] = std::move(narrowing_);
    }
    if (name == "mapget_docs")
        result["revision"] = std::move(helpRevision_);
    finish(std::move(result));
}

void McpNativeTools::Call::documentation()
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

bool McpNativeTools::Call::append(Json item)
{
    if (items_.size() >= limit_) {
        truncate("item_limit");
        return false;
    }
    items_.push_back(std::move(item));
    return true;
}

void McpNativeTools::Call::charge(size_t bytes)
{
    if (bytes > remainingBytes_) {
        truncate("byte_limit");
        throw std::length_error("MCP result budget");
    }
    remainingBytes_ -= bytes;
}

void McpNativeTools::Call::run(bool admitted)
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
        else if (name == "mapget_get_coverage")
            getCoverage();
        else if (name == "mapget_query_schema")
            querySchema();
        else if (name == "mapget_validate_expression")
            validateExpression();
        else if (name == "mapget_extract_features" || name == "mapget_extract_source_data") {
            if (arguments.contains("cursor")) {
                if (!resumeExtraction())
                    return;
            }
            else {
                scan_->catalogRevision = owner_.service_.sourceCatalogRevision();
                scan_->offsetRemaining = arguments.value("offset", size_t{0});
                prepareExtraction();
            }
            if (!finished_)
                loadNext();
            return;
        }
        else if (name == "mapget_convert_tile_id")
            append(boundedJson(convertTile()));
        else if (name == "mapget_convert_coordinates")
            append(boundedJson(convertCoordinates()));
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
                if (!append(boundedJson(item)))
                    break;
                // Keep the resolved identity/extent useful when the optional polygon exceeds
                // budgets. Assignment occurs only after complete conversion: never emit half a
                // ring.
                if (!boundary.is_null())
                    items_.back()["geometry"] = boundedJson(boundary);
            }
            if (!arguments.contains("id") && matches.size() == max)
                truncate("item_limit");
        }
        else if (name == "mapget_get_config") {
            auto config = owner_.readConfig_();
            config["writeAvailable"] = owner_.permitted("mapget_set_config", principal);
            append(boundedJson(config));
        }
        else if (name == "mapget_set_config") {
            // Recheck authority immediately before the irreversible file replacement.
            if (!step()) {
                finishItems();
                return;
            }
            append(boundedJson(
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
                        {"value", boundedJson(snapshot.at(key))}};
                    if (section == "transport")
                        value["rest"] = boundedJson(snapshot.at("tilesHttp"));
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

void McpNativeTools::Call::listSources()
{
    auto snapshot = owner_.service_.sourceCatalog(principal.datasourceHeaders, false);
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
            if (details) {
                value["stringPoolId"] = source.info->stringPoolId_;
                value["maxParallelJobs"] = source.info->maxParallelJobs_;
                value["protocolVersion"] = source.info->protocolVersion_.toJson();
            }
        }
        // Charge each piece once as it is attached, instead of first constructing an
        // unbounded source tree or charging nested copies again at every ancestor.
        value = boundedJson(value);
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
                    value["sourceDataLayers"].push_back(boundedJson(id));
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
                    {"hasFeatureModelSchema", bool(layer->featureModelSchema_)},
                    {"featureTypes", Json::array()},
                    {"coverageRangeCount", layer->coverage_.size()}};
                if (layer->featureModelSchema_)
                    metadata["schemaFeatureTypes"] = layer->featureModelSchema_->featureTypes();
                if (details)
                    metadata["version"] = layer->version_.toJson();
                if (layer->tileAssociationLevel_)
                    metadata["tileAssociationLevel"] = *layer->tileAssociationLevel_;
                metadata = boundedJson(metadata);
                for (auto const& type : layer->featureTypes_) {
                    if (!step())
                        break;
                    auto info = boundedJson({{"name", type.name_}});
                    if (details)
                        info["uniqueIdCompositions"] = Json::array();
                    if (details)
                        for (auto const& composition : type.uniqueIdCompositions_) {
                            if (!step())
                                break;
                            charge(24);
                            auto parts = Json::array();
                            for (auto const& part : composition) {
                                if (!step())
                                    break;
                                parts.push_back(boundedJson(part.toJson()));
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

void McpNativeTools::Call::getCoverage()
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
    result = boundedJson(result);
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
                range = boundedJson(range);
                // A raw mask is all-or-nothing: omitting it would imply a full rectangle.
                for (bool filled : coverage.filled_)
                    range["filled"].push_back(boundedJson(filled));
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
                range = boundedJson(range);
            }
            result["ranges"].push_back(std::move(range));
        }
    }
    catch (std::length_error const&) { /* Retain only fully serialized coverage records. */
    }
    append(std::move(result));
}

void McpNativeTools::Call::selectLayer()
{
    auto const map = arguments.at("mapId").get<std::string>();
    auto const layer = arguments.at("layerId").get<std::string>();
    size_t matches = 0;
    for (auto const& candidate :
         owner_.service_.sourceCatalog(principal.datasourceHeaders, false).sources)
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

Json McpNativeTools::Call::convertCoordinates()
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

Json McpNativeTools::Call::convertTile()
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
