cmake_minimum_required(VERSION 3.20)
cmake_policy(VERSION 3.20)
cmake_policy(SET CMP0117 NEW)

project(mapget CXX)

set(MAPGET_VERSION 2024.3.2)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
include(GNUInstallDirs)

if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(MAPGET_WITH_WHEEL ON CACHE BOOL "Enable mapget Python wheel (output to WHEEL_DEPLOY_DIRECTORY).")
  set(MAPGET_WITH_SERVICE ON CACHE BOOL "Enable mapget-service library. Requires threads.")
  set(MAPGET_WITH_HTTPLIB ON CACHE BOOL "Enable mapget-http-datasource and mapget-http-service libraries.")
  set(MAPGET_ENABLE_TESTING ON CACHE BOOL "Enable testing.")
  set(MAPGET_BUILD_EXAMPLES ON CACHE BOOL "Build examples.")
endif()

option(MAPGET_WITH_WHEEL "Enable mapget Python wheel (output to WHEEL_DEPLOY_DIRECTORY).")
option(MAPGET_WITH_SERVICE "Enable mapget-service library. Requires threads.")
option(MAPGET_WITH_HTTPLIB "Enable mapget-http-datasource and mapget-http-service libraries.")

set(Python3_FIND_STRATEGY LOCATION)

if (NOT MSVC)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(CMAKE_CXX_FLAGS -fPIC)
endif()

if (NOT MAPGET_DEPLOY_DIR)
  set (MAPGET_DEPLOY_DIR "${CMAKE_BINARY_DIR}/bin")
endif()
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${MAPGET_DEPLOY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${MAPGET_DEPLOY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${MAPGET_DEPLOY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MAPGET_DEPLOY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${MAPGET_DEPLOY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${MAPGET_DEPLOY_DIR}")
set(MAPGET_INSTALL_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}/mapget/")

option(WITH_COVERAGE "Enable gcovr coverage" NO)
if (NOT CMAKE_COMPILER_IS_GNUCXX AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(WITH_COVERAGE NO)
endif()

find_program(GCOVR_BIN gcovr)
if (WITH_COVERAGE AND NOT GCOVR_BIN)
  set(WITH_COVERAGE NO)
  message(WARNING "Could not find gcovr binary. Disabling coverage report!")
endif()

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
  if (MAPGET_WITH_SERVICE OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)
  endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake-modules")
if (WITH_COVERAGE)
  include(CodeCoverage)
endif()

##############
# deps
include(deps.cmake)

if (MAPGET_WITH_WHEEL)
  if (NOT TARGET wheel)
    list(APPEND CMAKE_MODULE_PATH "${python-cmake-wheel_SOURCE_DIR}")
    include(python-wheel)
    set(WHEEL_DEPLOY_DIRECTORY "${MAPGET_DEPLOY_DIR}/wheel")
  endif()
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
  endif()

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
