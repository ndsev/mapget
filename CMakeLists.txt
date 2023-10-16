cmake_minimum_required(VERSION 3.14)


if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set (MAPGET_WITH_WHEEL ON CACHE BOOL "Enable mapget Python wheel (output to WHEEL_DEPLOY_DIRECTORY).")
  set (MAPGET_WITH_SERVICE ON CACHE BOOL "Enable mapget-service library. Requires threads.")
  set (MAPGET_WITH_HTTPLIB ON CACHE BOOL "Enable mapget-http-datasource and mapget-http-service libraries.")
  set (MAPGET_ENABLE_TESTING ON CACHE BOOL "Enable testing.")
  set (MAPGET_BUILD_EXAMPLES ON CACHE BOOL "Build examples.")
endif()

option(MAPGET_WITH_WHEEL "Enable mapget Python wheel (output to WHEEL_DEPLOY_DIRECTORY).")
option(MAPGET_WITH_SERVICE "Enable mapget-service library. Requires threads.")
option(MAPGET_WITH_HTTPLIB "Enable mapget-http-datasource and mapget-http-service libraries.")

project(mapget)
include(FetchContent)

set(MAPGET_VERSION 0.1.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT MSVC)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(CMAKE_CXX_FLAGS -fPIC)
endif()

if (NOT MAPGET_DEPLOY_DIR)
  set (MAPGET_DEPLOY_DIR "${CMAKE_BINARY_DIR}/bin")
endif()
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY "${MAPGET_DEPLOY_DIR}")
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY "${MAPGET_DEPLOY_DIR}")
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${MAPGET_DEPLOY_DIR}")
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MAPGET_DEPLOY_DIR}")
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${MAPGET_DEPLOY_DIR}")
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${MAPGET_DEPLOY_DIR}")

if (CMAKE_COMPILER_IS_GNUCXX AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  option(WITH_COVERAGE "Enable gcovr coverage" YES)
else()
  set(WITH_COVERAGE NO)
endif()

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
  if (MAPGET_WITH_SERVICE OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)
  endif()
endif()

find_program(GCOVR_BIN gcovr)
if (WITH_COVERAGE AND NOT GCOVR_BIN)
  set(WITH_COVERAGE NO)
  message(WARNING "Could not find gcovr binary. Disabling coverage report!")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake-modules")

##############
# deps

if (NOT TARGET stx)
  FetchContent_Declare(stx
    GIT_REPOSITORY "https://github.com/Klebert-Engineering/stx.git"
    GIT_TAG        "main"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(stx)
endif()

if (NOT TARGET spdlog::spdlog)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY "https://github.com/gabime/spdlog.git"
    GIT_TAG        "v1.x"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(spdlog)
endif()

set(SIMFIL_WITH_MODEL_JSON YES)
if (NOT TARGET simfil)
  FetchContent_Declare(simfil
    GIT_REPOSITORY "https://github.com/Klebert-Engineering/simfil.git"
    GIT_TAG        "main"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(simfil)
endif()

if (NOT TARGET bitsery)
  FetchContent_Declare(bitsery
    GIT_REPOSITORY "https://github.com/fraillt/bitsery.git"
    GIT_TAG        "master"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(bitsery)
endif()

if (MAPGET_WITH_WHEEL OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
  if (NOT TARGET httplib)
    FetchContent_Declare(httplib
      GIT_REPOSITORY "https://github.com/yhirose/cpp-httplib.git"
      GIT_TAG        "master"
      GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(httplib)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(httplib
      INTERFACE
        CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(httplib INTERFACE OpenSSL::SSL)
  endif()

  if (NOT TARGET yaml-cpp)
    FetchContent_Declare(yaml-cpp
      GIT_REPOSITORY "https://github.com/jbeder/yaml-cpp.git"
      GIT_TAG        "master"
      GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(yaml-cpp)
  endif()

  if (NOT TARGET cli11)
    include(FetchContent)
    FetchContent_Declare(cli11
      GIT_REPOSITORY https://github.com/CLIUtils/CLI11
      GIT_TAG        v2.3.2
      GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(cli11)
  endif()

  if (NOT TARGET tiny-process-library)
    set(BUILD_TESTING OFF)
    FetchContent_Declare(tiny-process-library
      GIT_REPOSITORY https://gitlab.com/eidheim/tiny-process-library
      GIT_TAG        v2.0.4
      GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(tiny-process-library)
  endif()
endif()

if (MAPGET_WITH_WHEEL)
  if (NOT TARGET wheel)
    set(Python3_FIND_STRATEGY LOCATION)
    FetchContent_Declare(python-cmake-wheel
      GIT_REPOSITORY "https://github.com/klebert-engineering/python-cmake-wheel.git"
      GIT_TAG        "v0.9.0"
      GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(python-cmake-wheel)
    list(APPEND CMAKE_MODULE_PATH "${python-cmake-wheel_SOURCE_DIR}")
    include(python-wheel)
    set(WHEEL_DEPLOY_DIRECTORY "${MAPGET_DEPLOY_DIR}/wheel")
  endif()

  if (NOT TARGET pybind11)
    FetchContent_Declare(
      pybind11
      GIT_REPOSITORY https://github.com/pybind/pybind11.git
      GIT_TAG        v2.10.4
      GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(pybind11)
  endif()
endif()

if (WITH_COVERAGE)
  include(CodeCoverage)
endif()

# RocksDB adds ASM as language to the project, upon which CodeCoverage.cmake
# tries to find a matching compiler and fails. So RocksDB must be included
# after CodeCoverage.
if (NOT TARGET rocksdb)
  set(WITH_GFLAGS NO CACHE BOOL "rocksdb without gflags")
  set(WITH_TESTS NO CACHE BOOL "rocksdb without tests")
  set(WITH_BENCHMARK_TOOLS NO CACHE BOOL "rocksdb without benchmarking")
  set(BENCHMARK_ENABLE_GTEST_TESTS NO CACHE BOOL "rocksdb without gtest")

  FetchContent_Declare(rocksdb
    GIT_REPOSITORY "https://github.com/facebook/rocksdb.git"
    GIT_TAG        "main"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(rocksdb)
endif()

##############
# libs

add_subdirectory(libs/logging)
add_subdirectory(libs/model)

if (MAPGET_WITH_SERVICE OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
  add_subdirectory(libs/service)
endif()

if (MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
  add_subdirectory(libs/http-datasource)
  add_subdirectory(libs/http-service)
  add_subdirectory(apps/mapget)
endif()

if (MAPGET_WITH_WHEEL)
  add_subdirectory(libs/pymapget)
endif()

##############
# tests

if (MAPGET_ENABLE_TESTING)
  enable_testing()
  add_subdirectory(test/unit)

  if (MAPGET_WITH_WHEEL)
    add_subdirectory(test/integration)
  endif ()

  if (WITH_COVERAGE)
    setup_target_for_coverage_gcovr_html(
      NAME coverage
      EXECUTABLE test.mapget
      BASE_DIRECTORY "${CMAKE_SOURCE_DIR}/src"
      EXCLUDE "*catch2*")
  endif()
endif()

##############
# examples

if (MAPGET_BUILD_EXAMPLES OR MAPGET_ENABLE_TESTING)
  add_subdirectory(examples)
endif()
