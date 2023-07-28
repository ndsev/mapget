#include "cli.h"
#include "http-service.h"
#include "http-client.h"
#include "log.h"

#include "mapget/http-datasource/datasource-client.h"

#include <CLI/CLI.hpp>
#include <vector>
#include <string>

namespace mapget
{

struct ServeCommand
{
    int port_ = 0;
    std::vector<std::string> datasourceHosts_;
    std::vector<std::string> datasourceExecutables_;
    std::string webapp_;

    explicit ServeCommand(CLI::App& app) {
        auto serveCmd = app.add_subcommand("serve", "Starts the server.");
        serveCmd->add_option("-p,--port", port_, "Port to start the server on. Default is 0.")->default_val("0");
        serveCmd->add_option("-d,--datasource-host", datasourceHosts_, "Datasources for the server in format <host:port>. Can be specified multiple times.");
        serveCmd->add_option("-e,--datasource-exe", datasourceExecutables_, "Datasource executable paths, including arguments, for the server. Can be specified multiple times.");
        serveCmd->add_option("-w,--webapp", webapp_, "Serve a static web application, in the format [<url-scope>:]<filesystem-path>.");
        serveCmd->callback([this]() { serve(); });
    }

    void serve()
    {
        spdlog::info("Starting server on port {}.", port_);
        HttpService srv;

        if (!datasourceHosts_.empty()){
            for (auto& ds : datasourceHosts_){
                auto delimiterPos = ds.find(':');
                std::string dsHost = ds.substr(0, delimiterPos);
                int dsPort = std::stoi(ds.substr(delimiterPos+1, ds.size()));
                spdlog::info("Connecting to datasource at {}:{}.", dsHost, dsPort);
                try {
                    srv.add(std::make_shared<RemoteDataSource>(dsHost, dsPort));
                }
                catch(std::exception const& e) {
                    spdlog::error("  ...failed: {}", e.what());
                }
            }
        }

        if (!datasourceExecutables_.empty()){
            for (auto& ds : datasourceExecutables_){
                spdlog::info("Launching datasource exe: {}", ds);
                try {
                    srv.add(std::make_shared<RemoteDataSourceProcess>(ds));
                }
                catch(std::exception const& e) {
                    spdlog::error("  ...failed: {}", e.what());
                }
            }
        }

        if (!webapp_.empty()){
            spdlog::info("Webapp: {}", webapp_);
            if (!srv.mountFileSystem(webapp_)){
                spdlog::error("  ...failed to mount!");
                exit(1);
            }
        }
        srv.go("0.0.0.0", port_);
        srv.waitForSignal();
    }
};

struct FetchCommand
{
    std::string server_, map_, layer_;
    std::vector<int> tiles_;

    explicit FetchCommand(CLI::App& app) {
        auto fetchCmd = app.add_subcommand("fetch", "Connects to the server to fetch tiles.");
        fetchCmd->add_option("-s,--server", server_, "Server to connect to in format <host:port>.")->required();
        fetchCmd->add_option("-m,--map", map_, "Map to retrieve.")->required();
        fetchCmd->add_option("-l,--layer", layer_, "Layer of the map to retrieve.")->required();
        fetchCmd->add_option("-t,--tile", tiles_, "Tile of the map to retrieve. Can be specified multiple times.")->required();
        fetchCmd->callback([this]() { fetch(); });
    }

    void fetch() {
        if (spdlog::get_level() <= spdlog::level::debug) {
            // Skips building the tile list string if it will not be logged.
            std::string tileList = "";
            for (auto& tile : tiles_) {
                tileList += std::to_string(tile) + " ";
            }
            spdlog::debug(
                "Connecting client to server {} for map {} and layer {} with tiles:",
                server_,
                map_,
                layer_);
        }

        auto delimiterPos = server_.find(':');
        std::string host = server_.substr(0, delimiterPos);
        int port = std::stoi(server_.substr(delimiterPos+1, server_.size()));

        mapget::HttpClient cli(host, port);
        auto request = std::make_shared<Request>(
            map_, layer_, std::vector<TileId>{tiles_.begin(), tiles_.end()},
            [](auto&& tile){
                std::cout << tile->toGeoJson() << std::endl;
            });
        cli.request(request)->wait();
    }
};

int runFromCommandLine(std::vector<std::string> args)
{
    CLI::App app{"A client/server application for map data retrieval."};
    app.require_subcommand(1); // Require at least one subcommand

    ServeCommand serveCommand(app);
    FetchCommand fetchCommand(app);

    try {
        std::reverse(args.begin(), args.end());
        app.parse(std::move(args));
    } catch(const CLI::ParseError &e) {
        return app.exit(e);
    }
    return 0;
}

}
