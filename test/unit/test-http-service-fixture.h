#pragma once

#include "mapget/http-service/http-service.h"

namespace mapget::test
{

// Starts the HTTP service lazily (on first use) and keeps it alive for the
// lifetime of the test process. `shutdownHttpService()` stops the server and
// joins its server thread to avoid Drogon shutdown issues.
HttpService& httpService();

// Safe to call even if the service was never started.
void shutdownHttpService();

}  // namespace mapget::test

