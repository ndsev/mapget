project(test.mapget.unit CXX)

add_executable(test.mapget
  test-main.cpp
  test-object-partitions.cpp
  test-model.cpp
  test-mcp-build.cpp
  test-datasource-auth.cpp
  test-featureidentity.cpp
  test-feature-id-index.cpp
  test-foreign-feature-id.cpp
  test-model-geometry.cpp
  test-pointcloud-sampler.cpp
  test-incoming-relations.cpp
  test-filter-service.cpp
  test-filter-request.cpp
  test-subsetlayer.cpp
  test-subset-schema.cpp
  test-featurelayer-filter.cpp
  test-simfil-geometry.cpp
  test-info.cpp
  test-mapschema-metadata.cpp
  test-mapschema.cpp
  test-mapschema-json.cpp
  test-mapschema-descriptor.cpp
  test-mapschema-compose.cpp
  test-http-schema.cpp
  test-layerschema-domains.cpp
  test-layer-schema-binding.cpp
  test-schema-composition.cpp
  test-schema-inventory.cpp
  test-schema-validation.cpp
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

if(WIN32)
  target_link_libraries(test.mapget PRIVATE advapi32)
endif()

if(MAPGET_WITH_MCP)
  target_sources(test.mapget PRIVATE
    test-mcp-actions.cpp
    test-mcp-relay.cpp
    test-mcp-auth.cpp
    test-mcp-native.cpp
    test-mcp-help.cpp
    test-mcp-schema2.cpp)
endif()

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

# The HTTP tests launch this peer; rebuild both sides after protocol/static-ID changes.
add_dependencies(test.mapget test.mapget.datasource-server)

target_link_libraries(test.mapget
  PRIVATE
    jwt-cpp::jwt-cpp
    picosha2
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

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME test-mcp-build-cli
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../integration/test-mcp-build-cli.py"
    "$<TARGET_FILE:mapget>" "$<BOOL:${MAPGET_WITH_MCP}>")
set_tests_properties(test-mcp-build-cli PROPERTIES TIMEOUT 60)
