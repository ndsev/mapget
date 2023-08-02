cmake_minimum_required(VERSION 3.14.0)
project(mapget-log)

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_library(mapget-log STATIC
  include/mapget/log.h
  src/log.cpp)

target_link_libraries(mapget-log
  PUBLIC
  spdlog::spdlog
)

target_include_directories(mapget-log
  PUBLIC
    include
  PRIVATE
    src)
