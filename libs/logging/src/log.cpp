#include <shared_mutex>
#include <iostream>

#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include "log.h"

spdlog::logger& mapget::log()
{
    static std::shared_ptr<spdlog::logger> httpLogger;
    static std::shared_mutex loggerAccess;

    {
        // Check if the logger is already initialized - read-only lock
        std::shared_lock<std::shared_mutex> readLock(loggerAccess);
        if (httpLogger)
            return *httpLogger;
    }

    {
        // Get write lock
        std::lock_guard<std::shared_mutex> readLock(loggerAccess);

        // Check again, another thread might have initialized now
        if (httpLogger)
            return *httpLogger;

        // Initialize the logger
        auto getEnvSafe = [](char const* env){
            auto value = std::getenv(env);
            if (value)
                return std::string(value);
            return std::string();
        };
        std::string logLevel = getEnvSafe("HTTP_LOG_LEVEL");
        std::string logFile = getEnvSafe("HTTP_LOG_FILE");
        std::string logFileMaxSize = getEnvSafe("HTTP_LOG_FILE_MAXSIZE");
        uint64_t logFileMaxSizeInt = 1024ull*1024*1024; // 1GB

        // File logger on demand, otherwise console logger
        if (!logFile.empty()) {
            std::cerr << "Logging OpenAPI HTTP events to '" << logFile << "'!" << std::endl;
            if (!logFileMaxSize.empty()) {
                try {
                    logFileMaxSizeInt = std::stoull(logFileMaxSize);
                }
                catch (std::exception& e) {
                    std::cerr << "Could not parse value of HTTP_LOG_FILE_MAXSIZE." << std::endl;
                }
            }
            std::cerr << "Maximum logfile size is " << logFileMaxSizeInt << " bytes!" << std::endl;
            httpLogger = spdlog::rotating_logger_mt("openapi-http", logFile, logFileMaxSizeInt, 2);
        }
        else
            httpLogger = spdlog::stderr_color_mt("openapi-http");

        // Parse/set log level
        for (auto& ch : logLevel)
            ch = std::tolower(ch);
        if (logLevel == "error" || logLevel == "err")
            httpLogger->set_level(spdlog::level::err);
        else if (logLevel == "warning" || logLevel == "warn")
            httpLogger->set_level(spdlog::level::warn);
        else if (logLevel == "info")
            httpLogger->set_level(spdlog::level::info);
        else if (logLevel == "debug" || logLevel == "dbg")
            httpLogger->set_level(spdlog::level::debug);
        else if (logLevel == "trace")
            httpLogger->set_level(spdlog::level::trace);
    }

    return *httpLogger;
}
