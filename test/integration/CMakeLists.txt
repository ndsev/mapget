project(test.mapget.integration CXX)

# TODO: Figure out a way to do this without assuming free ports.
set (MAPGET_SERVER_PORT 61852)
set (DATASOURCE_CPP_PORT 61853)
set (DATASOURCE_PY_PORT 61854)

add_wheel_test(test-local-example
  WORKING_DIRECTORY
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  COMMANDS
    # As a base case, test fully local service example
    # that does not require networking
    -f "./cpp-sample-local-service")

if (NOT WITH_COVERAGE)
  # Python wheels currently don't play well with gcov.
  message(STATUS "WITH_COVERAGE: ${WITH_COVERAGE}: Enabling wheel-based integration test.")
  add_wheel_test(test-cli-cpp
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      # Run Python datasource
      -b "python ${CMAKE_CURRENT_LIST_DIR}/../../examples/python/datasource.py ${DATASOURCE_PY_PORT}"

      # Run C++ datasource
      -b "./cpp-sample-http-datasource ${DATASOURCE_CPP_PORT}"

      # Run service
      -b "./mapget --log-level trace serve -p ${MAPGET_SERVER_PORT} -d 127.0.0.1:${DATASOURCE_CPP_PORT} -d 127.0.0.1:${DATASOURCE_PY_PORT}"

      # Request from cpp datasource
      -f "./mapget --log-level trace fetch -s 127.0.0.1:${MAPGET_SERVER_PORT} -m Tropico -l WayLayer -t 12345"

      # Request from py datasource
      -f "./mapget --log-level trace fetch -s 127.0.0.1:${MAPGET_SERVER_PORT} -m TestMap -l WayLayer -t 12345"
    )
endif ()

if (NOT WITH_COVERAGE)
  add_wheel_test(test-cli-datasource-exe
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      # Run service with auto-launched python datasource
      -b "${CMAKE_CURRENT_LIST_DIR}/mapget-exec-datasource.bash ${MAPGET_SERVER_PORT}"

      # Request from py datasource
      -f "./mapget --log-level trace fetch -s 127.0.0.1:${MAPGET_SERVER_PORT} -m TestMap -l WayLayer -t 12345"
    )
endif ()

if (NOT WITH_COVERAGE)
  # Python wheels currently don't play well with gcov.
  add_wheel_test(test-cli-python
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      # Run Python datasource
      -b "python ${CMAKE_CURRENT_LIST_DIR}/../../examples/python/datasource.py ${DATASOURCE_PY_PORT}"

      # Run C++ datasource
      -b "./cpp-sample-http-datasource ${DATASOURCE_CPP_PORT}"

      # Run service
      -b "python -m mapget --log-level trace serve -p ${MAPGET_SERVER_PORT} -d 127.0.0.1:${DATASOURCE_CPP_PORT} -d 127.0.0.1:${DATASOURCE_PY_PORT}"

      # Request from py datasource
      -f "python -m mapget --log-level trace fetch -s 127.0.0.1:${MAPGET_SERVER_PORT} -m TestMap -l WayLayer -t 12345"
    )
endif()

if (NOT WITH_COVERAGE)
  # Python wheels currently don't play well with gcov.
  add_wheel_test(test-config-cpp
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      # Run Python datasource
      -b "python ${CMAKE_CURRENT_LIST_DIR}/../../examples/python/datasource.py ${DATASOURCE_PY_PORT}"

      # Run C++ datasource
      -b "./cpp-sample-http-datasource ${DATASOURCE_CPP_PORT}"

      # Run service
      -b "./mapget --config ${CMAKE_CURRENT_LIST_DIR}/../../examples/config/sample-service.yaml serve"

      # Request from py datasource
      -f "./mapget --config ${CMAKE_CURRENT_LIST_DIR}/../../examples/config/sample-second-datasource.yaml fetch"

      # Request from cpp datasource
      -f "./mapget --config ${CMAKE_CURRENT_LIST_DIR}/../../examples/config/sample-first-datasource.yaml fetch"
    )
endif ()

if (NOT WITH_COVERAGE)
  # Python wheels currently don't play well with gcov.
  add_wheel_test(test-config-py
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      # Run Python datasource
      -b "python ${CMAKE_CURRENT_LIST_DIR}/../../examples/python/datasource.py ${DATASOURCE_PY_PORT}"

      # Run C++ datasource
      -b "./cpp-sample-http-datasource ${DATASOURCE_CPP_PORT}"

      # Run service
      -b "python -m mapget --config ${CMAKE_CURRENT_LIST_DIR}/../../examples/config/sample-service.yaml serve"

      # Request from py datasource
      -f "python -m mapget --config ${CMAKE_CURRENT_LIST_DIR}/../../examples/config/sample-second-datasource.yaml fetch"

      # Request from cpp datasource
      -f "python -m mapget --config ${CMAKE_CURRENT_LIST_DIR}/../../examples/config/sample-first-datasource.yaml fetch"
  )
endif ()
