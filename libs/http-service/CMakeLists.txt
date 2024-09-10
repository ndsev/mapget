project(mapget-http-service CXX)

add_library(mapget-http-service STATIC
  include/mapget/http-service/http-service.h
  include/mapget/http-service/http-client.h
  include/mapget/http-service/cli.h

  src/http-service.cpp
  src/http-client.cpp
  src/cli.cpp)

target_include_directories(mapget-http-service
  PUBLIC
    include
  PRIVATE
    include/mapget/http-service)

target_link_libraries(mapget-http-service
  PUBLIC
    httplib::httplib
    yaml-cpp
    CLI11::CLI11
    nlohmann_json_schema_validator
    mapget-service
    mapget-http-datasource)

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

configure_file(default_config_schema.json ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/default_config_schema.json)
