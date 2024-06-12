#pragma once

#include "spdlog/spdlog.h"
#include "simfil/exception-handler.h"

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
 * Log an exception and throw it via simfil::raise().
 */
template<typename ExceptionType=std::runtime_error, typename... Args>
[[noreturn]] void raise(Args&&... args)
{
    ExceptionType exceptionInstance(std::forward<Args>(args)...);
    if constexpr (requires {exceptionInstance.what();})
        log().error(exceptionInstance.what());
    simfil::raise<ExceptionType>(exceptionInstance);
}

}