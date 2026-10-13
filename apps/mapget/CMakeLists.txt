project(mapget-app CXX)

add_executable(mapget
  src/mapget.cpp)

mapget_target_use_allocator(mapget)

target_link_libraries(mapget
  PUBLIC
    mapget-http-service)
