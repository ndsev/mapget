project(example-tests)

add_subdirectory(cpp/http-datasource)
add_subdirectory(cpp/minimal-http-service)
add_subdirectory(cpp/local-datasource)

if (MAPGET_ENABLE_TESTING AND MAPGET_WITH_WHEEL)

  add_wheel_test(test-examples
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      # Run Python datasource 54544
      -b "python ${CMAKE_CURRENT_LIST_DIR}/python/datasource.py"

      # Run C++ datasource on 54545
      -b "./cpp-sample-http-datasource 54545"

      # Run service on 54546
      -b "./minimal-http-service 54546 127.0.0.1:54544 127.0.0.1:54545"

      # Just on the side: Test fully local service example
      -f "./cpp-sample-local-service"

      # Request from cpp datasource
      -f "python -m mapget fetch -s 127.0.0.1:54546 -m Tropico -l WayLayer -t 12345"

      # Request from py datasource
      -f "python -m mapget fetch -s 127.0.0.1:54546 -m TestMap -l WayLayer -t 12345"
  )

  add_wheel_test(test-mapget-serve
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      # Run Python datasource 54544
      -b "python ${CMAKE_CURRENT_LIST_DIR}/python/datasource.py"

      # Run C++ datasource on 54545
      -b "./cpp-sample-http-datasource 54545"

      # Run service on 54546
      -b "python -m mapget serve -p 54546 -d 127.0.0.1:54544 -d 127.0.0.1:54545"

      # Request from py datasource
      -f "python -m mapget fetch -s 127.0.0.1:54546 -m TestMap -l WayLayer -t 12345"
  )

endif()
