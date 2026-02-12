// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "httplib.h"
#include "mapget/service/cache.h"

namespace mapget
{

/**
 * Set up the Developer UI routes on the given HTTP server.
 * Routes:
 *   GET  /dev/              - Serves the Developer UI HTML page
 *   GET  /dev/gridsource/config - Returns current GridDataSource config as JSON
 *   POST /dev/gridsource/config - Partial config update, clears caches
 *   POST /dev/cache/clear       - Clears the tile cache
 */
void setupDevUI(httplib::Server& server, Cache::Ptr cache);

}  // namespace mapget
