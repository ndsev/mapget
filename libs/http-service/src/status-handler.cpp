#include "http-service-impl.h"

#include <drogon/HttpResponse.h>

#include <sstream>

namespace mapget
{

void HttpService::Impl::handleStatusRequest(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
{
    auto serviceStats = self_.getStatistics();
    auto cacheStats = self_.cache()->getStatistics();

    std::ostringstream oss;
    oss << "<html><body>";
    oss << "<h1>Status Information</h1>";
    oss << "<h2>Service Statistics</h2>";
    oss << "<pre>" << serviceStats.dump(4) << "</pre>";
    oss << "<h2>Cache Statistics</h2>";
    oss << "<pre>" << cacheStats.dump(4) << "</pre>";
    oss << "</body></html>";

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeCode(drogon::CT_TEXT_HTML);
    resp->setBody(oss.str());
    callback(resp);
}

}  // namespace mapget

