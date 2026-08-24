#define CATCH_CONFIG_RUNNER

#include <catch2/catch_session.hpp>

#include <mutex>
#include <regex>
#include <stdexcept>

#include "mapget/log.h"
#include "test-http-service-fixture.h"

namespace mapget::test
{
namespace
{

std::mutex serviceMutex;
HttpService* servicePtr = nullptr;

}  // namespace

HttpService& httpService()
{
    std::lock_guard<std::mutex> lock(serviceMutex);

    if (!servicePtr) {
        // Intentionally leaked to avoid destructor ordering issues at process shutdown.
        HttpServiceConfig config;
        config.cacheResetEnabled = true;
        config.cacheResetAuthHeaderAlternatives.emplace(
            "x-cache-role",
            std::regex("^resetter$"));
        config.cacheResetAuthHeaderAlternatives.emplace(
            "x-cache-group",
            std::regex("^operators$"));
        servicePtr = new HttpService(
            std::make_shared<MemCache>(),
            config);
        if (!servicePtr->mountFileSystem(MAPGET_TEST_DATA_DIR "/startup-static"))
            throw std::runtime_error("Failed to create the startup static mount used by HTTP tests");
        servicePtr->go("127.0.0.1", 0, 5000);
    }

    return *servicePtr;
}

void shutdownHttpService()
{
    std::lock_guard<std::mutex> lock(serviceMutex);
    if (!servicePtr)
        return;

    servicePtr->stop();
}

}  // namespace mapget::test

int main(int argc, char* argv[])
{
    auto result = Catch::Session().run(argc, argv);
    mapget::test::shutdownHttpService();
    return result;
}
