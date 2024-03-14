project(mapget-app CXX)

add_executable(mapget
  src/mapget.cpp)

target_link_libraries(mapget
  PUBLIC
    mapget-http-service)
