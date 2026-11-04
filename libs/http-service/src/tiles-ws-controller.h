#pragma once

#include <memory>
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

class McpServer;

/** Register `/interactive`, its legacy `/tiles` websocket alias, and `/interactive/payload`. */
void registerTilesWebSocketController(
    drogon::HttpAppFramework& app,
    HttpService& service,
    std::weak_ptr<McpServer> mcp = {});

/** Build the websocket/long-poll metrics snapshot attached to service status data. */
[[nodiscard]] nlohmann::json tilesWebSocketMetricsSnapshot();

}  // namespace mapget::detail
