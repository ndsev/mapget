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
 * Set the level of the log instance to corresponding string.
 * If the string is empty, set to info.
 * @param logLevel String representation of the log level.
 * @param logInstance spdlog logger instance.
 */
void setLogLevel(std::string logLevel, spdlog::logger& logInstance);

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