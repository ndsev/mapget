#include "httplib.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/stream.h"

#include "datasource.h"

#include <utility>

namespace mapget
{

struct DataSource::Impl
{
    httplib::Server server_;
    DataSourceInfo info_;
    std::function<void(TileFeatureLayer&)> tileCallback_;
    uint16_t port_ = 0;
    std::thread serverThread_;
    std::shared_ptr<Fields> fields_;

    explicit Impl(DataSourceInfo info) : info_(std::move(info)), fields_(std::make_shared<Fields>(info_.nodeId_))
    {
        // Set up GET /tile endpoint
        server_.Get(
            "/tile",
            [this](const httplib::Request& req, httplib::Response& res)
            {
                auto layerIdParam = req.get_param_value("layer");
                auto layerIt = info_.layers_.find(layerIdParam);
                if (layerIt == info_.layers_.end())
                    throw std::runtime_error(stx::format("Unknown layer id `{}`!", layerIdParam));

                auto tileIdParam = TileId{std::stoull(req.get_param_value("tileId"))};
                auto fieldsOffsetParam = (simfil::FieldId)0;
                if (req.has_param("fieldsOffset"))
                    fieldsOffsetParam = (simfil::FieldId)
                        std::stoul(req.get_param_value("fieldsOffset"));

                auto tileFeatureLayer = std::make_shared<TileFeatureLayer>(
                    tileIdParam,
                    info_.nodeId_,
                    info_.mapId_,
                    layerIt->second,
                    fields_);
                if (tileCallback_)
                    tileCallback_(*tileFeatureLayer);

                // Serialize TileFeatureLayer using TileLayerStream
                std::stringstream content;
                TileLayerStream::FieldOffsetMap fieldOffsets{{info_.nodeId_, fieldsOffsetParam}};
                TileLayerStream::Writer layerWriter{
                    [&](auto&& msg) { content << msg; },
                    fieldOffsets};
                layerWriter.write(tileFeatureLayer);

                res.set_content(content.str(), "application/binary");
            });

        // Set up GET /info endpoint
        server_.Get(
            "/info",
            [this](const httplib::Request&, httplib::Response& res)
            {
                nlohmann::json j = info_.toJson();
                res.set_content(j.dump(), "application/json");
            });
    }
};

DataSource::DataSource(DataSourceInfo const& info)
    : impl_(new Impl(info))
{
}

DataSource& DataSource::onTileRequest(std::function<void(TileFeatureLayer&)> const& callback)
{
    impl_->tileCallback_ = callback;
    return *this;
}

void DataSource::go(std::string const& interfaceAddr, uint16_t port, uint32_t waitMs)
{
    if (impl_->server_.is_running() || impl_->serverThread_.joinable())
        throw std::runtime_error("DataSource is already running");

    if (port == 0) {
        impl_->port_ = impl_->server_.bind_to_any_port(interfaceAddr);
    }
    else {
        impl_->port_ = port;
        impl_->server_.bind_to_port(interfaceAddr, port);
    }

    impl_->serverThread_ = std::thread(
        [this, interfaceAddr, port]
        {
            std::cout << "====== Running on port " << impl_->port_ << " ======" << std::endl;
            impl_->server_.listen_after_bind();
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
    if (!impl_->server_.is_running() || !impl_->server_.is_valid())
        throw std::runtime_error(stx::format("Could not start DataSource on {}:{}", interfaceAddr, port));
}

bool DataSource::isRunning()
{
    return impl_->server_.is_running();
}

void DataSource::stop()
{
    if (!impl_->server_.is_running())
        return;

    impl_->server_.stop();
    impl_->serverThread_.join();
}

uint16_t DataSource::port() const
{
    return impl_->port_;
}

DataSourceInfo const& DataSource::info()
{
    return impl_->info_;
}

DataSource::~DataSource()
{
    if (isRunning())
        stop();
}

}  // namespace mapget
