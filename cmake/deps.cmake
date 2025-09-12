### Dependencies via CPM (converted from FetchContent)

CPMAddPackage("gh:g-truc/glm#1.0.1")
CPMAddPackage(
  URI "gh:fmtlib/fmt#11.1.3"
  OPTIONS "FMT_HEADER_ONLY OFF")
CPMAddPackage(
  URI "gh:gabime/spdlog@1.15.3"
  OPTIONS "SPDLOG_FMT_EXTERNAL ON")
CPMAddPackage("gh:fraillt/bitsery@5.2.4")
CPMAddPackage("gh:nlohmann/json@3.11.3")
CPMAddPackage(
  URI "gh:TartanLlama/expected@1.1.0"
  OPTIONS
    "EXPECTED_BUILD_TESTS OFF"
    "EXPECTED_BUILD_PACKAGE_DEB OFF")
CPMAddPackage(
  URI "gh:Klebert-Engineering/simfil@0.5.6"
  OPTIONS
    "SIMFIL_WITH_MODEL_JSON ON"
    "SIMFIL_SHARED ON")
CPMAddPackage("gl:eidheim/tiny-process-library#8bbb5a")  # Switch to release > 2.0.4 once available

if (MAPGET_WITH_WHEEL OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
    set (OPENSSL_VERSION openssl-3.5.2)
    CPMAddPackage("gh:klebert-engineering/openssl-cmake@1.0.0")
    CPMAddPackage(
      URI "gh:madler/zlib@1.3.1"
      OPTIONS
        "ZLIB_BUILD_EXAMPLES OFF"
        "BUILD_TESTING OFF")
    CPMAddPackage(
      URI "gh:yhirose/cpp-httplib@0.15.3"
      OPTIONS
        "CPPHTTPLIB_OPENSSL_SUPPORT ON"
        "CPPHTTPLIB_USE_POLL ON"
        "CPPHTTPLIB_ZLIB_SUPPORT ON")
    CPMAddPackage(
      URI "gh:jbeder/yaml-cpp#aa8d4e" # Swicth to release > 0.8.0 once available
      OPTIONS
        "YAML_CPP_BUILD_TESTS OFF"
        "YAML_CPP_BUILD_TOOLS OFF"
        "YAML_CPP_BUILD_CONTRIB OFF")
    CPMAddPackage("gh:CLIUtils/CLI11@2.5.0")
    CPMAddPackage("gh:pboettch/json-schema-validator#2.3.0")
    CPMAddPackage("gh:okdshin/PicoSHA2@1.0.1")
endif ()

if (MAPGET_WITH_WHEEL AND NOT TARGET pybind11)
  CPMAddPackage("gh:pybind/pybind11@2.13.6")
endif()

if ((MAPGET_WITH_SERVICE OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING) AND NOT TARGET SQLite::SQLite3)
  CPMAddPackage("gh:ndsev/sqlite-cmake#fix-populate")
  add_sqlite(BACKEND PUBLIC VERSION 3.50.2)
endif()

if (MAPGET_WITH_WHEEL AND NOT TARGET python-cmake-wheel)
  CPMAddPackage("gh:Klebert-Engineering/python-cmake-wheel@1.0.1")
endif()

if (MAPGET_ENABLE_TESTING)
  CPMAddPackage(
    URI "gh:catchorg/Catch2@3.5.2"
    OPTIONS
      "CATCH_INSTALL_DOCS OFF"
      "CATCH_INSTALL_EXTRAS OFF")
endif ()
