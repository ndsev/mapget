#include "http-service-impl.h"

#include <drogon/HttpResponse.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "fmt/format.h"
#include "mapget/model/layerschema.h"
#include "mapget/model/stream.h"
#include "nlohmann/json.hpp"
#include "picosha2.h"

namespace mapget
{
namespace
{

/** Diagnostic headers must remain bounded single-line values, including validator errors. */
std::string configMessageHeader(std::string_view message)
{
    constexpr size_t maxBytes = 1024;
    std::string result;
    result.reserve(std::min(message.size(), maxBytes));
    for (unsigned char c : message) {
        if (result.size() == maxBytes) break;
        // ASCII is safe across HTTP implementations; detailed diagnostics stay server-side.
        result.push_back(c < 0x20 || c == 0x7f ? ' ' : c < 0x80 ? static_cast<char>(c) : '?');
    }
    return result;
}

std::string_view catalogStatusToString(DataSourceCatalogStatus status)
{
    switch (status) {
    case DataSourceCatalogStatus::Initializing:
        return "initializing";
    case DataSourceCatalogStatus::Ready:
        return "ready";
    case DataSourceCatalogStatus::Failed:
        return "failed";
    }
    return "failed";
}

bool shouldBlockUntilReloadDone(const drogon::HttpRequestPtr& req)
{
    auto value = req->getParameter("blocking");
    if (value.empty()) {
        return true;
    }
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return !(value == "false" || value == "0" || value == "no");
}

nlohmann::json sourceCatalogEntryToJson(DataSourceCatalogEntry const& entry)
{
    nlohmann::json result;
    if (entry.info) {
        result = entry.info->toJson();
    }
    else {
        result = nlohmann::json::object({
            {"stringPoolId", entry.descriptor.displayName},
            {"mapId", entry.descriptor.displayName},
            {"layers", nlohmann::json::object()},
            {"maxParallelJobs", 0},
            {"addOn", entry.descriptor.addOn},
            {"extraJsonAttachment", nlohmann::json::object()},
            {"protocolVersion", TileLayerStream::CurrentProtocolVersion.toJson()},
        });
    }

    result["sourceId"] = entry.descriptor.sourceId;
    result["configIndex"] = entry.descriptor.configIndex;
    result["type"] = entry.descriptor.type;
    result["status"] = std::string(catalogStatusToString(entry.status));
    result["statusMessage"] = entry.statusMessage;
    result["retrying"] = entry.retrying;
    if (entry.progress) {
        result["progress"] = *entry.progress;
    }
    return result;
}

}  // namespace

void HttpService::Impl::handleSourcesRequest(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
{
    auto catalog = self_.sourceCatalog(
        config_.proxyAuth.authorization(req),
        shouldBlockUntilReloadDone(req));
    // The revision alone is shared by callers with different datasource permissions.
    // Bind validators to the visible rows so changing identity cannot reuse a broader catalog.
    auto visibleSources = nlohmann::json::array();
    for (auto const& source : catalog.sources)
        visibleSources.push_back(source.descriptor.sourceId);
    auto const etag = fmt::format("\"sources-{}-{}\"", catalog.revision,
        picosha2::hash256_hex_string(visibleSources.dump()));
    if (req->getHeader("if-none-match") == etag) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k304NotModified);
        resp->addHeader("ETag", etag);
        resp->addHeader("Cache-Control", "private, no-cache");
        resp->addHeader("X-Mapget-Sources-Revision", std::to_string(catalog.revision));
        resp->addHeader("X-Mapget-Sources-Config-Status", catalog.configStatus);
        if (!catalog.configStatusMessage.empty()) {
            resp->addHeader("X-Mapget-Sources-Config-Message", configMessageHeader(catalog.configStatusMessage));
        }
        callback(resp);
        return;
    }

    // A small, authorization-filtered capability projection. This is derived from
    // finalized forward declarations, not a producer opt-in or a second schema.
    std::map<std::string, std::set<std::string>> possibleTargets;
    std::set<std::string> unknownMaps;
    for (auto const& source : catalog.sources) {
        if (!source.info)
            continue;
        for (auto const& [_, layer] : source.info->layers_) {
            if (!layer->canRead_ || layer->type_ != LayerType::Features)
                continue;
            auto schema = layer->layerSchema();
            if (!schema->hasRelationDeclarations())
                unknownMaps.insert(source.info->mapId_);
            for (auto const& family : schema->relationDeclarations())
                possibleTargets[source.info->mapId_]
                    .insert(family.targetTypes_.begin(), family.targetTypes_.end());
        }
    }
    auto sourcesInfo = nlohmann::json::array();
    for (auto const& source : catalog.sources) {
        auto json = sourceCatalogEntryToJson(source);
        if (source.info) {
            for (auto const& [name, layer] : source.info->layers_) {
                if (layer->type_ != LayerType::Features)
                    continue;
                auto eligible = nlohmann::json::array();
                for (auto const& type : layer->layerSchema()->featureTypes())
                    if (possibleTargets[source.info->mapId_].contains(type))
                        eligible.push_back(type);
                json["layers"][name]["incomingRelationTargetTypes"] = std::move(eligible);
                json["layers"][name]["incomingRelationCoverageUnknown"] =
                    unknownMaps.contains(source.info->mapId_);
            }
        }
        sourcesInfo.push_back(std::move(json));
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->addHeader("ETag", etag);
    resp->addHeader("Cache-Control", "private, no-cache");
    resp->addHeader("X-Mapget-Sources-Revision", std::to_string(catalog.revision));
    resp->addHeader("X-Mapget-Sources-Config-Status", catalog.configStatus);
    if (!catalog.configStatusMessage.empty()) {
        resp->addHeader("X-Mapget-Sources-Config-Message", configMessageHeader(catalog.configStatusMessage));
    }
    resp->setBody(sourcesInfo.dump());
    callback(resp);
}

}  // namespace mapget
