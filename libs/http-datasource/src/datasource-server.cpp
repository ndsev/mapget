#include "datasource-server.h"
#include "mapget/detail/http-server.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/stream.h"

#include "httplib.h"
#include <utility>

namespace mapget {

struct DataSourceServer::Impl
{
    DataSourceInfo info_;
    std::function<void(TileFeatureLayer::Ptr)> tileCallback_;
    std::shared_ptr<Fields> fields_;

    explicit Impl(DataSourceInfo info)
        : info_(std::move(info)), fields_(std::make_shared<Fields>(info_.nodeId_))
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
DataSourceServer::onTileRequest(std::function<void(TileFeatureLayer::Ptr)> const& callback) {
    impl_->tileCallback_ = callback;
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
            auto fieldsOffsetParam = (simfil::FieldId)0;
            if (req.has_param("fieldsOffset"))
                fieldsOffsetParam = (simfil::FieldId)
                    std::stoul(req.get_param_value("fieldsOffset"));

            std::string responseType = "binary";
            if (req.has_param("responseType"))
                responseType = req.get_param_value("responseType");

            // Create response TileFeatureLayer.
            auto tileFeatureLayer = std::make_shared<TileFeatureLayer>(
                tileIdParam,
                impl_->info_.nodeId_,
                impl_->info_.mapId_,
                layer,
                impl_->fields_);
            if (impl_->tileCallback_)
                impl_->tileCallback_(tileFeatureLayer);

            // Serialize TileFeatureLayer using TileLayerStream.
            if (responseType == "binary") {
                std::stringstream content;
                TileLayerStream::FieldOffsetMap fieldOffsets{
                    {impl_->info_.nodeId_, fieldsOffsetParam}};
                TileLayerStream::Writer layerWriter{
                    [&](auto&& msg, auto&& msgType) { content << msg; },
                    fieldOffsets};
                layerWriter.write(tileFeatureLayer);
                res.set_content(content.str(), "application/binary");
            }
            else {
                res.set_content(nlohmann::to_string(tileFeatureLayer->toGeoJson()), "application/json");
            }
        });

    // Set up GET /info endpoint
    server.Get(
        "/info",
        [this](const httplib::Request&, httplib::Response& res) {
            nlohmann::json j = impl_->info_.toJson();
            res.set_content(j.dump(), "application/json");
        });
}

}  // namespace mapget
