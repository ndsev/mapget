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
    std::function<ObjectDiscoveryResult(ObjectDiscoveryRequest const&)> discoveryCallback_;
    std::function<void(PartitionFeatureLayer::Ptr)> tileFeatureCallback_ = [](auto&&)
    {
        throw std::runtime_error("PartitionFeatureLayer callback is unset!");
    };
    std::function<void(PartitionSourceDataLayer::Ptr)> tileSourceDataCallback_ = [](auto&&)
    {
        throw std::runtime_error("PartitionSourceDataLayer callback is unset!");
    };
    std::function<
        std::vector<LocateCandidate>(
            LocateRequest const&)>
        locateCallback_;
    std::function<std::optional<AttachmentResponse>(
        AttachmentRequest const&)>
        attachmentCallback_;
    std::function<void(MapPartitionKey const&, std::chrono::system_clock::time_point)>
        cacheExpiredCallback_;
    std::shared_ptr<StringPool> strings_;

    explicit Impl(DataSourceInfo info) : info_(std::move(info)), strings_(std::make_shared<StringPool>(info_.stringPoolId_))
    {
    }
};

DataSourceServer::DataSourceServer(DataSourceInfo const& info) : HttpServer(), impl_(new Impl(info))
{
    printPortToStdOut(true);
}

DataSourceServer& DataSourceServer::onObjectDiscoveryRequest(
    std::function<ObjectDiscoveryResult(ObjectDiscoveryRequest const&)> const& callback)
{
    impl_->discoveryCallback_ = callback;
    return *this;
}

DataSourceServer::~DataSourceServer() = default;

DataSourceServer& DataSourceServer::onTileFeatureRequest(
    std::function<void(PartitionFeatureLayer::Ptr)> const& callback)
{
    impl_->tileFeatureCallback_ = callback;
    return *this;
}

DataSourceServer& DataSourceServer::onTileSourceDataRequest(
    std::function<void(PartitionSourceDataLayer::Ptr)> const& callback)
{
    impl_->tileSourceDataCallback_ = callback;
    return *this;
}

DataSourceServer& DataSourceServer::onLocateRequest(
    std::function<
        std::vector<LocateCandidate>(
            LocateRequest const&)> const& callback)
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
    const std::function<void(MapPartitionKey const&, std::chrono::system_clock::time_point)>&
        callback)
{
    impl_->cacheExpiredCallback_ = callback;
    return *this;
}

DataSourceInfo const& DataSourceServer::info() { return impl_->info_; }

void DataSourceServer::setup(drogon::HttpAppFramework& app)
{
    app.registerHandler(
        "/objects/discover",
        [this](
            drogon::HttpRequestPtr const& req,
            std::function<void(drogon::HttpResponsePtr const&)>&& callback)
        {
            auto response = drogon::HttpResponse::newHttpResponse();
            try {
                auto json = nlohmann::json::parse(req->body());
                ObjectDiscoveryRequest request{
                    impl_->info_.mapId_,
                    json.at("layerId").get<std::string>(),
                    PartitionId::fromJson({{"kind", "tile"}, {"id", json.at("tileId")}}).tileId()};
                auto layer = impl_->info_.getLayer(request.layerId_);
                if (!layer || layer->partitionKind_ != PartitionKind::Object ||
                    !layer->tileAssociationLevel_ || !request.tileId_.isValid() ||
                    request.tileId_.level() != *layer->tileAssociationLevel_)
                    throw std::invalid_argument("Unsupported object discovery layer or level.");
                ObjectDiscoveryResult result;
                if (impl_->discoveryCallback_)
                    result = impl_->discoveryCallback_(request);
                else
                    result.status_ = ObjectDiscoveryResult::Status::Unavailable;
                response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                response->setBody(result.toJson().dump());
            }
            catch (std::exception const& error) {
                response->setStatusCode(drogon::k400BadRequest);
                response->setBody(error.what());
            }
            callback(response);
        },
        {drogon::Post});

    app.registerHandler(
        "/tile",
        [this](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        {
            try {
                auto const& layerIdParam = req->getParameter("layer");
                auto const& tileIdParam = req->getParameter("tileId");

                if (layerIdParam.empty() ||
                    (tileIdParam.empty() && req->getParameter("partition").empty())) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k400BadRequest);
                    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                    resp->setBody("Missing query parameter: layer and/or tileId");
                    callback(resp);
                    return;
                }

                auto layer = impl_->info_.getLayer(layerIdParam);
                auto tileId = req->getParameter("partition").empty() ?
                    PartitionId::fromValue(std::stoi(tileIdParam)) :
                    PartitionId::fromJson(nlohmann::json::parse(req->getParameter("partition")));

                auto stringPoolOffsetParam = (simfil::StringId)0;
                auto const& stringPoolOffsetStr = req->getParameter("stringPoolOffset");
                if (!stringPoolOffsetStr.empty()) {
                    stringPoolOffsetParam = (simfil::StringId)std::stoul(stringPoolOffsetStr);
                }

                std::string responseType = "binary";
                auto const& responseTypeStr = req->getParameter("responseType");
                if (!responseTypeStr.empty())
                    responseType = responseTypeStr;

                auto tileLayer = [&]() -> std::shared_ptr<PartitionLayer>
                {
                    switch (layer->type_) {
                    case mapget::LayerType::Features: {
                        auto tileFeatureLayer = std::make_shared<PartitionFeatureLayer>(
                            tileId,
                            impl_->info_.stringPoolId_,
                            impl_->info_.mapId_,
                            layer,
                            impl_->strings_);
                        impl_->tileFeatureCallback_(tileFeatureLayer);
                        return tileFeatureLayer;
                    }
                    case mapget::LayerType::SourceData: {
                        auto tileSourceLayer = std::make_shared<PartitionSourceDataLayer>(
                            tileId,
                            impl_->info_.stringPoolId_,
                            impl_->info_.mapId_,
                            layer,
                            impl_->strings_);
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
                if (layerId.empty() || (tileId.empty() && req->getParameter("partition").empty()) ||
                    name.empty()) {
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

                auto request = AttachmentRequest{
                    .tileKey_ = MapPartitionKey(
                        LayerType::Features,
                        impl_->info_.mapId_,
                        std::move(layerId),
                        req->getParameter("partition").empty() ?
                            PartitionId::fromValue(std::stoi(tileId)) :
                            PartitionId::fromJson(
                                nlohmann::json::parse(req->getParameter("partition")))),
                    .name_ = std::move(name),
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
                    auto const tileKey = MapPartitionKey(body.at("tileKey").get<std::string>());
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
