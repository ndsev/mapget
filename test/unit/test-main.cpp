#define CATCH_CONFIG_RUNNER

#include <catch2/catch_session.hpp>

#include <mutex>

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
        servicePtr = new HttpService();
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

