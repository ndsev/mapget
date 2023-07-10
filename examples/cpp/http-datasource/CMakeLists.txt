project(cpp-sample-http-datasource)

add_executable(${PROJECT_NAME}
  main.cpp)

target_link_libraries(${PROJECT_NAME}
  PUBLIC
  mapget-http-service)

configure_file(sample_datasource_info.json ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sample_datasource_info.json)