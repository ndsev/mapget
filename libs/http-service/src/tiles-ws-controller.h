#pragma once

namespace drogon
{
class HttpAppFramework;
}

namespace mapget
{
class HttpService;
}

namespace mapget::detail
{

void registerTilesWebSocketController(drogon::HttpAppFramework& app, HttpService& service);

}  // namespace mapget::detail

