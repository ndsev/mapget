project(mapget-app)

add_executable(mapget
  src/mapget.cpp)

target_link_libraries(mapget
  PUBLIC
    mapget-http-service)