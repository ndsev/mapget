#include "datasource-server.h"
#include "mapget/detail/http-server.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/info.h"
#include "mapget/model/layer.h"
#include "mapget/model/stream.h"

#include "httplib.h"
#include <memory>
#include <stdexcept>

namespace mapget {

struct DataSourceServer::Impl
{
    DataSourceInfo info_;
    std::function<void(TileFeatureLayer::Ptr)> tileFeatureCallback_ = [](auto&&)
    {
        throw std::runtime_error("TileFeatureLayer callback is unset!");
    };
    std::function<void(TileSourceDataLayer::Ptr)> tileSourceDataCallback_ = [](auto&&)
    {
        throw std::runtime_error("TileSourceDataLayer callback is unset!");
    };
    std::function<std::vector<LocateResponse>(const LocateRequest&)> locateCallback_;
    std::shared_ptr<StringPool> fields_;

    explicit Impl(DataSourceInfo info)
        : info_(std::move(info)), fields_(std::make_shared<StringPool>(info_.nodeId_))
    {
    }
};

DataSourceServer::DataSourceServer(DataSourceInfo const& info)
    : HttpServer(), impl_(new Impl(info))
{
    printPortToStdOut(true);
}

DataSourceServer::~DataSourceServer() = default;

DataSourceServer&
DataSourceServer::onTileFeatureRequest(std::function<void(TileFeatureLayer::Ptr)> const& callback)
{
    impl_->tileFeatureCallback_ = callback;
    return *this;
}

DataSourceServer&
DataSourceServer::onTileSourceDataRequest(std::function<void(TileSourceDataLayer::Ptr)> const& callback)
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

DataSourceInfo const& DataSourceServer::info() {
    return impl_->info_;
}

void DataSourceServer::setup(httplib::Server& server)
{
    // Set up GET /tile endpoint
    server.Get(
        "/tile",
        [this](const httplib::Request& req, httplib::Response& res) {
            // Extract parameters from request.
            auto layerIdParam = req.get_param_value("layer");
            auto layer = impl_->info_.getLayer(layerIdParam);

            auto tileIdParam = TileId{std::stoull(req.get_param_value("tileId"))};
            auto fieldsOffsetParam = (simfil::StringId)0;
            if (req.has_param("fieldsOffset"))
                fieldsOffsetParam = (simfil::StringId)
                    std::stoul(req.get_param_value("fieldsOffset"));

            std::string responseType = "binary";
            if (req.has_param("responseType"))
                responseType = req.get_param_value("responseType");

            // Create response TileFeatureLayer.
            auto tileLayer = [&]() -> std::shared_ptr<TileLayer> {
                switch (layer->type_) {
                case mapget::LayerType::Features: {
                    auto tileFeatureLayer = std::make_shared<TileFeatureLayer>(
                        tileIdParam,
                        impl_->info_.nodeId_,
                        impl_->info_.mapId_,
                        layer,
                        impl_->fields_);
                    impl_->tileFeatureCallback_(tileFeatureLayer);
                    return tileFeatureLayer;
                }
                case mapget::LayerType::SourceData: {
                    auto tileSourceLayer = std::make_shared<TileSourceDataLayer>(
                        tileIdParam,
                        impl_->info_.nodeId_,
                        impl_->info_.mapId_,
                        layer,
                        impl_->fields_);
                    impl_->tileSourceDataCallback_(tileSourceLayer);
                    return tileSourceLayer;
                }
                default:
                    throw std::runtime_error(fmt::format("Unsupported layer type {}", (int)layer->type_));
                }
            }();

            // Serialize TileLayer using TileLayerStream.
            if (responseType == "binary") {
                std::stringstream content;
                TileLayerStream::StringOffsetMap fieldOffsets{
                    {impl_->info_.nodeId_, fieldsOffsetParam}};
                TileLayerStream::Writer layerWriter{
                    [&](auto&& msg, auto&& msgType) { content << msg; },
                    fieldOffsets};
                layerWriter.write(tileLayer);
                res.set_content(content.str(), "application/binary");
            }
            else {
                res.set_content(nlohmann::to_string(tileLayer->toJson()), "application/json");
            }
        });

    // Set up GET /info endpoint
    server.Get(
        "/info",
        [this](const httplib::Request&, httplib::Response& res) {
            nlohmann::json j = impl_->info_.toJson();
            res.set_content(j.dump(), "application/json");
        });

    // Set up POST /locate endpoint
    server.Post(
        "/locate",
        [this](const httplib::Request& req, httplib::Response& res) {
            LocateRequest parsedReq(nlohmann::json::parse(req.body));
            auto responseJson = nlohmann::json::array();

            if (impl_->locateCallback_) {
                for (auto const& response : impl_->locateCallback_(parsedReq)) {
                    responseJson.emplace_back(response.serialize());
                }
            }

            res.set_content(responseJson.dump(), "application/json");
        });
}

}  // namespace mapget
