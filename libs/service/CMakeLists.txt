project(mapget-hub)

add_library(mapget-hub STATIC
  include/mapget/service/service.h
  include/mapget/service/cache.h
  include/mapget/service/datasource.h
  include/mapget/service/memcache.h

  src/service.cpp
  src/cache.cpp
  src/datasource.cpp
  src/memcache.cpp)

target_include_directories(mapget-hub
  PUBLIC
    include
  PRIVATE
  include/mapget/service)

target_link_libraries(mapget-hub
  PUBLIC
    stx::stx
    mapget-model)

if (MSVC)
  target_compile_definitions(mapget-hub
    PRIVATE
      _CRT_SECURE_NO_WARNINGS)

  set_target_properties(mapget-hub PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if (WITH_COVERAGE)
  append_coverage_compiler_flags_to_target(mapget-hub)
endif()
