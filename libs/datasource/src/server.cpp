#include "httplib.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/stream.h"

#include "server.h"

#include <utility>
#include <csignal>
#include <atomic>

namespace mapget
{

static std::atomic<DataSourceServer*> activeDataSource;

struct DataSourceServer::Impl
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

    static void handleSignal(int signal)
    {
        // Temporarily holds the current active data source
        auto* expected = activeDataSource.load();

        // Stop the active instance when a signal is received.
        // We use compare_exchange_strong to make the operation atomic.
        if (activeDataSource.compare_exchange_strong(expected, nullptr)) {
            if (expected) {
                expected->stop();
            }
        }
    }
};

DataSourceServer::DataSourceServer(DataSourceInfo const& info)
    : impl_(new Impl(info))
{
}

DataSourceServer&
DataSourceServer::onTileRequest(std::function<void(TileFeatureLayer&)> const& callback)
{
    impl_->tileCallback_ = callback;
    return *this;
}

void DataSourceServer::go(std::string const& interfaceAddr, uint16_t port, uint32_t waitMs)
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
        [this, interfaceAddr]
        {
            std::cout << "====== Running on port " << impl_->port_ << " ======" << std::endl;
            impl_->server_.listen_after_bind();
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
    if (!impl_->server_.is_running() || !impl_->server_.is_valid())
        throw std::runtime_error(stx::format("Could not start DataSource on {}:{}", interfaceAddr, port));
}

bool DataSourceServer::isRunning()
{
    return impl_->server_.is_running();
}

void DataSourceServer::stop()
{
    if (!impl_->server_.is_running())
        return;

    impl_->server_.stop();
    impl_->serverThread_.join();
}

uint16_t DataSourceServer::port() const
{
    return impl_->port_;
}

DataSourceInfo const& DataSourceServer::info()
{
    return impl_->info_;
}

DataSourceServer::~DataSourceServer()
{
    if (isRunning())
        stop();
}

void DataSourceServer::waitForSignal()
{
    // So the signal handler knows what to call
    activeDataSource = this;

    // Set the signal handler for SIGINT and SIGTERM.
    std::signal(SIGINT, Impl::handleSignal);
    std::signal(SIGTERM, Impl::handleSignal);

    // Wait for the signal handler to stop us, or the server to shut down on its own.
    while (isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds (200));
    }

    activeDataSource = nullptr;
}

}  // namespace mapget
