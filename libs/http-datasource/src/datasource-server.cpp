#include "datasource-server.h"

#include "mapget/log.h"
#include "mapget/model/info.h"
#include "mapget/model/stream.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>

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
    std::shared_ptr<StringPool> strings_;

    explicit Impl(DataSourceInfo info) : info_(std::move(info)), strings_(std::make_shared<StringPool>(info_.nodeId_))
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
                TileId tileId{std::stoull(tileIdParam)};

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
                            tileId, impl_->info_.nodeId_, impl_->info_.mapId_, layer, impl_->strings_);
                        impl_->tileFeatureCallback_(tileFeatureLayer);
                        return tileFeatureLayer;
                    }
                    case mapget::LayerType::SourceData: {
                        auto tileSourceLayer = std::make_shared<TileSourceDataLayer>(
                            tileId, impl_->info_.nodeId_, impl_->info_.mapId_, layer, impl_->strings_);
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
                    TileLayerStream::StringPoolOffsetMap stringPoolOffsets{{impl_->info_.nodeId_, stringPoolOffsetParam}};
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
}

}  // namespace mapget
