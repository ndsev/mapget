#include <iostream>
#include <nlohmann/json.hpp>
#include "mapget/http-service/http-service.h"
#include "mapget/http-datasource/datasource-client.h"
#include "stx/string.h"

using namespace mapget;
using json = nlohmann::json;
namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cout << "Usage: minimal-http-service <port|0> [<remote-data-source-host:port>...]" << std::endl;
        return 1;
    }
    int port = std::stoi(argv[1]);  // Get port from command line argument
    std::cout << "Running on port " << port << std::endl;

    // Instantiate http service
    HttpService httpService;

    // Connect to remote data sources as specified
    for (auto i = 2; i < argc; ++i) {
        auto hostAndPort = stx::split(argv[i], ":");
        if (hostAndPort.size() < 2)
            std::cout << "Expecting host:port, got " << argv[i] << std::endl;
        std::cout << "Adding data source " << argv[i] << std::endl;
        httpService.add(std::make_shared<RemoteDataSource>(hostAndPort[0], std::stoul(hostAndPort[1])));
    }

    // Run the service
    httpService.go("0.0.0.0", port);
    std::cout << "Running... " << std::endl;
    httpService.waitForSignal();
}
