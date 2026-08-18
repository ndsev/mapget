project(test.mapget.integration CXX)

set(MAPGET_PICK_PORTS_PY "${CMAKE_CURRENT_LIST_DIR}/detect-ports-and-prepare-config-yaml.py")
set(MAPGET_RUN_WITH_PORTS "${CMAKE_CURRENT_LIST_DIR}/run-with-ports.bash")
set(MAPGET_NDSMATH_PYTHON_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../ndslive-math/python")
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
  NAME test-cli-bind-before-datasources
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/test-bind-before-datasources.py"
    "$<TARGET_FILE:mapget>")
set_tests_properties(test-cli-bind-before-datasources PROPERTIES LABELS integration)

if (EXISTS "${MAPGET_NDSMATH_PYTHON_DIR}/pyproject.toml")
  set(MAPGET_INSTALL_NDSMATH_PYTHON -f "python -m pip install ${MAPGET_NDSMATH_PYTHON_DIR}")
else()
  set(MAPGET_INSTALL_NDSMATH_PYTHON -f "python -m pip install ndslive-math")
endif()

add_wheel_test(test-local-example
  WORKING_DIRECTORY
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  COMMANDS
    # As a base case, test fully local service example
    # that does not require networking
    -f "./cpp-sample-local-service")

add_wheel_test(test-python-bindings-smoke
  WORKING_DIRECTORY
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  COMMANDS
    ${MAPGET_INSTALL_NDSMATH_PYTHON}
    -f "python ${CMAKE_CURRENT_LIST_DIR}/python-bindings-smoke.py")

if (TARGET mapget-location-database)
  add_wheel_test(test-python-location-database-smoke
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      ${MAPGET_INSTALL_NDSMATH_PYTHON}
      -f "python ${CMAKE_CURRENT_LIST_DIR}/python-location-database-smoke.py")
endif()

if (NOT WITH_COVERAGE)
  # Python wheels currently don't play well with gcov.
  message(STATUS "WITH_COVERAGE: ${WITH_COVERAGE}: Enabling wheel-based integration test.")
  add_wheel_test(test-cli-cpp
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      ${MAPGET_INSTALL_NDSMATH_PYTHON}

      # Pick ports at test runtime (reduces CI flakiness vs. hard-coded ports)
      -f "python ${MAPGET_PICK_PORTS_PY} --out-dir .integration/test-cli-cpp"

      # Run Python datasource
      -b "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-cli-cpp/ports.env python ${CMAKE_CURRENT_LIST_DIR}/../../examples/python/datasource.py $DATASOURCE_PY_PORT"

      # Run C++ datasource
      -b "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-cli-cpp/ports.env ./cpp-sample-http-datasource $DATASOURCE_CPP_PORT"

      # Run service
      -b "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-cli-cpp/ports.env ./mapget --log-level trace serve --host 127.0.0.1 --wait-ms 5000 -p $MAPGET_SERVER_PORT -d 127.0.0.1:$DATASOURCE_CPP_PORT -d 127.0.0.1:$DATASOURCE_PY_PORT"

      # Request from cpp datasource
      -f "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-cli-cpp/ports.env ./mapget --log-level trace fetch -s 127.0.0.1:$MAPGET_SERVER_PORT -m Tropico -l WayLayer -t 65536"

      # Request from py datasource
      -f "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-cli-cpp/ports.env ./mapget --log-level trace fetch -s 127.0.0.1:$MAPGET_SERVER_PORT -m TestMap -l WayLayer -t 65536"
    )
endif ()

if (NOT WITH_COVERAGE)
  add_wheel_test(test-cli-datasource-exe
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      ${MAPGET_INSTALL_NDSMATH_PYTHON}

      # Pick ports at test runtime (reduces CI flakiness vs. hard-coded ports)
      -f "python ${MAPGET_PICK_PORTS_PY} --out-dir .integration/test-cli-datasource-exe"

      # Run service with auto-launched python datasource
      -b "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-cli-datasource-exe/ports.env ${CMAKE_CURRENT_LIST_DIR}/mapget-exec-datasource.bash $MAPGET_SERVER_PORT"

      # Request from py datasource
      -f "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-cli-datasource-exe/ports.env ./mapget --log-level trace fetch -s 127.0.0.1:$MAPGET_SERVER_PORT -m TestMap -l WayLayer -t 65536"
    )
