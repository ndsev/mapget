#include "http-service-impl.h"

#include <drogon/HttpResponse.h>

#include "nlohmann/json.hpp"

namespace mapget
{

void HttpService::Impl::handleSourcesRequest(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
{
    auto sourcesInfo = nlohmann::json::array();
    for (auto& source : self_.info(detail::authHeadersFromRequest(req))) {
        sourcesInfo.push_back(source.toJson());
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(sourcesInfo.dump());
    callback(resp);
}

}  // namespace mapget

