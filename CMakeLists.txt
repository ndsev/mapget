cmake_minimum_required(VERSION 3.14)

if (NOT PROJECT_NAME)
  set (MAPGET_ENABLE_TESTING ON)
endif()

project(mapget)
include(FetchContent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(MAPGET_VERSION 0.0.1)

option(MAPGET_BUILD_WHEEL "Enable mapget whl-output to WHEEL_DEPLOY_DIRECTORY." ON)

if (NOT MSVC)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(CMAKE_CXX_FLAGS -fPIC)
endif()

if (NOT MAPGET_DEPLOY_DIR)
  set (MAPGET_DEPLOY_DIR "${CMAKE_BINARY_DIR}/bin")
endif()
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY "${MAPGET_DEPLOY_DIR}")
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY "${MAPGET_DEPLOY_DIR}")

##############
# deps

find_package(OpenSSL REQUIRED)

# if (MAPGET_BUILD_WHEEL)
#   if (NOT TARGET wheel)
#     set(Python3_FIND_STRATEGY LOCATION)
#     set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/deps/python-cmake-wheel")
#     include(python-wheel)
#     set(WHEEL_DEPLOY_DIRECTORY "${MAPGET_DEPLOY_DIR}/wheel")
#   endif()
# endif()

if (NOT TARGET stx)
  FetchContent_Declare(stx
    GIT_REPOSITORY "https://github.com/Klebert-Engineering/stx.git"
    GIT_TAG        "main"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(stx)
endif()

if (NOT TARGET spdlog)
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
    GIT_TAG        "issue/27-modelpool-serialization"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(simfil)
endif()

if (NOT TARGET httplib)
  FetchContent_Declare(httplib
    GIT_REPOSITORY "https://github.com/yhirose/cpp-httplib.git"
    GIT_TAG        "master"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(httplib)
  target_compile_definitions(httplib
    INTERFACE
      CPPHTTPLIB_OPENSSL_SUPPORT)
  target_link_libraries(httplib INTERFACE OpenSSL::SSL)
endif()

if (NOT TARGET bitsery)
  FetchContent_Declare(bitsery
    GIT_REPOSITORY "https://github.com/fraillt/bitsery.git"
    GIT_TAG        "master"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(bitsery)
endif()

##############
# libs

add_subdirectory(libs/model)

##############
# tests

if (MAPGET_ENABLE_TESTING)
  add_subdirectory(test)
endif()

##############
# wheels

# if (MAPGET_BUILD_WHEEL)
#   add_subdirectory(wheel)
# endif()