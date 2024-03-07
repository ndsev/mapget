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
    mapget-service
    tiny-process-library)

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

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
  target_link_libraries(mapget-http-datasource PUBLIC Threads::Threads)
endif()

install(TARGETS mapget-http-datasource)
install(DIRECTORY include/mapget/http-datasource
  DESTINATION ${MAPGET_INSTALL_INCLUDEDIR})
install(DIRECTORY include/mapget/detail
  DESTINATION ${MAPGET_INSTALL_INCLUDEDIR})
