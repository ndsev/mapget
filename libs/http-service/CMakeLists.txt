project(mapget-http-service CXX)

set_source_files_properties(src/mcp-server.cpp PROPERTIES COMPILE_DEFINITIONS
  "MAPGET_MCP_VERSION=\"${MAPGET_VERSION}\"")

add_library(mapget-http-service STATIC
  include/mapget/http-service/http-service.h
  include/mapget/http-service/http-client.h
  include/mapget/http-service/cli.h
  include/mapget/http-service/mcp-config.h

  src/http-service.cpp
  src/http-service-impl.h
  src/http-service-impl.cpp
  src/attachment-handler.cpp
  src/cache-reset-handler.cpp
  src/config-handler.cpp
  src/http-client.cpp
  src/location-handler.cpp
  src/mcp-action-catalog.h
  src/mcp-action-catalog.cpp
  src/mcp-auth.h
  src/mcp-auth.cpp
  src/mcp-config.cpp
  src/mcp-server.h
  src/mcp-server.cpp
  src/mcp-viewer-relay.h
  src/mcp-viewer-relay.cpp
  src/locate-handler.cpp
  src/object-discovery-handler.cpp
  src/sources-handler.cpp
  src/status-handler.cpp
  src/status-page.h
  src/status-page.cpp
  src/tiles-request-json.h
  src/tiles-request-json.cpp
  src/tiles-http-handler.cpp
  src/tiles-ws-controller.cpp
  src/tiles-stream-encoding.h
  src/tiles-stream-encoding.cpp
  src/tiles-ws-request.h
  src/tiles-ws-request.cpp
  src/tiles-ws-session.h
  src/tiles-ws-session.cpp
  src/tiles-ws-status.h
  src/tiles-ws-status.cpp
  src/cli.cpp)

target_include_directories(mapget-http-service
  PUBLIC
    include
  PRIVATE
    include/mapget/http-service)

target_link_libraries(mapget-http-service
  PUBLIC
    drogon
    yaml-cpp
    CLI11::CLI11
    nlohmann_json_schema_validator
    mapget-service
    mapget-http-datasource
    mapget-location
    gridsource
    geojsonsource
  PRIVATE
    jwt-cpp::jwt-cpp
    picosha2
    zlibstatic)

if (MSVC)
  target_compile_definitions(mapget-http-service
    PRIVATE
      _CRT_SECURE_NO_WARNINGS)

  set_target_properties(mapget-http-service PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if (WITH_COVERAGE)
  append_coverage_compiler_flags_to_target(mapget-http-service)
endif()

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
  target_link_libraries(mapget-http-service PUBLIC Threads::Threads)
endif()

install(TARGETS mapget-http-service)
install(DIRECTORY include/mapget/http-service
  DESTINATION ${MAPGET_INSTALL_INCLUDEDIR})
