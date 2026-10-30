#include "http-service-impl.h"

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
                 schemaId = std::move(schemaId), headers = detail::authHeadersFromRequest(req),
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

} // namespace mapget
