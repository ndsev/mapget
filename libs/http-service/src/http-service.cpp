#include "http-service-impl.h"

#include "tiles-ws-controller.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <utility>

namespace mapget
{

HttpService::HttpService(Cache::Ptr cache, const HttpServiceConfig& config)
    : Service(std::move(cache), config.watchConfig, config.defaultTtl, config.workerCount),
      impl_(std::make_unique<Impl>(*this, config))
{
}

HttpService::~HttpService()
{
    // Stop Drogon before destroying the executor used by websocket sessions.
    stop();
}

bool HttpService::enqueueInteractiveControlTask(std::function<void()> task)
{
    return impl_->enqueueInteractiveControlTask(std::move(task));
}

void HttpService::setup(drogon::HttpAppFramework& app)
{
    detail::registerTilesWebSocketController(app, *this);

    app.registerHandler(
        "/tiles",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        { impl_->handleTilesRequest(req, std::move(callback)); },
        {drogon::Post});

    app.registerHandler(
        "/filter",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        { impl_->handleFilterRequest(req, std::move(callback)); },
        {drogon::Post});

    app.registerHandler(
        "/attachment",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        { impl_->handleAttachmentRequest(req, std::move(callback)); },
        {drogon::Get});

    app.registerHandler(
        "/sources",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        { impl_->handleSourcesRequest(req, std::move(callback)); },
        {drogon::Get});

    app.registerHandler(
        "/status",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        { impl_->handleStatusRequest(req, std::move(callback)); },
        {drogon::Get});

    app.registerHandler(
        "/status-data",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        { impl_->handleStatusDataRequest(req, std::move(callback)); },
        {drogon::Get});

    app.registerHandler(
        "/status-data/cache-report",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        { impl_->handleStatusCacheReportRequest(req, std::move(callback)); },
        {drogon::Post});

    app.registerHandler(
        "/locate",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        { impl_->handleLocateRequest(req, std::move(callback)); },
        {drogon::Post});

    app.registerHandler(
        "/location",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        { impl_->handleLocationRequest(req, std::move(callback)); },
        {drogon::Get});

    app.registerHandler(
        "/cache/reset",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            impl_->handleCacheResetRequest(req, std::move(callback));
        },
        {drogon::Post});

    app.registerHandler(
        "/config",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        {
            if (req->method() == drogon::Get) {
                impl_->handleGetConfigRequest(req, std::move(callback));
                return;
            }
            if (req->method() == drogon::Post) {
                impl_->handlePostConfigRequest(req, std::move(callback));
                return;
            }
            if (req->method() == drogon::Patch) {
                impl_->handlePatchConfigRequest(req, std::move(callback));
                return;
            }

            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k405MethodNotAllowed);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody("Method not allowed");
            callback(resp);
        },
        {drogon::Get, drogon::Post, drogon::Patch});
}

}  // namespace mapget
