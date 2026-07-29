#include "datasource-server.h"

#include "mapget/log.h"
#include "mapget/model/info.h"
#include "mapget/model/stream.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

#include "fmt/format.h"

namespace mapget
{

struct DataSourceServer::Impl
{
    DataSourceInfo info_;
    std::function<void(TileFeatureLayer::Ptr)> tileFeatureCallback_ = [](auto&&) {
        throw std::runtime_error("TileFeatureLayer callback is unset!");
    };
    std::function<void(TileSourceDataLayer::Ptr)> tileSourceDataCallback_ = [](auto&&) {
        throw std::runtime_error("TileSourceDataLayer callback is unset!");
    };
    std::function<std::vector<LocateResponse>(const LocateRequest&)> locateCallback_;
    std::function<std::optional<AttachmentResponse>(
        AttachmentRequest const&)>
        attachmentCallback_;
    std::function<void(MapTileKey const&, std::chrono::system_clock::time_point)> cacheExpiredCallback_;
    std::shared_ptr<StringPool> strings_;

    explicit Impl(DataSourceInfo info) : info_(std::move(info)), strings_(std::make_shared<StringPool>(info_.stringPoolId_))
    {
    }
};

DataSourceServer::DataSourceServer(DataSourceInfo const& info) : HttpServer(), impl_(new Impl(info))
{
    printPortToStdOut(true);
}

DataSourceServer::~DataSourceServer() = default;

DataSourceServer& DataSourceServer::onTileFeatureRequest(std::function<void(TileFeatureLayer::Ptr)> const& callback)
{
    impl_->tileFeatureCallback_ = callback;
    return *this;
}

DataSourceServer& DataSourceServer::onTileSourceDataRequest(std::function<void(TileSourceDataLayer::Ptr)> const& callback)
{
    impl_->tileSourceDataCallback_ = callback;
    return *this;
}

DataSourceServer& DataSourceServer::onLocateRequest(
    const std::function<std::vector<LocateResponse>(const LocateRequest&)>& callback)
{
    impl_->locateCallback_ = callback;
    return *this;
}

DataSourceServer& DataSourceServer::onAttachmentRequest(
    std::function<std::optional<AttachmentResponse>(
        AttachmentRequest const&)> const& callback)
{
    impl_->attachmentCallback_ = callback;
    return *this;
}

DataSourceServer& DataSourceServer::onCacheExpired(
    const std::function<void(MapTileKey const&, std::chrono::system_clock::time_point)>& callback)
{
    impl_->cacheExpiredCallback_ = callback;
    return *this;
}

DataSourceInfo const& DataSourceServer::info() { return impl_->info_; }

void DataSourceServer::setup(drogon::HttpAppFramework& app)
{
    app.registerHandler(
        "/tile",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        {
            try {
                auto const& layerIdParam = req->getParameter("layer");
                auto const& tileIdParam = req->getParameter("tileId");

                if (layerIdParam.empty() || tileIdParam.empty()) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k400BadRequest);
                    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                    resp->setBody("Missing query parameter: layer and/or tileId");
                    callback(resp);
                    return;
                }

                auto layer = impl_->info_.getLayer(layerIdParam);
                auto tileId = TileId::fromValue(std::stoi(tileIdParam));

                auto stringPoolOffsetParam = (simfil::StringId)0;
                auto const& stringPoolOffsetStr = req->getParameter("stringPoolOffset");
                if (!stringPoolOffsetStr.empty()) {
                    stringPoolOffsetParam = (simfil::StringId)std::stoul(stringPoolOffsetStr);
                }

                std::string responseType = "binary";
                auto const& responseTypeStr = req->getParameter("responseType");
                if (!responseTypeStr.empty())
                    responseType = responseTypeStr;

                auto tileLayer = [&]() -> std::shared_ptr<TileLayer>
                {
                    switch (layer->type_) {
                    case mapget::LayerType::Features: {
                        auto tileFeatureLayer = std::make_shared<TileFeatureLayer>(
                            tileId, impl_->info_.stringPoolId_, impl_->info_.mapId_, layer, impl_->strings_);
                        impl_->tileFeatureCallback_(tileFeatureLayer);
                        return tileFeatureLayer;
                    }
                    case mapget::LayerType::SourceData: {
                        auto tileSourceLayer = std::make_shared<TileSourceDataLayer>(
                            tileId, impl_->info_.stringPoolId_, impl_->info_.mapId_, layer, impl_->strings_);
                        impl_->tileSourceDataCallback_(tileSourceLayer);
                        return tileSourceLayer;
                    }
                    default:
                        throw std::runtime_error(fmt::format("Unsupported layer type {}", (int)layer->type_));
                    }
                }();

                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);

                if (responseType == "binary") {
                    std::string content;
                    TileLayerStream::StringPoolOffsetMap stringPoolOffsets{{impl_->info_.stringPoolId_, stringPoolOffsetParam}};
                    TileLayerStream::Writer layerWriter{
                        [&](std::string const& bytes, TileLayerStream::MessageType) { content.append(bytes); },
                        stringPoolOffsets};
                    layerWriter.write(tileLayer);

                    resp->setContentTypeString("application/binary");
                    resp->setBody(std::move(content));
                } else {
                    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                    resp->setBody(tileLayer->toJson().dump());
                }

                callback(resp);
            }
            catch (std::exception const& e) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k500InternalServerError);
                resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                resp->setBody(std::string("Error: ") + e.what());
                callback(resp);
            }
        },
        {drogon::Get});

    app.registerHandler(
        "/info",
        [this](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(impl_->info_.toJson().dump());
            callback(resp);
        },
        {drogon::Get});

    app.registerHandler(
        "/locate",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        {
            try {
                LocateRequest parsedReq(nlohmann::json::parse(std::string(req->body())));
                auto responseJson = nlohmann::json::array();

                if (impl_->locateCallback_) {
                    for (auto const& response : impl_->locateCallback_(parsedReq)) {
                        responseJson.emplace_back(response.serialize());
                    }
                }

                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                resp->setBody(responseJson.dump());
                callback(resp);
            }
            catch (std::exception const& e) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k400BadRequest);
                resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                resp->setBody(std::string("Invalid request: ") + e.what());
                callback(resp);
            }
        },
        {drogon::Post});

    app.registerHandler(
        "/attachment",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(
                const drogon::HttpResponsePtr&)>&&
                callback)
        {
            try {
                auto layerId =
                    req->getParameter("layer");
                auto tileId =
                    req->getParameter("tileId");
                auto name =
                    req->getParameter("name");
                if (layerId.empty() ||
                    tileId.empty() ||
                    name.empty())
                {
                    auto response =
                        drogon::HttpResponse::
                            newHttpResponse();
                    response->setStatusCode(
                        drogon::k400BadRequest);
                    response->setBody(
                        "Missing query parameter: "
                        "layer, tileId, and/or name");
                    callback(response);
                    return;
                }
                auto layer =
                    impl_->info_.getLayer(
                        layerId);
                if (!layer ||
                    layer->type_ !=
                        LayerType::Features ||
                    !impl_->attachmentCallback_)
                {
                    auto response =
                        drogon::HttpResponse::
                            newHttpResponse();
                    response->setStatusCode(
                        drogon::k404NotFound);
                    callback(response);
                    return;
                }

                auto request =
                    AttachmentRequest{
                        .tileKey_ = MapTileKey(
                            LayerType::Features,
                            impl_->info_.mapId_,
                            std::move(layerId),
                            TileId::fromValue(
                                std::stoi(
                                    tileId))),
                        .name_ =
                            std::move(name),
                    };
                auto attachment =
                    impl_->attachmentCallback_(
                        request);
                if (!attachment ||
                    attachment->name_ !=
                        request.name_ ||
                    !attachment->bytes_)
                {
                    auto response =
                        drogon::HttpResponse::
                            newHttpResponse();
                    response->setStatusCode(
                        drogon::k404NotFound);
                    callback(response);
                    return;
                }
                if (attachment->etag_ &&
                    req->getHeader(
                        "if-none-match") ==
                        *attachment->etag_)
                {
                    auto response =
                        drogon::HttpResponse::
                            newHttpResponse();
                    response->setStatusCode(
                        drogon::k304NotModified);
                    response->addHeader(
                        "ETag",
                        *attachment->etag_);
                    callback(response);
                    return;
                }

                auto response =
                    drogon::HttpResponse::
                        newHttpResponse();
                response->setStatusCode(
                    drogon::k200OK);
                response->setContentTypeString(
                    attachment->mimeType_.empty()
                        ? "application/octet-stream"
                        : attachment->mimeType_);
                if (attachment->etag_) {
                    response->addHeader(
                        "ETag",
                        *attachment->etag_);
                }
                response->setBody(std::string(
                    attachment->bytes_->begin(),
                    attachment->bytes_->end()));
                callback(response);
            }
            catch (std::exception const& error) {
                auto response =
                    drogon::HttpResponse::
                        newHttpResponse();
                response->setStatusCode(
                    drogon::k400BadRequest);
                response->setContentTypeCode(
                    drogon::CT_TEXT_PLAIN);
                response->setBody(
                    std::string(
                        "Invalid request: ") +
                    error.what());
                callback(response);
            }
        },
        {drogon::Get});

    app.registerHandler(
        "/cache-expired",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        {
            try {
                if (impl_->cacheExpiredCallback_) {
                    auto const body = nlohmann::json::parse(std::string(req->body()));
                    auto const tileKey = MapTileKey(body.at("tileKey").get<std::string>());
                    auto const expiredAtUs = body.at("expiredAt").get<int64_t>();
                    auto const expiredAt = std::chrono::system_clock::time_point{
                        std::chrono::microseconds{expiredAtUs}};
                    impl_->cacheExpiredCallback_(tileKey, expiredAt);
                }

                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k204NoContent);
                callback(resp);
            }
            catch (std::exception const& e) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k400BadRequest);
                resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                resp->setBody(std::string("Invalid request: ") + e.what());
                callback(resp);
            }
        },
        {drogon::Post});
}

}  // namespace mapget
