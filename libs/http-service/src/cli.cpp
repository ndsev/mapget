#include "cli.h"
#include "http-service.h"
#include "http-client.h"

#include "mapget/http-datasource/datasource-client.h"

#include <CLI/CLI.hpp>
#include <vector>
#include <string>
#include <iostream>

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
        std::cout << "Starting server on port " << port_ << "." << std::endl;
        HttpService srv;

        if (!datasourceHosts_.empty()){
            for (auto& ds : datasourceHosts_){
                auto delimiterPos = ds.find(':');
                std::string dsHost = ds.substr(0, delimiterPos);
                int dsPort = std::stoi(ds.substr(delimiterPos+1, ds.size()));
                std::cout << "Connecting to datasource on host/port: " << dsHost << " " << dsPort << std::endl;
                try {
                    srv.add(std::make_shared<RemoteDataSource>(dsHost, dsPort));
                }
                catch(std::exception const& e) {
                    std::cout << "  ...failed: " << e.what() << std::endl;
                }
            }
        }

        if (!datasourceExecutables_.empty()){
            for (auto& ds : datasourceExecutables_){
                std::cout << "Launching datasource exe: " << ds << std::endl;
                try {
                    srv.add(std::make_shared<RemoteDataSourceProcess>(ds));
                }
                catch(std::exception const& e) {
                    std::cout << "  ...failed: " << e.what() << std::endl;
                }
            }
        }

        if (!webapp_.empty()){
            std::cout << "Webapp: " << webapp_ << std::endl;
            if (!srv.mountFileSystem(webapp_)){
                std::cout << "Failed to mount web app " << webapp_ << "." << std::endl;
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
    std::vector<uint64_t> tiles_;

    explicit FetchCommand(CLI::App& app) {
        auto fetchCmd = app.add_subcommand("fetch", "Connects to the server to fetch tiles.");
        fetchCmd->add_option("-s,--server", server_, "Server to connect to in format <host:port>.")->required();
        fetchCmd->add_option("-m,--map", map_, "Map to retrieve.")->required();
        fetchCmd->add_option("-l,--layer", layer_, "Layer of the map to retrieve.")->required();
        fetchCmd->add_option("-t,--tile", tiles_, "Tile of the map to retrieve. Can be specified multiple times.")->required();
        fetchCmd->callback([this]() { fetch(); });
    }

    void fetch() {
        std::cout << "Connecting client to server " << server_ << " for map " << map_ << " and layer " << layer_ << " with tiles ";
        for(auto& tile : tiles_)
            std::cout << tile << " ";
        std::cout << std::endl;

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
