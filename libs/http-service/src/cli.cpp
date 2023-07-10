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

void serve(int port, std::vector<std::string> const& datasources, std::string const& webapp)
{
    std::cout << "Starting server on port " << port << "." << std::endl;
    HttpService srv;
    if (!datasources.empty()){
        for (auto& ds : datasources){
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
    if (!webapp.empty()){
        std::cout << "Webapp: " << webapp << std::endl;
        if (!srv.mountFileSystem(webapp)){
            std::cout << "Failed to mount web app " << webapp << "." << std::endl;
            exit(1);
        }
    }
    srv.go("0.0.0.0", port);
    srv.waitForSignal();
}

void fetch(std::string const& server, std::string const& map, std::string const&  layer, std::vector<int> const& tiles)
{
    std::cout << "Connecting client to server " << server << " for map " << map << " and layer " << layer << " with tiles ";
    for(auto& tile : tiles)
        std::cout << tile << " ";
    std::cout << std::endl;

    auto delimiterPos = server.find(':');
    std::string host = server.substr(0, delimiterPos);
    int port = std::stoi(server.substr(delimiterPos+1, server.size()));

    mapget::HttpClient cli(host, port);
    auto request = std::make_shared<Request>(
        map, layer, std::vector<TileId>{tiles.begin(), tiles.end()},
        [](auto&& tile){
            std::cout << tile->toGeoJson() << std::endl;
        });
    cli.request(request)->wait();
}

int runFromCommandLine(std::vector<std::string> args)
{
    CLI::App app{"A client/server application for map data retrieval."};
    app.require_subcommand(1); // Require at least one subcommand

    auto serve_cmd = app.add_subcommand("serve", "Starts the server.");
    int port;
    std::vector<std::string> datasources;
    std::string webapp;
    serve_cmd->add_option("-p,--port", port, "Port to start the server on. Default is 0.")->default_val("0");
    serve_cmd->add_option("-d,--datasource", datasources, "Datasources for the server in format <host:port>. Can be specified multiple times.");
    serve_cmd->add_option("-w,--webapp", webapp, "Serve a static web application, in the format [<url-scope>:]<filesystem-path>.");
    serve_cmd->callback([&]() { serve(port, datasources, webapp); });

    auto fetch_cmd = app.add_subcommand("fetch", "Connects to the server to fetch tiles.");
    std::string server, map, layer;
    std::vector<int> tiles;
    fetch_cmd->add_option("-s,--server", server, "Server to connect to in format <host:port>.")->required();
    fetch_cmd->add_option("-m,--map", map, "Map to retrieve.")->required();
    fetch_cmd->add_option("-l,--layer", layer, "Layer of the map to retrieve.")->required();
    fetch_cmd->add_option("-t,--tile", tiles, "Tile of the map to retrieve. Can be specified multiple times.")->required();
    fetch_cmd->callback([&]() { fetch(server, map, layer, tiles); });

    try {
        std::reverse(args.begin(), args.end());
        app.parse(std::move(args));
    } catch(const CLI::ParseError &e) {
        return app.exit(e);
    }
    return 0;
}

}
