project(example-tests)

add_subdirectory(cpp/http-datasource)
add_subdirectory(cpp/local-datasource)

if (MAPGET_ENABLE_TESTING AND MAPGET_WITH_WHEEL)

  add_wheel_test(test-local-example
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      # Test fully local service example
      -f "./cpp-sample-local-service")

  add_wheel_test(test-cli-cpp
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      # Run Python datasource 56731
      -b "python ${CMAKE_CURRENT_LIST_DIR}/python/datasource.py"
      # Run C++ datasource on 56732
      -b "./cpp-sample-http-datasource 56732"

      # Run service on 56733
      -b "./mapget serve --port 56733 --datasource-host 127.0.0.1:56731 --datasource-host 127.0.0.1:56732"
      # Request from cpp datasource
      -f "./mapget fetch -s 127.0.0.1:56733 -m Tropico -l WayLayer -t 12345"
      # Request from py datasource
      -f "./mapget fetch -s 127.0.0.1:56733 -m TestMap -l WayLayer -t 12345"
  )

  add_wheel_test(test-cli-python
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      # Run Python datasource 56731
      -b "python ${CMAKE_CURRENT_LIST_DIR}/python/datasource.py"
      # Run C++ datasource on 56732
      -b "./cpp-sample-http-datasource 56732"
      # Run service on 56733
      -b "python -m mapget serve -p 56733 -d 127.0.0.1:56731 -d 127.0.0.1:56732"
      # Request from py datasource
      -f "python -m mapget fetch -s 127.0.0.1:56733 -m TestMap -l WayLayer -t 12345"
  )

endif()
