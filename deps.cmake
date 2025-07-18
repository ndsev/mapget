include(FetchContent)



if (NOT TARGET glm::glm)
  FetchContent_Declare(glm
    GIT_REPOSITORY "https://github.com/g-truc/glm.git"
    GIT_TAG        "1.0.1"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(glm)
endif()

if (NOT TARGET fmt::fmt)
  FetchContent_Declare(fmt
    GIT_REPOSITORY "https://github.com/fmtlib/fmt.git"
    GIT_TAG        "11.1.3"
    GIT_SHALLOW    ON
    CMAKE_ARGS     -DFMT_HEADER_ONLY=OFF)
  FetchContent_MakeAvailable(fmt)
endif()

if (NOT TARGET spdlog::spdlog)
  set (SPDLOG_FMT_EXTERNAL ON)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY "https://github.com/gabime/spdlog.git"
    GIT_TAG        "v1.x"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(spdlog)
endif()

if (NOT TARGET Bitsery::bitsery)
  FetchContent_Declare(bitsery
    GIT_REPOSITORY "https://github.com/fraillt/bitsery.git"
    GIT_TAG        "v5.2.4"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(bitsery)
endif()

if (NOT TARGET httplib::httplib)
  FetchContent_Declare(cpp-httplib
    GIT_REPOSITORY "https://github.com/yhirose/cpp-httplib.git"
    GIT_TAG        "v0.15.3"
    GIT_SHALLOW    ON)
endif()

if (NOT TARGET yaml-cpp::yaml-cpp)
  FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY "https://github.com/jbeder/yaml-cpp.git"
    GIT_TAG        "0.8.0"
    GIT_SHALLOW    ON)
endif()

if (NOT TARGET CLI11::CLI11)
  FetchContent_Declare(cli11
    GIT_REPOSITORY "https://github.com/CLIUtils/CLI11"
    GIT_TAG        "v2.3.2"
    GIT_SHALLOW    ON)
endif()

if (NOT TARGET nlohmann_json_schema_validator)
  FetchContent_Declare(nlohmann_json_schema_validator
    GIT_REPOSITORY "https://github.com/pboettch/json-schema-validator"
    GIT_TAG        "2.3.0"
    GIT_SHALLOW    ON)
endif()

if (NOT TARGET picosha2::picosha2)
  FetchContent_Declare(picosha2
    GIT_REPOSITORY "https://github.com/okdshin/PicoSHA2"
    GIT_TAG        "v1.0.1"
    GIT_SHALLOW    ON)
endif()

if (MAPGET_WITH_WHEEL AND NOT TARGET pybind11)
  FetchContent_Declare(pybind11
    GIT_REPOSITORY "https://github.com/pybind/pybind11.git"
    GIT_TAG        "v2.13.6"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(pybind11)
endif()


if ((MAPGET_WITH_SERVICE OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING) AND NOT TARGET SQLite::SQLite3)
  # Use our clean SQLite integration
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/sqlite.cmake)
  
  add_sqlite(
    VERSION 3.50.2
    TARGET_NAME SQLite3
    NAMESPACE SQLite
    ENABLE_FTS5 ON
    ENABLE_RTREE ON
    ENABLE_JSON1 ON
    ENABLE_MATH ON
    ENABLE_COLUMN_METADATA ON
    THREADSAFE 1
  )
endif()

if (MAPGET_WITH_WHEEL OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
  if (NOT TARGET httplib::httplib)
    FetchContent_MakeAvailable(cpp-httplib)
  endif()
  if (NOT TARGET yaml-cpp::yaml-cpp)
    FetchContent_MakeAvailable(yaml-cpp)
  endif()
  if (NOT TARGET CLI11::CLI11)
    FetchContent_MakeAvailable(cli11)
  endif()
  if (NOT TARGET nlohmann_json_schema_validator)
    FetchContent_MakeAvailable(nlohmann_json_schema_validator)
  endif()
  if (NOT TARGET picosha2::picosha2)
    FetchContent_MakeAvailable(picosha2)
    add_library(picosha2::picosha2 ALIAS picosha2)
  endif()
endif()

# Using option CPPHTTPLIB_USE_POLL to bypass hardcoded file descriptor limit on linux.
# FD_SETSIZE is fixed to 1024 on linux, which is also used for sockets. Once an
# application has opened more than 1024 files, cpp-httplib cannot respond to requests
# anymore, even if some files are closed inbetween.
# For details, see https://github.com/yhirose/cpp-httplib/issues/215
FetchContent_GetProperties(cpp-httplib)
if (cpp_httplib_POPULATED)
  find_package(OpenSSL REQUIRED)
  target_compile_definitions(cpp-httplib
    INTERFACE
      CPPHTTPLIB_OPENSSL_SUPPORT
      CPPHTTPLIB_USE_POLL)
  target_link_libraries(cpp-httplib INTERFACE OpenSSL::SSL)
endif()

if (NOT TARGET simfil)
  set(SIMFIL_WITH_MODEL_JSON YES CACHE BOOL "Simfil with JSON support")
  set(SIMFIL_SHARED          NO  CACHE BOOL "Simfil as static library")
  FetchContent_Declare(simfil
    GIT_REPOSITORY "https://github.com/Klebert-Engineering/simfil.git"
    # TODO: We want to have the simfil diagnostics feature, there is
    #       not yet an official release containing it -> activate main branch
    GIT_TAG        "v0.4.0"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(simfil)
endif()

if (MAPGET_WITH_WHEEL AND NOT TARGET python-cmake-wheel)
  FetchContent_Declare(python-cmake-wheel
    GIT_REPOSITORY "https://github.com/klebert-engineering/python-cmake-wheel.git"
    GIT_TAG        "v1.0.0"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(python-cmake-wheel)
endif()

if (NOT TARGET tiny-process-library)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(tiny-process-library
    GIT_REPOSITORY "https://gitlab.com/eidheim/tiny-process-library"
    GIT_TAG        "v2.0.4"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(tiny-process-library)
endif()
