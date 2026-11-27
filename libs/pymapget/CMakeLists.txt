project(pymapget CXX)

nanobind_add_module(pymapget
  BACKEND_MODULE nanobind_backend
    py-mapget.cpp
    binding/py-model.h
    binding/py-tileid.h
    binding/py-layer.h
    binding/py-datasource.h
    binding/py-client.h)

if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  target_compile_options(pymapget PUBLIC /permissive-)
endif ()

target_link_libraries(pymapget
  PRIVATE
    fmt::fmt
    mapget-model
    mapget-http-datasource
    mapget-http-service)

target_include_directories(pymapget
  PRIVATE
    binding)

target_compile_features(pymapget
  PRIVATE
    cxx_std_20)

set(DEPLOY_FILES "${CMAKE_CURRENT_LIST_DIR}/__main__.py")
if (TARGET mapget-location-database)
  get_target_property(
    MAPGET_LOCATION_DEPLOY_FILES
    mapget-location-database
    MAPGET_LOCATION_DEPLOY_FILES)
  list(APPEND DEPLOY_FILES ${MAPGET_LOCATION_DEPLOY_FILES})
endif()

add_wheel(pymapget
  NAME mapget
  AUTHOR "Klebert Engineering"
  URL "https://github.com/ndsev/mapget"
  PYTHON_REQUIRES ">=3.10"
  LIMITED_API cp310
  LICENSE_EXPRESSION "BSD-3-Clause"
  README_PATH "${CMAKE_CURRENT_LIST_DIR}/../../README.md"
  VERSION "${MAPGET_VERSION}"
  DESCRIPTION "Python bindings for the mapget library."
  TARGET_DEPENDENCIES mapget-model mapget-http-datasource
  MODULE_DEPENDENCIES ndslive-math "nanobind-backend>=1.0,<2"
  LICENSE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../LICENSE"
  DEPLOY_FILES ${DEPLOY_FILES})

if (TARGET mapget-location-database)
  # The generated database must exist before python-cmake-wheel copies package
  # resources into its staging directory.
  add_dependencies(pymapget-copy-files mapget-location-database)
endif()

if(TARGET mapget-mcp-docs)
  get_target_property(help_directory mapget-mcp-docs MAPGET_MCP_HELP_DIRECTORY)
  add_dependencies(pymapget-copy-files mapget-mcp-docs)
  # Preserve the directory tree (DEPLOY_FILES only copies individual files).
  add_custom_command(TARGET pymapget-copy-files POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/mapget.wheel/mapget/mcp-help"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${help_directory}"
      "${CMAKE_CURRENT_BINARY_DIR}/mapget.wheel/mapget/mcp-help" VERBATIM)
endif()
