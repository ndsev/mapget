project(mapget-service CXX)

add_library(mapget-service STATIC
  include/mapget/service/service.h
  include/mapget/service/cache.h
  include/mapget/service/datasource.h
  include/mapget/service/memcache.h
  include/mapget/service/rocksdbcache.h
  include/mapget/service/locate.h
  include/mapget/service/config.h

  src/service.cpp
  src/cache.cpp
  src/datasource.cpp
  src/memcache.cpp
  src/rocksdbcache.cpp
  src/locate.cpp
  src/config.cpp)

target_include_directories(mapget-service
  PUBLIC
    include
  PRIVATE
    include/mapget/service)

target_link_libraries(mapget-service
  PUBLIC
    mapget-model
    mapget-log
    RocksDB::rocksdb
    yaml-cpp::yaml-cpp)

if (MSVC)
  target_compile_definitions(mapget-service
    PRIVATE
      _CRT_SECURE_NO_WARNINGS)

  set_target_properties(mapget-service PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if (WITH_COVERAGE)
  append_coverage_compiler_flags_to_target(mapget-service)
endif()

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
  target_link_libraries(mapget-service PUBLIC Threads::Threads)
endif()

install(TARGETS mapget-service)
install(DIRECTORY include/mapget/service
  DESTINATION ${MAPGET_INSTALL_INCLUDEDIR})
