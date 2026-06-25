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

/** Register `/interactive` websocket and `/interactive/payload` long-poll handlers with Drogon. */
void registerTilesWebSocketController(drogon::HttpAppFramework& app, HttpService& service);

/** Build the websocket/long-poll metrics snapshot attached to service status data. */
[[nodiscard]] nlohmann::json tilesWebSocketMetricsSnapshot();

}  // namespace mapget::detail
