#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <iostream>
#include "mapget/log.h"

TEST_CASE("FileLogging", "[Logging]")
{
    auto file_name = "logfile-test.log";

#ifdef _WIN32
    _putenv_s("MAPGET_LOG_LEVEL", "trace");
    _putenv_s("MAPGET_LOG_FILE", file_name);
    _putenv_s("MAPGET_LOG_FILE_MAXSIZE", "100000");
#else
    setenv("MAPGET_LOG_LEVEL", "trace", 1);
    setenv("MAPGET_LOG_FILE", file_name, 1);
    setenv("MAPGET_LOG_FILE_MAXSIZE", "100000", 1);
#endif

    std::filesystem::path test_log_file = std::filesystem::current_path() / file_name;
    std::cout << "Using test log file: " << test_log_file << std::endl;
    auto test_log_size = 0;
    if (std::filesystem::exists(test_log_file)) {
        test_log_size = std::filesystem::file_size(test_log_file);
    }

    mapget::log().trace("Hello from logging test!");
    mapget::log().flush();
    auto new_test_log_size = std::filesystem::file_size(test_log_file);
    REQUIRE(test_log_size < new_test_log_size);

    try {
        std::filesystem::remove(test_log_file);
    } catch(std::filesystem::filesystem_error& e) {
        // Under Windows, we can get an exception that the file is
        // being used by another process - skip deletion, as the
        // test should work without removal (only problematic when log
        // is filled to maxsize, but that takes a lot of test runs).
    }
}
