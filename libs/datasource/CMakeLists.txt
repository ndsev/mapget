project(mapget-datasource)

add_library(mapget-datasource STATIC
  include/mapget/datasource/datasource.h
  src/datasource.cpp)

target_include_directories(mapget-datasource
  PUBLIC
    include
  PRIVATE
    include/mapget/datasource)

target_link_libraries(mapget-datasource
  PUBLIC
    httplib::httplib
    yaml-cpp
    mapget-model)

if (MSVC)
  target_compile_definitions(mapget-datasource
    PRIVATE
      _CRT_SECURE_NO_WARNINGS)

  set_target_properties(mapget-datasource PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if (WITH_COVERAGE)
  append_coverage_compiler_flags_to_target(mapget-datasource)
endif()
