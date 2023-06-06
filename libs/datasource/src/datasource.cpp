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
    bool isRunning_ = false;
    uint16_t port_ = 0;
    std::thread serverThread_;
    std::shared_ptr<Fields> fields_;

    explicit Impl(DataSourceInfo info) : info_(std::move(info))
    {
        fields_ = std::make_shared<Fields>(info_.nodeId_);
    }

    static uint16_t findFreePort()
    {
        // Random device for generating port numbers.
        std::random_device rd;
        std::uniform_int_distribution<uint16_t> dist(49152, 65535);

        // Trial and error until we find a free port.
        while (true)
        {
            uint16_t trialPort = dist(rd);
            httplib::Server trialServer;

            // Start the server in a separate thread
            std::thread serverThread(
                [&trialServer, trialPort]() { trialServer.listen("localhost", trialPort); });

            // Give the server some time to start
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Check if server is running
            if (trialServer.is_running()) {
                trialServer.stop();
                serverThread.join(); // Join the thread after stopping the server.
                return trialPort;
            }

            // If server is not running, join the thread.
            serverThread.join();
        }
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

void DataSource::go(std::string const& interfaceAddr, uint16_t port)
{
    if (impl_->isRunning_)
        throw std::runtime_error("DataSource is already running");

    impl_->server_.Get(
        "/tile",
        [this](const httplib::Request& req, httplib::Response& res)
        {
            auto layerIdParam = req.get_param_value("layer");
            auto layerIt = impl_->info_.layers_.find(layerIdParam);
            if (layerIt == impl_->info_.layers_.end())
                throw std::runtime_error(stx::format("Unknown layer id `{}`!", layerIdParam));

            auto tileIdParam = TileId{std::stoull(req.get_param_value("tileId"))};
            auto fieldsOffsetParam = (simfil::FieldId)0;
            if (req.has_param("fieldsOffset"))
                fieldsOffsetParam = (simfil::FieldId)std::stoul(req.get_param_value("fieldsOffset"));

            auto tileFeatureLayer = std::make_shared<TileFeatureLayer>(
                tileIdParam,
                impl_->info_.nodeId_,
                impl_->info_.mapId_,
                layerIt->second,
                impl_->fields_);
            if (impl_->tileCallback_)
                impl_->tileCallback_(*tileFeatureLayer);

            // Serialize TileFeatureLayer using TileLayerStream
            std::stringstream content;
            TileLayerStream::FieldOffsetMap fieldOffsets{{
                impl_->info_.nodeId_,
                fieldsOffsetParam
            }};
            TileLayerStream::Writer layerWriter{
                [&](auto&& msg) { content << msg; },
                fieldOffsets};
            layerWriter.write(tileFeatureLayer);

            res.set_content(content.str(), "application/binary");
        });

    impl_->server_.Get(
        "/info",
        [this](const httplib::Request&, httplib::Response& res)
        {
            nlohmann::json j = impl_->info_.toJson();
            res.set_content(j.dump(), "application/json");
        });

    if (port == 0)
        port = impl_->findFreePort();

    impl_->serverThread_ = std::thread(
        [this, interfaceAddr, port]
        {
            std::cout << "====== Running on port " << port << " ======" << std::endl;
            impl_->server_.listen(interfaceAddr, port);
        });

    impl_->isRunning_ = true;
    impl_->port_ = port;
}

bool DataSource::isRunning()
{
    return impl_->isRunning_;
}

void DataSource::stop()
{
    if (!impl_->isRunning_)
        return;

    impl_->server_.stop();
    impl_->isRunning_ = false;
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
