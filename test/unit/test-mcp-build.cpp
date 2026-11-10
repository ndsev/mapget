#include "test-http-service-fixture.h"

#include "mapget/http-service/mcp-config.h"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <CLI/CLI.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

using namespace mapget;

TEST_CASE("MCP CLI options follow the compiled capability", "[mcp-build]")
{
    CLI::App serve;
    McpConfig config;
    config.addOptions(serve);
    CHECK((serve.get_option_no_throw("--mcp") != nullptr) == static_cast<bool>(MAPGET_WITH_MCP));
    CHECK(
        (serve.get_option_no_throw("--mcp-endpoint") != nullptr) ==
        static_cast<bool>(MAPGET_WITH_MCP));
    CHECK_NOTHROW(config.validate());
}

#if !MAPGET_WITH_MCP
TEST_CASE("Disabled MCP cannot be enabled by an HttpService embedder", "[mcp-build]")
{
    for (auto mode : {McpConfig::Mode::Local, McpConfig::Mode::OAuth}) {
        HttpServiceConfig config;
        config.watchConfig = false;
        config.locationLookupEnabled = false;
        config.workerCount = 1;
        config.mcp.mode = mode;
        CHECK_THROWS_WITH(config.mcp.validate(), "MCP support is not included in this build.");
        CHECK_THROWS_WITH(
            config.mcp.resolveDefaults("127.0.0.1", 8089, {}),
            "MCP support is not included in this build.");
        CHECK_THROWS_WITH(
            HttpService(std::make_shared<MemCache>(), config),
            "MCP support is not included in this build.");
    }
}

TEST_CASE(
    "Disabled MCP registers no HTTP endpoints while ordinary APIs remain usable",
    "[mcp-build]")
{
    auto& service = test::httpService();
    auto client =
        drogon::HttpClient::newHttpClient("http://127.0.0.1:" + std::to_string(service.port()));
    for (auto const* path :
         {"/mcp",
          "/mcp/browser",
          "/mcp/info",
          "/.well-known/oauth-protected-resource",
          "/.well-known/oauth-protected-resource/mcp"})
    {
        for (auto method : {drogon::Get, drogon::Post}) {
            INFO(path);
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setPath(path);
            request->setMethod(method);
            auto [status, response] = client->sendRequest(request, 5);
            REQUIRE(status == drogon::ReqResult::Ok);
            CHECK(response->statusCode() == drogon::k404NotFound);
        }
    }
    for (auto const* path : {"/sources", "/status"}) {
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setPath(path);
        auto [status, response] = client->sendRequest(request, 5);
        REQUIRE(status == drogon::ReqResult::Ok);
        CHECK(response->statusCode() == drogon::k200OK);
    }
}
#endif
