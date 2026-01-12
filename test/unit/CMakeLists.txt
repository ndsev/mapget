project(test.mapget.unit CXX)

add_executable(test.mapget
  test-model.cpp
  test-model-geometry.cpp
  test-simfil-geometry.cpp
  test-info.cpp
  test-http-datasource.cpp
  test-cache.cpp
  test-service-ttl.cpp
  test-config.cpp
  test-geojsonsource.cpp
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
    geojsonsource
    Catch2::Catch2WithMain)

target_link_libraries(test.mapget.filelog
  PUBLIC
  mapget-log
  Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch)
include(CTest)
catch_discover_tests(test.mapget)
catch_discover_tests(test.mapget.filelog)
