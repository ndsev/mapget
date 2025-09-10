### Dependencies via CPM (converted from FetchContent)

CPMAddPackage(
  URI "gh:madler/zlib@1.3.1"
  OPTIONS
    "ZLIB_BUILD_EXAMPLES OFF"
    "BUILD_TESTING OFF")
# Create the ZLIB::ZLIB alias that CMake's FindZLIB would create
add_library(ZLIB::ZLIB ALIAS zlibstatic)

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
  URI "gh:Klebert-Engineering/simfil@0.5.4"
  OPTIONS
    "SIMFIL_WITH_MODEL_JSON ON"
    "SIMFIL_SHARED ON")
CPMAddPackage(
  NAME tiny-process-library
  GIT_REPOSITORY "https://gitlab.com/eidheim/tiny-process-library"
  GIT_TAG "v2.0.4")

if (MAPGET_WITH_WHEEL OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
    set (OPENSSL_VERSION openssl-3.5.2)
    CPMAddPackage("gh:klebert-engineering/openssl-cmake@1.0.0")
    CPMAddPackage(
      URI "gh:yhirose/cpp-httplib@0.15.3"
      OPTIONS
        "CPPHTTPLIB_OPENSSL_SUPPORT ON"
        "CPPHTTPLIB_USE_POLL ON"
        "CPPHTTPLIB_ZLIB_SUPPORT ON")
    CPMAddPackage(
      URI "gh:jbeder/yaml-cpp#0.8.0"
      OPTIONS
        "YAML_CPP_BUILD_TESTS OFF"
        "YAML_CPP_BUILD_TOOLS OFF"
        "YAML_CPP_BUILD_CONTRIB OFF")
    CPMAddPackage("gh:CLIUtils/CLI11@2.3.2")
    CPMAddPackage("gh:pboettch/json-schema-validator#2.3.0")
    CPMAddPackage("gh:okdshin/PicoSHA2@1.0.1")
endif ()

if (MAPGET_WITH_WHEEL AND NOT TARGET pybind11)
  CPMAddPackage("gh:pybind/pybind11@2.13.6")
endif()

if ((MAPGET_WITH_SERVICE OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING) AND NOT TARGET SQLite::SQLite3)
  CPMAddPackage(
    NAME sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
    URL_HASH SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
    DOWNLOAD_ONLY YES
  )
  # Create a library for SQLite3
  add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
  target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
  target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_RTREE=1
    SQLITE_ENABLE_FTS5=1
    SQLITE_ENABLE_JSON1=1
    SQLITE_ENABLE_COLUMN_METADATA=1
    SQLITE_THREADSAFE=1
  )
  # Create alias target
  add_library(SQLite::SQLite3 ALIAS sqlite3)
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
