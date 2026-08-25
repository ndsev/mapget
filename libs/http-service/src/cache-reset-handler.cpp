#include "http-service-impl.h"

#include "mapget/log.h"
#include "nlohmann/json.hpp"

#include <drogon/HttpResponse.h>

#include <string>

namespace mapget
{
namespace
{

constexpr size_t kMaximumRequestBodyBytes = 8192;
constexpr size_t kMaximumMapIdBytes = 4096;

[[nodiscard]] drogon::HttpResponsePtr plainResponse(
    drogon::HttpStatusCode status,
    std::string body = {})
{
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(status);
    if (!body.empty()) {
        response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        response->setBody(std::move(body));
    }
    return response;
}

}  // namespace

void HttpService::Impl::handleCacheResetRequest(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
{
    auto const clientHeaders =
        detail::authHeadersFromRequest(req);
    if (!config_.cacheResetEnabled ||
        !authHeadersMatch(
            config_.cacheResetAuthHeaderAlternatives,
            clientHeaders))
    {
        callback(plainResponse(
            drogon::k403Forbidden,
            "Cache reset is not available for this caller."));
        return;
    }

    if (req->body().size() > kMaximumRequestBodyBytes) {
        callback(plainResponse(
            drogon::k400BadRequest,
            "Cache reset request is too large."));
        return;
    }
    if (req->contentType() != drogon::CT_APPLICATION_JSON) {
        callback(plainResponse(
            drogon::k400BadRequest,
            "Cache reset requires application/json."));
        return;
    }

    try {
        auto body = nlohmann::json::parse(req->body());
        if (!body.is_object() ||
            !body.contains("mapId") ||
            !body["mapId"].is_string())
        {
            callback(plainResponse(
                drogon::k400BadRequest,
                "A string mapId is required."));
            return;
        }

        auto mapId = body["mapId"].get<std::string>();
        if (mapId.empty() || mapId.size() > kMaximumMapIdBytes) {
            callback(plainResponse(
                drogon::k400BadRequest,
                "mapId must contain between 1 and 4096 bytes."));
            return;
        }

        if (!self_.resetMapCache(mapId, clientHeaders)) {
            callback(plainResponse(
                drogon::k404NotFound,
                "No resettable map was found."));
            return;
        }
        callback(plainResponse(drogon::k204NoContent));
    }
    catch (nlohmann::json::exception const&) {
        callback(plainResponse(
            drogon::k400BadRequest,
            "Invalid cache reset request."));
    }
    catch (std::exception const& error) {
        log().error(
            "POST /cache/reset failed: {}",
            error.what());
        callback(plainResponse(
            drogon::k500InternalServerError,
            "Cache reset failed."));
    }
}

}  // namespace mapget
