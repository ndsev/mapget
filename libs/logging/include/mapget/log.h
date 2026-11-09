#pragma once

#include "spdlog/spdlog.h"
#include "fmt/core.h"
#include "simfil/exception-handler.h"

namespace mapget
{

/**
 * Obtain the process-wide logger, initialized once on first use from
 * the following environment variables:
 *  - MAPGET_LOG_LEVEL
 *  - MAPGET_LOG_FILE
 *  - MAPGET_LOG_FILE_MAXSIZE (bytes; defaults to 1 GiB)
 */
spdlog::logger& log();

/**
 * Set the level of the log instance to corresponding string.
 * Matching is case-insensitive; an empty string selects info. Unknown values
 * leave the current level unchanged and write a diagnostic to stderr.
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

template <class ExceptionType = std::runtime_error, class... Args>
[[noreturn]] void raiseFmt(std::string_view fmt, Args&&... args)
{
    raise<ExceptionType>(fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));
}

}
