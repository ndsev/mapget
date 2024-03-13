cmake_minimum_required(VERSION 3.14.0)
project(mapget-log)

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_library(mapget-log STATIC
  include/mapget/log.h
  src/log.cpp)

target_link_libraries(mapget-log
  PUBLIC
    spdlog::spdlog
    fmt::fmt
    simfil::simfil)

target_include_directories(mapget-log
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${MAPGET_INSTALL_INCLUDEDIR}>
  PRIVATE
    src)

install(TARGETS mapget-log)
install(FILES include/mapget/log.h
  DESTINATION ${MAPGET_INSTALL_INCLUDEDIR})
