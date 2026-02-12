#pragma once

#include "nlohmann/json_fwd.hpp"

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
[[nodiscard]] nlohmann::json tilesWebSocketMetricsSnapshot();

}  // namespace mapget::detail
