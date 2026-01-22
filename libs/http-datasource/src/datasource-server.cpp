#include "datasource-server.h"

#include "mapget/log.h"
#include "mapget/model/info.h"
#include "mapget/model/stream.h"

#include <App.h>

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

void DataSourceServer::setup(uWS::App& app)
{
    app.get("/tile", [this](auto* res, auto* req) {
        try {
            auto layerIdParam = req->getQuery("layer");
            auto tileIdParam = req->getQuery("tileId");

            if (layerIdParam.empty() || tileIdParam.empty()) {
                res->writeStatus("400 Bad Request");
                res->writeHeader("Content-Type", "text/plain");
                res->end("Missing query parameter: layer and/or tileId");
                return;
            }

            auto layer = impl_->info_.getLayer(std::string(layerIdParam));

            TileId tileId{std::stoull(std::string(tileIdParam))};

            auto stringPoolOffsetParam = (simfil::StringId)0;
            auto stringPoolOffsetStr = req->getQuery("stringPoolOffset");
            if (!stringPoolOffsetStr.empty()) {
                stringPoolOffsetParam = (simfil::StringId)std::stoul(std::string(stringPoolOffsetStr));
            }

            std::string responseType = "binary";
            auto responseTypeStr = req->getQuery("responseType");
            if (!responseTypeStr.empty()) {
                responseType = std::string(responseTypeStr);
            }

            auto tileLayer = [&]() -> std::shared_ptr<TileLayer> {
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

            if (responseType == "binary") {
                std::string content;
                TileLayerStream::StringPoolOffsetMap stringPoolOffsets{
                    {impl_->info_.nodeId_, stringPoolOffsetParam}};
                TileLayerStream::Writer layerWriter{
                    [&](std::string bytes, TileLayerStream::MessageType) { content.append(bytes); },
                    stringPoolOffsets};
                layerWriter.write(tileLayer);

                res->writeStatus("200 OK");
                res->writeHeader("Content-Type", "application/binary");
                res->end(content);
            } else {
                res->writeStatus("200 OK");
                res->writeHeader("Content-Type", "application/json");
                res->end(tileLayer->toJson().dump());
            }
        }
        catch (std::exception const& e) {
            res->writeStatus("500 Internal Server Error");
            res->writeHeader("Content-Type", "text/plain");
            res->end(std::string("Error: ") + e.what());
        }
    });

    app.get("/info", [this](auto* res, auto* /*req*/) {
        res->writeStatus("200 OK");
        res->writeHeader("Content-Type", "application/json");
        res->end(impl_->info_.toJson().dump());
    });

    app.post("/locate", [this](auto* res, auto* /*req*/) {
        auto aborted = std::make_shared<std::atomic_bool>(false);
        res->onAborted([aborted]() { *aborted = true; });

        res->onData([this, res, aborted, body = std::string()](std::string_view chunk, bool last) mutable {
            if (*aborted)
                return;
            body.append(chunk.data(), chunk.size());
            if (!last)
                return;
            try {
                LocateRequest parsedReq(nlohmann::json::parse(body));
                auto responseJson = nlohmann::json::array();

                if (impl_->locateCallback_) {
                    for (auto const& response : impl_->locateCallback_(parsedReq)) {
                        responseJson.emplace_back(response.serialize());
                    }
                }

                if (*aborted)
                    return;
                res->writeStatus("200 OK");
                res->writeHeader("Content-Type", "application/json");
                res->end(responseJson.dump());
            }
            catch (std::exception const& e) {
                if (*aborted)
                    return;
                res->writeStatus("400 Bad Request");
                res->writeHeader("Content-Type", "text/plain");
                res->end(std::string("Invalid request: ") + e.what());
            }
        });
    });
}

}  // namespace mapget
