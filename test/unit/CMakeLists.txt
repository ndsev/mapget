project(test.mapget.unit CXX)

add_executable(test.mapget
  test-main.cpp
  test-object-partitions.cpp
  test-model.cpp
  test-feature-id-index.cpp
  test-model-geometry.cpp
  test-filter-service.cpp
  test-subsetlayer.cpp
  test-featurelayer-filter.cpp
  test-simfil-geometry.cpp
  test-info.cpp
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

mapget_target_use_allocator(test.mapget)

# Exercise both executable interposition and the non-invasive library/Python path.
add_executable(test.mapget.allocator test-allocator-memory.cpp)
target_link_libraries(test.mapget.allocator PRIVATE mapget-service Catch2::Catch2WithMain)
target_compile_definitions(test.mapget.allocator PRIVATE MAPGET_TEST_JEMALLOC=$<BOOL:${MAPGET_WITH_JEMALLOC}>)
mapget_target_use_allocator(test.mapget.allocator)
add_test(NAME test.mapget.allocator
  COMMAND "${CMAKE_COMMAND}" -E env --unset=MALLOC_CONF "$<TARGET_FILE:test.mapget.allocator>" "[allocator]")

if(MAPGET_WITH_JEMALLOC)
  add_executable(test.mapget.allocator-system test-allocator-memory.cpp)
  target_link_libraries(test.mapget.allocator-system PRIVATE mapget-service Catch2::Catch2WithMain)
  target_compile_definitions(test.mapget.allocator-system PRIVATE MAPGET_TEST_JEMALLOC=0)
  add_test(NAME test.mapget.allocator-system COMMAND test.mapget.allocator-system "[allocator]")
  add_test(NAME test.mapget.allocator-no-background COMMAND test.mapget.allocator "[allocator]")
  set_tests_properties(test.mapget.allocator-no-background PROPERTIES
    ENVIRONMENT "MALLOC_CONF=background_thread:false;MAPGET_TEST_BACKGROUND_THREADS=0")
  add_test(NAME test.mapget.allocator-purge COMMAND test.mapget.allocator "[allocator-purge]")
  set_tests_properties(test.mapget.allocator-purge PROPERTIES
    ENVIRONMENT "MALLOC_CONF=background_thread:true,dirty_decay_ms:100,muzzy_decay_ms:0"
    TIMEOUT 20)
endif()

target_link_libraries(test.mapget
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
