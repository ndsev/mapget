#include "http-service-impl.h"

#include <drogon/HttpResponse.h>

#include <algorithm>
#include <cctype>

#include "fmt/format.h"
#include "mapget/model/stream.h"
#include "nlohmann/json.hpp"

namespace mapget
{
namespace
{

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
        detail::authHeadersFromRequest(req),
        shouldBlockUntilReloadDone(req));
    const auto etag = fmt::format("\"sources-{}\"", catalog.revision);
    if (req->getHeader("if-none-match") == etag) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k304NotModified);
        resp->addHeader("ETag", etag);
        resp->addHeader("X-Mapget-Sources-Revision", std::to_string(catalog.revision));
        resp->addHeader("X-Mapget-Sources-Config-Status", catalog.configStatus);
        if (!catalog.configStatusMessage.empty()) {
            resp->addHeader("X-Mapget-Sources-Config-Message", catalog.configStatusMessage);
        }
        callback(resp);
        return;
    }

    auto sourcesInfo = nlohmann::json::array();
    for (auto& source : catalog.sources) {
        sourcesInfo.push_back(sourceCatalogEntryToJson(source));
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->addHeader("ETag", etag);
    resp->addHeader("X-Mapget-Sources-Revision", std::to_string(catalog.revision));
    resp->addHeader("X-Mapget-Sources-Config-Status", catalog.configStatus);
    if (!catalog.configStatusMessage.empty()) {
        resp->addHeader("X-Mapget-Sources-Config-Message", catalog.configStatusMessage);
    }
    resp->setBody(sourcesInfo.dump());
    callback(resp);
}

}  // namespace mapget
