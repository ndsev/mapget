project(cpp-sample-local-service CXX)

add_executable(${PROJECT_NAME}
  main.cpp)

target_link_libraries(${PROJECT_NAME}
  PUBLIC
  mapget-service)