endif ()

if (NOT WITH_COVERAGE)
  # Python wheels currently don't play well with gcov.
  add_wheel_test(test-cli-python
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      ${MAPGET_INSTALL_NDSMATH_PYTHON}

      # Pick ports at test runtime (reduces CI flakiness vs. hard-coded ports)
      -f "python ${MAPGET_PICK_PORTS_PY} --out-dir .integration/test-cli-python"

      # Run Python datasource
      -b "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-cli-python/ports.env python ${CMAKE_CURRENT_LIST_DIR}/../../examples/python/datasource.py $DATASOURCE_PY_PORT"

      # Run C++ datasource
      -b "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-cli-python/ports.env ./cpp-sample-http-datasource $DATASOURCE_CPP_PORT"

      # Run service
      -b "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-cli-python/ports.env python -m mapget --log-level trace serve -p $MAPGET_SERVER_PORT -d 127.0.0.1:$DATASOURCE_CPP_PORT -d 127.0.0.1:$DATASOURCE_PY_PORT"

      # Request from py datasource
      -f "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-cli-python/ports.env python -m mapget --log-level trace fetch -s 127.0.0.1:$MAPGET_SERVER_PORT -m TestMap -l WayLayer -t 65536"
    )
endif()

if (NOT WITH_COVERAGE)
  # Python wheels currently don't play well with gcov.
  add_wheel_test(test-config-cpp
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      ${MAPGET_INSTALL_NDSMATH_PYTHON}

      # Pick ports + write config YAMLs at test runtime.
      -f "python ${MAPGET_PICK_PORTS_PY} --out-dir .integration/test-config-cpp"

      # Run Python datasource
      -b "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-config-cpp/ports.env python ${CMAKE_CURRENT_LIST_DIR}/../../examples/python/datasource.py $DATASOURCE_PY_PORT"

      # Run C++ datasource
      -b "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-config-cpp/ports.env ./cpp-sample-http-datasource $DATASOURCE_CPP_PORT"

      # Run service
      -b "./mapget --config .integration/test-config-cpp/sample-service.yaml serve"

      # Request from py datasource
      -f "./mapget --config .integration/test-config-cpp/sample-second-datasource.yaml fetch"

      # Request from cpp datasource
      -f "./mapget --config .integration/test-config-cpp/sample-first-datasource.yaml fetch"
    )
endif ()

if (NOT WITH_COVERAGE)
  # Python wheels currently don't play well with gcov.
  add_wheel_test(test-config-py
    WORKING_DIRECTORY
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMANDS
      ${MAPGET_INSTALL_NDSMATH_PYTHON}

      # Pick ports + write config YAMLs at test runtime.
      -f "python ${MAPGET_PICK_PORTS_PY} --out-dir .integration/test-config-py"

      # Run Python datasource
      -b "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-config-py/ports.env python ${CMAKE_CURRENT_LIST_DIR}/../../examples/python/datasource.py $DATASOURCE_PY_PORT"

      # Run C++ datasource
      -b "bash ${MAPGET_RUN_WITH_PORTS} .integration/test-config-py/ports.env ./cpp-sample-http-datasource $DATASOURCE_CPP_PORT"

      # Run service
      -b "python -m mapget --config .integration/test-config-py/sample-service.yaml serve"

      # Request from py datasource
      -f "python -m mapget --config .integration/test-config-py/sample-second-datasource.yaml fetch"

      # Request from cpp datasource
      -f "python -m mapget --config .integration/test-config-py/sample-first-datasource.yaml fetch"
  )
endif ()
