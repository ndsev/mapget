project(test.mapget.unit CXX)

add_executable(test.mapget
  test-main.cpp
  test-object-partitions.cpp
  test-model.cpp
  test-mcp-actions.cpp
  test-mcp-relay.cpp
  test-mcp-auth.cpp
  test-feature-id-index.cpp
  test-model-geometry.cpp
  test-filter-service.cpp
  test-subsetlayer.cpp
  test-featurelayer-filter.cpp
  test-simfil-geometry.cpp
  test-info.cpp
  test-layerschema-domains.cpp
  test-location.cpp
  test-http-datasource.cpp
  test-cache.cpp
  test-service-ttl.cpp
  test-config.cpp
  test-geojson-import.cpp
  test-geojsonsource.cpp
  test-gridsource.cpp
  utility.cpp
  utility.h)

add_executable(test.mapget.filelog
  test-file-logging.cpp)

add_executable(test.mapget.datasource-server
  test-datasource-server.cpp)

# The HTTP tests launch this peer; rebuild both sides after protocol/static-ID changes.
add_dependencies(test.mapget test.mapget.datasource-server)

target_link_libraries(test.mapget
  PRIVATE
    jwt-cpp::jwt-cpp
  PUBLIC
    mapget-log
    mapget-model
    mapget-http-datasource
    mapget-http-service
    mapget-location
    gridsource
    geojsonsource
    Catch2::Catch2)

target_link_libraries(test.mapget.datasource-server
  PUBLIC
    mapget-log
    mapget-http-datasource)

target_compile_definitions(test.mapget
  PRIVATE
    MAPGET_TEST_DATASOURCE_SERVER_EXE=\"$<TARGET_FILE:test.mapget.datasource-server>\"
    MAPGET_TEST_LOCATION_DB=\"${plazs_SOURCE_DIR}/tests/data/places.sqlite\"
    MAPGET_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\")

target_link_libraries(test.mapget.filelog
  PUBLIC
  mapget-log
  Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch)
include(CTest)
catch_discover_tests(test.mapget)
catch_discover_tests(test.mapget.filelog)
