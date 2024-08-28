project(test.mapget.unit CXX)

FetchContent_Declare(Catch2
  GIT_REPOSITORY "https://github.com/catchorg/Catch2.git"
  GIT_TAG        "v3.5.2"
  GIT_SHALLOW    ON
  FIND_PACKAGE_ARGS)
FetchContent_MakeAvailable(Catch2)

FetchContent_GetProperties(Catch2)
if (Catch2_POPULATED)
  list(APPEND CMAKE_MODULE_PATH "${CATCH2_SOURCE_DIR}/contrib")
endif()

add_executable(test.mapget
  test-model.cpp
  test-model-geometry.cpp
  test-simfil-geometry.cpp
  test-info.cpp
  test-http-datasource.cpp
  test-cache.cpp
  test-config.cpp
  utility.cpp
  utility.h)

add_executable(test.mapget.filelog
  test-file-logging.cpp)

target_link_libraries(test.mapget
  PUBLIC
    mapget-log
    mapget-model
    mapget-http-datasource
    mapget-http-service
    Catch2::Catch2WithMain)

target_link_libraries(test.mapget.filelog
  PUBLIC
  mapget-log
  Catch2::Catch2WithMain)

include(Catch)
include(CTest)
catch_discover_tests(test.mapget)
catch_discover_tests(test.mapget.filelog)

