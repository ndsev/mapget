#include "http-service-impl.h"

#include <drogon/HttpResponse.h>

#include "nlohmann/json.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace mapget
{
namespace
{

/** Parse and clamp the public /location limit query parameter. */
uint32_t parseLimit(std::string_view rawLimit, uint32_t fallback, uint32_t maxLimit)
{
    if (rawLimit.empty()) {
        return std::max<uint32_t>(1, std::min(fallback, maxLimit));
    }

    uint32_t parsed = 0;
    auto const* begin = rawLimit.data();
    auto const* end = rawLimit.data() + rawLimit.size();
    auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc() || ptr != end || parsed == 0) {
        return std::max<uint32_t>(1, std::min(fallback, maxLimit));
    }
    return std::max<uint32_t>(1, std::min(parsed, maxLimit));
}

/** Create a JSON response with the status code expected by Drogon. */
drogon::HttpResponsePtr jsonResponse(nlohmann::json const& body, drogon::HttpStatusCode statusCode)
{
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(statusCode);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(body.dump());
    return resp;
}

}  // namespace

void HttpService::Impl::handleLocationRequest(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
{
    if (!locationLookup_ || !locationLookup_->available()) {
        callback(jsonResponse(
            nlohmann::json::object({{"error", "location database unavailable"}}),
            drogon::k503ServiceUnavailable));
        return;
    }

    auto const maxLimit = std::max<uint32_t>(1, config_.locationResultMaxLimit);
    auto const limit = parseLimit(req->getParameter("limit"), 10, maxLimit);
    auto const matches = locationLookup_->search(req->getParameter("name"), limit);

    auto response = nlohmann::json::array();
    for (auto const& match : matches) {
        response.emplace_back(match.serialize());
    }
    callback(jsonResponse(response, drogon::k200OK));
}

}  // namespace mapget
