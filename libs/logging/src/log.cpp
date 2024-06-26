#include <shared_mutex>
#include <iostream>

#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include "mapget/log.h"

spdlog::logger& mapget::log()
{
    static std::shared_ptr<spdlog::logger> logInstance;
    static std::shared_mutex loggerAccess;

    static auto ENVVAR_LOG_LEVEL = "MAPGET_LOG_LEVEL";
    static auto ENVVAR_LOG_FILE = "MAPGET_LOG_FILE";
    static auto ENVVAR_LOG_FILE_MAXSIZE = "MAPGET_LOG_FILE_MAXSIZE";
    static auto LOG_MARKER = "mapget";

    {
        // Check if the logger is already initialized - read-only lock.
        std::shared_lock<std::shared_mutex> readLock(loggerAccess);
        if (logInstance)
            return *logInstance;
    }

    {
        // Get write lock.
        std::lock_guard<std::shared_mutex> readLock(loggerAccess);

        // Check again, another thread might have initialized now.
        if (logInstance)
            return *logInstance;

        // Initialize the logger.
        auto getEnvSafe = [](char const* env){
            auto value = std::getenv(env);
            if (value)
                return std::string(value);
            return std::string();
        };
        std::string logLevel = getEnvSafe(ENVVAR_LOG_LEVEL);
        std::string logFile = getEnvSafe(ENVVAR_LOG_FILE);
        std::string logFileMaxSize = getEnvSafe(ENVVAR_LOG_FILE_MAXSIZE);
        uint64_t logFileMaxSizeInt = 1024ull*1024*1024; // 1GB

        // File logger on demand, otherwise console logger.
        if (!logFile.empty()) {
            std::cout << "Logging mapget events to: " << logFile << std::endl;
            if (!logFileMaxSize.empty()) {
                try {
                    logFileMaxSizeInt = std::stoull(logFileMaxSize);
                }
                catch (std::exception&) {
                    std::cerr << "Could not parse value of "
                              << ENVVAR_LOG_FILE_MAXSIZE << " ." << std::endl;
                }
            }
            std::cout << "Maximum logfile size: " << logFileMaxSizeInt << " bytes" << std::endl;
            logInstance = spdlog::rotating_logger_mt(LOG_MARKER, logFile, logFileMaxSizeInt, 2);
        }
        else
            logInstance = spdlog::stderr_color_mt(LOG_MARKER);

        mapget::setLogLevel(logLevel, *logInstance);
    }

    return *logInstance;
}


void mapget::setLogLevel(std::string logLevel, spdlog::logger& logInstance) {
    for (auto& ch : logLevel)
        ch = std::tolower(ch);
    if (logLevel == "critical")
        logInstance.set_level(spdlog::level::critical);
    else if (logLevel == "error" || logLevel == "err")
        logInstance.set_level(spdlog::level::err);
    else if (logLevel == "warning" || logLevel == "warn")
        logInstance.set_level(spdlog::level::warn);
    else if (logLevel == "info")
        logInstance.set_level(spdlog::level::info);
    else if (logLevel == "debug" || logLevel == "dbg")
        logInstance.set_level(spdlog::level::debug);
    else if (logLevel == "trace") {
        logInstance.set_level(spdlog::level::trace);
        // If you set log to trace and do not get this message,
        // something went wrong with level processing or output.
        logInstance.trace("spdlog level set to [{}].", logLevel);
    }
    else if (logLevel == "") {
        logInstance.set_level(spdlog::level::info);
        logLevel = "info";
    }
    else {
        std::cerr << "Log level not recognized: " << logLevel << std::endl;
        return;
    }
}
