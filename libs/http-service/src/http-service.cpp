#include "http-service-impl.h"

#include "tiles-ws-controller.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <utility>

namespace mapget
{

HttpService::HttpService(Cache::Ptr cache, const HttpServiceConfig& config)
    : Service(std::move(cache), config.watchConfig, config.defaultTtl), impl_(std::make_unique<Impl>(*this, config))
{
}

HttpService::~HttpService() = default;

void HttpService::setup(drogon::HttpAppFramework& app)
{
    detail::registerTilesWebSocketController(app, *this);

    app.registerHandler(
        "/tiles",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            impl_->handleTilesRequest(req, std::move(callback));
        },
        {drogon::Post});

    app.registerHandler(
        "/search",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            impl_->handleSearchRequest(req, std::move(callback));
        },
        {drogon::Post});

    app.registerHandler(
        "/sources",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            impl_->handleSourcesRequest(req, std::move(callback));
        },
        {drogon::Get});

    app.registerHandler(
        "/status",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            impl_->handleStatusRequest(req, std::move(callback));
        },
        {drogon::Get});

    app.registerHandler(
        "/status-data",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            impl_->handleStatusDataRequest(req, std::move(callback));
        },
        {drogon::Get});

    app.registerHandler(
        "/locate",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            impl_->handleLocateRequest(req, std::move(callback));
        },
        {drogon::Post});

    app.registerHandler(
        "/config",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            if (req->method() == drogon::Get) {
                Impl::handleGetConfigRequest(req, std::move(callback));
                return;
            }
            if (req->method() == drogon::Post) {
                impl_->handlePostConfigRequest(req, std::move(callback));
                return;
            }

            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k405MethodNotAllowed);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody("Method not allowed");
            callback(resp);
        },
        {drogon::Get, drogon::Post});
}

}  // namespace mapget
