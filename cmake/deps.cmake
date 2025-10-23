### Dependencies via CPM (converted from FetchContent)

CPMAddPackage("gh:g-truc/glm#1.0.1")
CPMAddPackage(
  URI "gh:fmtlib/fmt#11.1.4"
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
  URI "gh:Klebert-Engineering/simfil@0.6.0#make-simfil-even-more-exception-free"
  OPTIONS
    "SIMFIL_WITH_MODEL_JSON ON"
    "SIMFIL_SHARED OFF")
CPMAddPackage(
  URI "gl:eidheim/tiny-process-library#8bbb5a"  # Switch to release > 2.0.4 once available
  OPTIONS
    "BUILD_TESTING OFF")

if (MAPGET_WITH_WHEEL OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
    set (OPENSSL_VERSION openssl-3.5.2)
    CPMAddPackage("gh:klebert-engineering/openssl-cmake@1.0.0")
    CPMAddPackage(
      URI "gh:madler/zlib@1.3.1"
      OPTIONS
        "ZLIB_BUILD_EXAMPLES OFF"
        "BUILD_TESTING OFF")
    set_target_properties(zlib PROPERTIES EXCLUDE_FROM_ALL TRUE)
    set_target_properties(zlibstatic PROPERTIES EXCLUDE_FROM_ALL TRUE)
    # Create ZLIB::ZLIB alias if it doesn't exist
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()

    CPMAddPackage(
      URI "gh:yhirose/cpp-httplib@0.15.3"
      OPTIONS
        "CPPHTTPLIB_USE_POLL ON"
        "HTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN OFF"
        "HTTPLIB_INSTALL OFF"
        "HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF"
        "HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF")
    # Manually enable openssl/zlib in httplib to avoid FindPackage calls.
    target_compile_definitions(httplib INTERFACE
      CPPHTTPLIB_OPENSSL_SUPPORT
      CPPHTTPLIB_ZLIB_SUPPORT)
    target_link_libraries(httplib INTERFACE
      OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)

    CPMAddPackage(
      URI "gh:jbeder/yaml-cpp#0.8.0" # Switch to release > 0.8.0 once available
      OPTIONS
        "YAML_CPP_BUILD_TESTS OFF"
        "YAML_CPP_BUILD_TOOLS OFF"
        "YAML_CPP_BUILD_CONTRIB OFF")
    CPMAddPackage("gh:CLIUtils/CLI11@2.5.0")
    CPMAddPackage("gh:pboettch/json-schema-validator#2.3.0")
    CPMAddPackage("gh:okdshin/PicoSHA2@1.0.1")
endif ()

if (MAPGET_WITH_WHEEL AND NOT TARGET pybind11)
  CPMAddPackage("gh:pybind/pybind11@3.0.1")
endif()

if (MAPGET_WITH_SERVICE OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
  CPMAddPackage("gh:ndsev/sqlite-cmake@0.2.4")
  add_sqlite(BACKEND PUBLIC)
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
