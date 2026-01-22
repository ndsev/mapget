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
    URI "gh:Klebert-Engineering/simfil@0.6.3#v0.6.3"
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
        URI "gh:jbeder/yaml-cpp#aa8d4e@0.8.0" # Use > 0.8.0 once available.
        GIT_SHALLOW OFF
        OPTIONS
            "YAML_CPP_BUILD_TESTS OFF"
            "YAML_CPP_BUILD_TOOLS OFF"
            "YAML_CPP_BUILD_CONTRIB OFF")
    CPMAddPackage("gh:CLIUtils/CLI11@2.5.0")
    CPMAddPackage("gh:pboettch/json-schema-validator#2.3.0")
    CPMAddPackage("gh:okdshin/PicoSHA2@1.0.1")

    if (WIN32)
        CPMAddPackage(
                NAME libuv
                GIT_REPOSITORY https://github.com/libuv/libuv
                GIT_TAG v1.48.0
                GIT_SHALLOW ON
                OPTIONS
                    "LIBUV_BUILD_TESTS OFF"
                    "LIBUV_BUILD_BENCH OFF"
                    "LIBUV_BUILD_SHARED OFF"
                    "LIBUV_BUILD_EXAMPLES OFF")
    endif()

    CPMAddPackage(
            NAME uSockets
            GIT_REPOSITORY https://github.com/uNetworking/uSockets
            GIT_TAG v0.8.5
            GIT_SHALLOW ON
            GIT_SUBMODULES "")
    if (NOT TARGET uSockets)
        file(GLOB_RECURSE U_SOCKETS_SOURCES CONFIGURE_DEPENDS
                "${uSockets_SOURCE_DIR}/src/*.c"
                "${uSockets_SOURCE_DIR}/src/*.cpp")
        add_library(uSockets STATIC ${U_SOCKETS_SOURCES})
        target_include_directories(uSockets PUBLIC "${uSockets_SOURCE_DIR}/src")
        target_compile_definitions(uSockets PRIVATE LIBUS_USE_OPENSSL)
        target_link_libraries(uSockets PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        if (WIN32)
            target_link_libraries(uSockets PUBLIC ws2_32)
            if (TARGET uv_a)
                target_link_libraries(uSockets PUBLIC uv_a)
            elseif (TARGET uv)
                target_link_libraries(uSockets PUBLIC uv)
            else()
                message(FATAL_ERROR "libuv was requested for uSockets on Windows, but no CMake target (uv_a/uv) was found.")
            endif()
        endif()
    endif()

    CPMAddPackage(
            NAME uWebSockets
            GIT_REPOSITORY https://github.com/uNetworking/uWebSockets
            GIT_TAG v20.37.0
            GIT_SHALLOW ON
            GIT_SUBMODULES "")
    if (NOT TARGET uWebSockets)
        add_library(uWebSockets INTERFACE)
        target_include_directories(uWebSockets INTERFACE "${uWebSockets_SOURCE_DIR}/src")
        target_link_libraries(uWebSockets INTERFACE uSockets ZLIB::ZLIB)
        if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(uWebSockets INTERFACE -Wno-deprecated-declarations)
        endif()
    endif()
endif ()

if (MAPGET_WITH_WHEEL AND NOT TARGET pybind11)
    CPMAddPackage("gh:pybind/pybind11@3.0.1")
endif()

if (MAPGET_WITH_SERVICE OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
    CPMAddPackage("gh:ndsev/sqlite-cmake@0.2.4")
    add_sqlite(BACKEND PUBLIC)
endif()

if (MAPGET_WITH_WHEEL AND NOT TARGET python-cmake-wheel)
    CPMAddPackage("gh:Klebert-Engineering/python-cmake-wheel@1.1.0")
endif()

if (MAPGET_ENABLE_TESTING)
    CPMAddPackage(
        URI "gh:catchorg/Catch2@3.5.2"
        OPTIONS
            "CATCH_INSTALL_DOCS OFF"
            "CATCH_INSTALL_EXTRAS OFF")
endif ()
