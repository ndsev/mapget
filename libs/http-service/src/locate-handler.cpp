#include "http-service-impl.h"

#include <drogon/HttpResponse.h>

#include "nlohmann/json.hpp"

namespace mapget
{

void HttpService::Impl::handleLocateRequest(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
{
    try {
        nlohmann::json j = nlohmann::json::parse(std::string(req->body()));
        auto requestsJson = j["requests"];
        auto allResponsesJson = nlohmann::json::array();

        for (auto const& locateReqJson : requestsJson) {
            LocateRequest locateReq{locateReqJson};
            auto responsesJson = nlohmann::json::array();
            for (auto const& resp : self_.locate(locateReq))
                responsesJson.emplace_back(resp.serialize());
            allResponsesJson.emplace_back(responsesJson);
        }

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(nlohmann::json::object({{"responses", allResponsesJson}}).dump());
        callback(resp);
    }
    catch (const std::exception& e) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k400BadRequest);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody(std::string("Invalid JSON: ") + e.what());
        callback(resp);
    }
}

}  // namespace mapget

