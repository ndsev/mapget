#include "http-service-impl.h"
#include "mapget/model/mapschema.h"

#include <algorithm>
#include <array>
#include <charconv>

namespace mapget
{
namespace
{

/** Do not leak token existence or backend exception details through schema failures. */
drogon::HttpResponsePtr schemaError(drogon::HttpStatusCode status, std::string message)
{
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(status);
    response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
    response->addHeader("Cache-Control", "no-store");
    response->setBody(std::move(message));
    return response;
}

} // namespace

void HttpService::Impl::handleSchemaRequest(
    drogon::HttpRequestPtr const& req,
    std::function<void(drogon::HttpResponsePtr const&)>&& callback) const
{
    auto sourceId = req->getParameter("sourceId");
    auto schemaId = req->getParameter("schemaId");
    if (sourceId.empty() || schemaId.empty() || sourceId.size() > 4096 || schemaId.size() > 4096) {
        callback(schemaError(drogon::k400BadRequest, "sourceId and schemaId are required (at most 4096 bytes each)."));
        return;
    }

    // The service outlives its workers. The HTTP Impl does not; capture the cache separately.
    auto* service = static_cast<Service*>(&self_);
    auto task = [service, cache = schemaResponses_, sourceId = std::move(sourceId),
                 schemaId = std::move(schemaId), headers = config_.proxyAuth.authorization(req),
                 validator = req->getHeader("if-none-match"), callback](bool admitted) {
        if (!admitted) {
            callback(schemaError(drogon::k503ServiceUnavailable, "Schema service is stopping."));
            return;
        }
        drogon::HttpResponsePtr response;
        try {
            auto schema = service->getSchema(sourceId, schemaId, headers);
            if (!schema)
                response = schemaError(drogon::k404NotFound, "Schema binding is unavailable.");
            else {
                response = cache->response(schema, validator);
                // Removal/rebinding during encoding must not publish the old artifact or a 304.
                if (service->getSchema(sourceId, schemaId, headers) != schema)
                    response = schemaError(drogon::k404NotFound, "Schema binding is unavailable.");
            }
        }
        catch (...) {
            response = schemaError(drogon::k500InternalServerError, "Schema serialization failed.");
        }
        callback(response);
    };
    if (!service->scheduleTask(std::move(task)))
        callback(schemaError(drogon::k503ServiceUnavailable, "Schema request queue is full or stopping."));
}

void HttpService::Impl::handleSchemaInspectionRequest(
    drogon::HttpRequestPtr const& req,
    std::function<void(drogon::HttpResponsePtr const&)>&& callback) const
{
    auto inventory = req->path() == "/schemas";
    auto artifact = req->getParameter("mapSchemaId");
    auto typeIdParameter = req->getParameter("typeId");
    auto search = req->parameters().contains("find");
    auto text = req->getParameter("find");
    auto layerName = req->getParameter("layerId");
    if (search &&
        (inventory || text.empty() || text.size() > 128 || layerName.empty() ||
         layerName.size() > 4096))
    {
        callback(schemaError(
            drogon::k400BadRequest,
            "Schema search requires layerId (1..4096 bytes) and find (1..128 bytes) on "
            "/schema/inspect."));
        return;
    }
    uint32_t id = 0;
    if (!inventory) {
        auto parsed = std::from_chars(
            typeIdParameter.data(),
            typeIdParameter.data() + typeIdParameter.size(),
            id);
        if (artifact.empty() || artifact.size() > 256 || parsed.ec != std::errc{} ||
            parsed.ptr != typeIdParameter.data() + typeIdParameter.size() || id == 0 ||
            id > simfil::MaxTypeId)
        {
            callback(schemaError(
                drogon::k400BadRequest,
                "Exact mapSchemaId and positive numeric typeId are required."));
            return;
        }
    }
    auto* service = static_cast<Service*>(&self_);
    auto task =
        [service,
         headers = config_.proxyAuth.authorization(req),
         inventory,
         search,
         text = std::move(text),
         layerName = std::move(layerName),
         artifact = std::move(artifact),
         id,
         callback](bool admitted)
    {
        if (!admitted) {
            callback(schemaError(drogon::k503ServiceUnavailable, "Schema service is stopping."));
            return;
        }
        drogon::HttpResponsePtr response;
        try {
            auto const revision = service->sourceCatalogRevision();
            MapSchema::ConstPtr owner;
            size_t remaining = 16384;
            size_t remainingBytes = 1024 * 1024;
            auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            auto checkpoint = [&]
            {
                if (remaining == 0 || std::chrono::steady_clock::now() >= deadline)
                    throw std::length_error("Schema inspection exceeded its work budget.");
                --remaining;
            };
            auto reserve = [&](size_t bytes)
            {
                checkpoint();
                if (bytes > remainingBytes)
                    throw std::length_error("Schema inspection exceeded its byte budget.");
                remainingBytes -= bytes;
            };
            auto chargeString = [&](std::string_view value)
            {
                if (value.size() > remainingBytes / 6)
                    throw std::length_error("Schema text exceeds the inspection budget.");
                reserve(
                    2 + value.size() * 6);  // Quotes and worst-case JSON escaping, before copying.
            };
            auto copyName = [&](std::string_view value)
            {
                chargeString(value);
                return std::string(value);
            };
            auto body = nlohmann::json::object();
            if (inventory) {
                body["schemas"] = service->schemaInventory(headers, {}, checkpoint);
            }
            else if (search) {
                owner = service->getSchema({}, artifact, headers);
                auto layer = owner ? owner->layer(layerName) : LayerSchema::Ptr{};
                if (!layer || !owner->schemaType(static_cast<simfil::TypeId>(id))) {
                    callback(
                        schemaError(drogon::k404NotFound, "Schema search root is unavailable."));
                    return;
                }
                chargeString(artifact);
                chargeString(layerName);
                std::array roots{static_cast<simfil::TypeId>(id)};
                // Match MCP's normal scan allowance while bounding browser output and latency.
                body = layer->find(
                    text,
                    roots,
                    {},
                    {.maxResults = 32, .maxWork = 100000, .deadline = deadline},
                    remainingBytes);
                body.erase("rootMatches");
                body["mapSchemaId"] = artifact;
                body["layerId"] = layerName;
                body["typeId"] = id;
            }
            else {
                owner = service->getSchema({}, artifact, headers);
                auto root = owner ?
                    owner->typeNode(static_cast<simfil::TypeId>(id)) :
                    simfil::ModelNode::Ptr{};
                if (!root) {
                    callback(schemaError(drogon::k404NotFound, "Schema type is unavailable."));
                    return;
                }
                // Traverse metadata, not data edges. The very same canonical schema descriptors
                // used by MCP supply explicit references for every child/shared/cyclic type.
                body = {
                    {"mapSchemaId", artifact},
                    {"typeId", id},
                    {"descriptor", nlohmann::json::object()}};
                // Identity/relation contracts belong to mapget's layer, not generic simfil
                // types. Discovery and contract allocation share the descriptor budget.
                for (auto const& name : owner->layerNames()) {
                    checkpoint();
                    auto layer = owner->layer(name);
                    for (auto const& type : layer->featureTypes()) {
                        checkpoint();
                        if (layer->featureSchema(type) != id)
                            continue;
                        auto contract = layer->featureTypeContract(type, reserve);
                        contract["layerId"] = copyName(name);
                        body["featureTypes"].push_back(std::move(contract));
                    }
                }
                std::function<nlohmann::json(simfil::ModelNode const&, size_t)> copyMetadata;
                copyMetadata = [&](simfil::ModelNode const& node, size_t depth) -> nlohmann::json
                {
                    if (depth > 64)
                        throw std::length_error("Schema type exceeds the inspection budget.");
                    reserve(64);
                    if (node.type() == simfil::ValueType::String) {
                        auto value = simfil::Value::field(node);
                        chargeString(value.stringView());
                    }
                    if (node.type() != simfil::ValueType::Object &&
                        node.type() != simfil::ValueType::Array) {
                        auto value = node.toJson();
                        if (value.is_number_unsigned() &&
                            value.get<uint64_t>() > 9007199254740991ULL)
                            return {
                                {"$mapget", "uint64"},
                                {"value", std::to_string(value.get<uint64_t>())}};
                        if (value.is_number_integer() && !value.is_number_unsigned()) {
                            auto number = value.get<int64_t>();
                            if (number < -9007199254740991LL || number > 9007199254740991LL)
                                return {{"$mapget", "int64"}, {"value", std::to_string(number)}};
                        }
                        return value;
                    }
                    auto result = node.type() == simfil::ValueType::Array ?
                        nlohmann::json::array() :
                        nlohmann::json::object();
                    for (uint32_t i = 0; i < node.size(); ++i) {
                        auto child = node.at(i);
                        if (node.type() == simfil::ValueType::Array)
                            result.push_back(copyMetadata(*child, depth + 1));
                        else
                            result[copyName(
                                node.owningModel()->lookupStringId(node.keyAt(i)).value())] =
                                copyMetadata(*child, depth + 1);
                    }
                    return result;
                };
                for (uint32_t i = 0; i < root->size(); ++i) {
                    checkpoint();
                    auto key = root->keyAt(i);
                    auto name = copyName(root->owningModel()->lookupStringId(key).value());
                    auto child = root->at(i);
                    if (key != simfil::StringPool::SchemaFields &&
                        key != simfil::StringPool::SchemaElements &&
                        key != simfil::StringPool::SchemaAlternatives)
                    {
                        body["descriptor"][name] = copyMetadata(*child, 0);
                        continue;
                    }
                    auto& edges = body[name] = nlohmann::json::array();
                    for (uint32_t j = 0; j < child->size(); ++j) {
                        reserve(64);
                        auto entry = child->at(j);
                        auto link = nlohmann::json::object();
                        if (key == simfil::StringPool::SchemaFields)
                            link["name"] = copyName(
                                child->owningModel()->lookupStringId(child->keyAt(j)).value());
                        if (auto reference = entry->get(simfil::StringPool::SchemaRef))
                            link["$ref"] = copyMetadata(*reference, 0);
                        // Immediate native metadata gives real alternatives useful
                        // labels without another request or a client-side schema crawl.
                        // Keep every edge: equal labels are not proof of equal types.
                        for (auto key :
                             {simfil::StringPool::SchemaKind,
                              simfil::StringPool::SchemaTypeName,
                              simfil::StringPool::SchemaNullable})
                        {
                            if (auto value = entry->get(key))
                                link["descriptor"]
                                    [copyName(entry->owningModel()->lookupStringId(key).value())] =
                                        copyMetadata(*value, 0);
                        }
                        // Summaries distinguish enum strings from numeric fallbacks
                        // without copying a potentially large enum onto every edge.
                        // The complete values and raw ordinals remain on expansion.
                        if (auto values = entry->get(simfil::StringPool::SchemaEnum)) {
                            reserve(64);
                            link["descriptor"]["enumCount"] = values->size();
                        }
                        if (auto values = entry->get(simfil::StringPool::SchemaNamedValues)) {
                            reserve(64);
                            auto const count = values->size();
                            link["descriptor"]["namedValueCount"] = count;
                            auto& names = link["descriptor"]["namedValueNames"] =
                                nlohmann::json::array();
                            for (uint32_t index = 0; index < std::min(count, uint32_t{3}); ++index)
                            {
                                if (auto name =
                                        values->at(index)->get(simfil::StringPool::SchemaName))
                                    names.push_back(copyMetadata(*name, 0));
                            }
                        }
                        // A shared typename does not imply equal numeric meaning.
                        // Keep scalar constraints and at most two ordinary ranges;
                        // the preview is bounded independently of the full definition.
                        if (auto numeric = entry->get(simfil::StringPool::SchemaNumericDomain)) {
                            auto& preview = link["descriptor"]["numericDomain"] =
                                nlohmann::json::object();
                            for (auto key :
                                 {simfil::StringPool::SchemaEncoding,
                                  simfil::StringPool::SchemaUnit,
                                  simfil::StringPool::SchemaMinimum,
                                  simfil::StringPool::SchemaMaximum,
                                  simfil::StringPool::SchemaScale,
                                  simfil::StringPool::SchemaOffset,
                                  simfil::StringPool::SchemaComplete})
                            {
                                if (auto value = numeric->get(key))
                                    preview[copyName(
                                        numeric->owningModel()->lookupStringId(key).value())] =
                                        copyMetadata(*value, 0);
                            }
                            if (auto ranges =
                                    numeric->get(simfil::StringPool::SchemaOrdinaryRanges)) {
                                reserve(64);
                                auto const count = ranges->size();
                                preview["ordinaryRangeCount"] = count;
                                auto& examples = preview["ordinaryRanges"] =
                                    nlohmann::json::array();
                                for (uint32_t index = 0; index < std::min(count, uint32_t{2});
                                     ++index)
                                    examples.push_back(copyMetadata(*ranges->at(index), 0));
                            }
                        }
                        // Requiredness and context-only lookup are properties of this edge,
                        // not of the destination type shared by other fields.
                        if (auto required = entry->get(simfil::StringPool::SchemaRequired))
                            link["required"] = copyMetadata(*required, 0);
                        if (auto role = entry->get(simfil::StringPool::SchemaRole))
                            link["role"] = copyMetadata(*role, 0);
                        edges.push_back(std::move(link));
                    }
                }
            }
            auto encoded = body.dump();
            if (encoded.size() > 1024 * 1024)
                throw std::length_error(
                    "Schema inspection response exceeds one MiB; use a focused MCP query.");
            response = drogon::HttpResponse::newHttpResponse();
            response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            response->addHeader("Cache-Control", "no-store");
            response->setBody(std::move(encoded));
            // Encoding can outlive a reload. Revalidate only after all materialization,
            // once per response, so no stale consumer rows or type view are published.
            if (inventory && service->sourceCatalogRevision() != revision)
                response =
                    schemaError(drogon::k409Conflict, "Schema catalog changed; retry inspection.");
            else if (!inventory && service->getSchema({}, artifact, headers) != owner)
                response = schemaError(drogon::k404NotFound, "Schema binding is unavailable.");
        }
        catch (std::length_error const&) {
            response = schemaError(
                drogon::k413RequestEntityTooLarge,
                "Inspection budget exceeded; use a focused schema query.");
        }
        catch (std::invalid_argument const&) {
            response = schemaError(drogon::k400BadRequest, "Invalid schema search arguments.");
        }
        catch (std::system_error const& error) {
            response = error.code() == std::errc::resource_unavailable_try_again ?
                schemaError(drogon::k409Conflict, "Schema catalog changed; retry inspection.") :
                schemaError(drogon::k500InternalServerError, "Schema inspection failed.");
        }
        catch (...) {
            response = schemaError(drogon::k500InternalServerError, "Schema inspection failed.");
        }
        callback(response);
    };
    if (!service->scheduleTask(std::move(task)))
        callback(schemaError(
            drogon::k503ServiceUnavailable,
            "Schema request queue is full or stopping."));
}

} // namespace mapget
