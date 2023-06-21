project(mapget-http-datasource)

add_library(mapget-http-datasource STATIC
  include/mapget/http-datasource/datasource-server.h
  include/mapget/http-datasource/datasource-client.h
  include/mapget/detail/http-server.h

  src/datasource-server.cpp
  src/datasource-client.cpp
  src/http-server.cpp)

target_include_directories(mapget-http-datasource
  PUBLIC
    include
  PRIVATE
    include/mapget/http-datasource)

target_link_libraries(mapget-http-datasource
  PUBLIC
    httplib::httplib
    mapget-model
    mapget-service)

if (MSVC)
  target_compile_definitions(mapget-http-datasource
    PRIVATE
      _CRT_SECURE_NO_WARNINGS)

  set_target_properties(mapget-http-datasource PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if (WITH_COVERAGE)
  append_coverage_compiler_flags_to_target(mapget-http-datasource)
endif()
