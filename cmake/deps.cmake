### Dependencies via CPM (converted from FetchContent)

CPMAddPackage("gh:g-truc/glm#1.0.1")
set(MAPGET_NDSMATH_SOURCE_DIR "" CACHE PATH
    "Local ndslive-math source directory to use instead of fetching from Git.")
set(_mapget_ndsmath_source_dir "${MAPGET_NDSMATH_SOURCE_DIR}")
if ("${_mapget_ndsmath_source_dir}" STREQUAL ""
    AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../ndslive-math/cpp/CMakeLists.txt")
    set(_mapget_ndsmath_source_dir "${CMAKE_CURRENT_LIST_DIR}/../../ndslive-math/cpp")
endif()

# Link ndsmath statically into mapget targets. The C++ API is tiny, and static
# linkage avoids adding another platform-specific runtime library to Python wheels.
set(_mapget_restore_build_shared_libs OFF)
if (DEFINED BUILD_SHARED_LIBS)
    set(_mapget_restore_build_shared_libs ON)
    set(_mapget_previous_build_shared_libs "${BUILD_SHARED_LIBS}")
endif()
set(BUILD_SHARED_LIBS OFF)

if (NOT "${_mapget_ndsmath_source_dir}" STREQUAL "")
    message(STATUS "Using local ndsmath from ${_mapget_ndsmath_source_dir}")
    CPMAddPackage(
        NAME ndsmath
        SOURCE_DIR "${_mapget_ndsmath_source_dir}"
        OPTIONS
            "NDSMATH_BUILD_TESTS OFF"
            "NDSMATH_INSTALL OFF")
else()
    CPMAddPackage(
        NAME ndsmath
        GITHUB_REPOSITORY ndsev/ndslive-math
        GIT_TAG 009bba06356dfd01f98c1570a003e31180788811
        GIT_SHALLOW FALSE
        SOURCE_SUBDIR cpp
        OPTIONS
            "NDSMATH_BUILD_TESTS OFF"
            "NDSMATH_INSTALL OFF")
endif()

if (_mapget_restore_build_shared_libs)
    set(BUILD_SHARED_LIBS "${_mapget_previous_build_shared_libs}")
else()
    unset(BUILD_SHARED_LIBS)
endif()
unset(_mapget_previous_build_shared_libs)
unset(_mapget_restore_build_shared_libs)

if (TARGET ndsmath AND NOT TARGET ndsmath::ndsmath)
    add_library(ndsmath::ndsmath ALIAS ndsmath)
endif()

CPMAddPackage(
    URI "gh:fmtlib/fmt#11.1.4"
    OPTIONS "FMT_HEADER_ONLY OFF")
CPMAddPackage(
    URI "gh:gabime/spdlog@1.15.3"
    OPTIONS "SPDLOG_FMT_EXTERNAL ON")
CPMAddPackage("gh:fraillt/bitsery@5.2.4")
CPMAddPackage("gh:nlohmann/json@3.11.3")
CPMAddPackage("gh:pboettch/json-schema-validator#2.3.0")
CPMAddPackage(
    URI "gh:TartanLlama/expected@1.1.0"
    OPTIONS
        "EXPECTED_BUILD_TESTS OFF"
        "EXPECTED_BUILD_PACKAGE_DEB OFF")

set(MAPGET_SIMFIL_SOURCE_DIR "" CACHE PATH
    "Local simfil source directory to use instead of fetching from Git.")

set(_mapget_simfil_source_dir "${MAPGET_SIMFIL_SOURCE_DIR}")
if ("${_mapget_simfil_source_dir}" STREQUAL ""
    AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../simfil/CMakeLists.txt")
    set(_mapget_simfil_source_dir "${CMAKE_CURRENT_LIST_DIR}/../../simfil")
endif()

if (NOT "${_mapget_simfil_source_dir}" STREQUAL "")
    message(STATUS "Using local simfil from ${_mapget_simfil_source_dir}")
    CPMAddPackage(
        NAME simfil
        SOURCE_DIR "${_mapget_simfil_source_dir}"
        OPTIONS
            "SIMFIL_WITH_MODEL_JSON ON"
            "SIMFIL_SHARED OFF")
else()
    CPMAddPackage(
        NAME simfil
        GITHUB_REPOSITORY Klebert-Engineering/simfil
        GIT_TAG release/1.1.2
        GIT_SHALLOW FALSE
        OPTIONS
            "SIMFIL_WITH_MODEL_JSON ON"
            "SIMFIL_SHARED OFF")
endif()

# Switch to a release newer than 2.0.4 once available.
CPMAddPackage(
    NAME tiny-process-library
    URL "https://gitlab.com/eidheim/tiny-process-library/-/archive/8bbb5a211c5c9df8ee69301da9d22fb977b27dc1/tiny-process-library-8bbb5a211c5c9df8ee69301da9d22fb977b27dc1.tar.gz"
    URL_HASH SHA256=0ff45f47da95f1c36ce5e1e91342285301bf339150b34c3c5964687117ef01e5
    OPTIONS
        "BUILD_TESTING OFF")

if (MAPGET_WITH_WHEEL OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
    # OpenSSL's Configure script needs a "full" Perl distribution. Git for
    # Windows ships a minimal perl that is missing required modules (e.g.
    # Locale::Maketext::Simple), causing OpenSSL builds to fail.
    if (WIN32)
        if (NOT DEFINED PERL_EXECUTABLE OR PERL_EXECUTABLE MATCHES "[\\\\/]Git[\\\\/]usr[\\\\/]bin[\\\\/]perl\\.exe$")
            find_program(_MAPGET_STRAWBERRY_PERL
                NAMES perl.exe
                PATHS "C:/Strawberry/perl/bin"
                NO_DEFAULT_PATH)
            if (_MAPGET_STRAWBERRY_PERL)
                set(PERL_EXECUTABLE "${_MAPGET_STRAWBERRY_PERL}" CACHE FILEPATH "" FORCE)
            endif()
        endif()
    endif()

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
        NAME jsoncpp
        GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp
        GIT_TAG 1.9.5
        GIT_SHALLOW ON
        OPTIONS
            "JSONCPP_WITH_TESTS OFF"
            "JSONCPP_WITH_POST_BUILD_UNITTEST OFF"
            "JSONCPP_WITH_PKGCONFIG_SUPPORT OFF"
            "JSONCPP_WITH_CMAKE_PACKAGE OFF"
            "BUILD_SHARED_LIBS OFF"
            "BUILD_STATIC_LIBS ON"
            "BUILD_OBJECT_LIBS OFF")
    # Help Drogon's FindJsoncpp.cmake locate jsoncpp when built via CPM.
    set(JSONCPP_INCLUDE_DIRS "${jsoncpp_SOURCE_DIR}/include" CACHE PATH "" FORCE)
    set(JSONCPP_LIBRARIES jsoncpp_static CACHE STRING "" FORCE)
    # CPM generates a dummy package redirect config at
    # `${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/jsoncpp-config.cmake`. Drogon uses
    # `find_package(Jsoncpp)` (config-first), so make that redirect actually
    # define the expected `Jsoncpp_lib` target.
    if (DEFINED CMAKE_FIND_PACKAGE_REDIRECTS_DIR)
        file(MAKE_DIRECTORY "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}")
        file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/jsoncpp-extra.cmake" [=[
if(NOT TARGET Jsoncpp_lib)
  add_library(Jsoncpp_lib INTERFACE)
  target_include_directories(Jsoncpp_lib INTERFACE "${JSONCPP_INCLUDE_DIRS}")
  target_link_libraries(Jsoncpp_lib INTERFACE ${JSONCPP_LIBRARIES})
endif()
]=])
    endif()

    # Drogon defines install(EXPORT ...) rules unconditionally, which fail when
    # used as a subproject with CPM-provided dependencies (zlib/jsoncpp/etc).
    # Since mapget only needs Drogon for building, temporarily suppress install
    # rule generation while configuring Drogon.
    set(_MAPGET_PREV_SKIP_INSTALL_RULES "${CMAKE_SKIP_INSTALL_RULES}")
    if (DEFINED BUILD_TESTING)
        set(_MAPGET_PREV_BUILD_TESTING "${BUILD_TESTING}")
    endif()
    set(CMAKE_SKIP_INSTALL_RULES ON)
    set(BUILD_TESTING OFF)

    CPMAddPackage(
        URI "gh:drogonframework/drogon@1.9.7"
        OPTIONS
            "BUILD_CTL OFF"
            "BUILD_EXAMPLES OFF"
            "BUILD_ORM OFF"
            "BUILD_BROTLI OFF"
            "BUILD_YAML_CONFIG OFF"
            "BUILD_SHARED_LIBS OFF"
            "USE_SUBMODULE ON"
            "USE_STATIC_LIBS_ONLY OFF"
            "USE_POSTGRESQL OFF"
            "USE_MYSQL OFF"
            "USE_SQLITE3 OFF"
        GIT_SUBMODULES "trantor")

    set(CMAKE_SKIP_INSTALL_RULES "${_MAPGET_PREV_SKIP_INSTALL_RULES}")
    if (DEFINED _MAPGET_PREV_BUILD_TESTING)
        set(BUILD_TESTING "${_MAPGET_PREV_BUILD_TESTING}")
    endif()

    CPMAddPackage(
        URI "gh:jbeder/yaml-cpp#yaml-cpp-0.9.0@0.9.0"
        GIT_SHALLOW OFF
        OPTIONS
            "YAML_CPP_BUILD_TESTS OFF"
            "YAML_CPP_BUILD_TOOLS OFF"
            "YAML_CPP_BUILD_CONTRIB OFF")
    CPMAddPackage("gh:CLIUtils/CLI11@2.5.0")
    CPMAddPackage("gh:okdshin/PicoSHA2@1.0.1")

endif ()

if (MAPGET_WITH_WHEEL AND NOT TARGET pybind11)
    CPMAddPackage("gh:pybind/pybind11@3.0.1")
endif()

if (MAPGET_WITH_SERVICE OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
    CPMAddPackage("gh:ndsev/sqlite-cmake@0.2.4")
    add_sqlite(
        BACKEND PUBLIC
        ENABLE_JSON1 ON
        ENABLE_FTS5 ON
        ENABLE_RTREE ON
        ENABLE_MATH ON
        ENABLE_COLUMN_METADATA ON)
endif()

if (MAPGET_WITH_WHEEL AND NOT TARGET python-cmake-wheel)
    CPMAddPackage("gh:Klebert-Engineering/python-cmake-wheel#v1.2.8@1.2.8")
endif()

if (MAPGET_ENABLE_TESTING)
    CPMAddPackage(
        URI "gh:catchorg/Catch2@3.5.2"
        OPTIONS
            "CATCH_INSTALL_DOCS OFF"
            "CATCH_INSTALL_EXTRAS OFF")
endif ()
