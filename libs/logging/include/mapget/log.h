#pragma once

#include "spdlog/spdlog.h"

namespace mapget
{

/**
 * Obtain global logger, which is initialized from
 * the following environment variables:
 *  - MAPGET_LOG_LEVEL
 *  - MAPGET_LOG_FILE
 *  - MAPGET_LOG_FILE_MAXSIZE
 */
spdlog::logger& log();

/**
 * Log a runtime error and return the throwable object.
 * @param what Runtime error message.
 * @return std::runtime_error to throw.
 */
template <typename error_t = std::runtime_error>
error_t logRuntimeError(std::string const& what)
{
    log().error(what);
    return error_t(what);
}

}