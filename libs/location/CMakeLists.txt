project(mapget-location CXX)

option(MAPGET_BUNDLE_GEONAMES_LOCATION_DB "Generate bundled GeoNames cities5000 SQLite location database." ON)

add_library(mapget-location STATIC
  include/mapget/location/location.h

  src/location.cpp)

target_include_directories(mapget-location
  PUBLIC
    include
  PRIVATE
    include/mapget/location)

target_link_libraries(mapget-location
  PUBLIC
    mapget-log
    nlohmann_json::nlohmann_json
    SQLite::SQLite3
  PRIVATE
    ${CMAKE_DL_LIBS})

if (MSVC)
  target_compile_definitions(mapget-location
    PRIVATE
      _CRT_SECURE_NO_WARNINGS)

  set_target_properties(mapget-location PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if (WITH_COVERAGE)
  append_coverage_compiler_flags_to_target(mapget-location)
endif()

install(TARGETS mapget-location)
install(DIRECTORY include/mapget/location
  DESTINATION ${MAPGET_INSTALL_INCLUDEDIR})

if (MAPGET_BUNDLE_GEONAMES_LOCATION_DB)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  if (TARGET sqlite3)
    get_target_property(MAPGET_SQLITE3_SOURCE_DIR sqlite3 SOURCE_DIR)
    if (EXISTS "${MAPGET_SQLITE3_SOURCE_DIR}/shell.c")
      add_executable(mapget-sqlite-shell EXCLUDE_FROM_ALL
        "${MAPGET_SQLITE3_SOURCE_DIR}/shell.c")
      target_link_libraries(mapget-sqlite-shell
        PRIVATE
          SQLite::SQLite3)
    endif()
  endif()

  set(MAPGET_GEONAMES_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../data/geonames")
  set(MAPGET_GEONAMES_CITIES_TXT "${MAPGET_GEONAMES_DATA_DIR}/cities5000.txt")
  set(MAPGET_GEONAMES_README_TXT "${MAPGET_GEONAMES_DATA_DIR}/geonames-readme.txt")
  set(MAPGET_GEONAMES_IMPORT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/import_geonames.py")
  set(MAPGET_LOCATION_DB "${MAPGET_DEPLOY_DIR}/geonames-cities5000.sqlite")
  set(MAPGET_LOCATION_README "${MAPGET_DEPLOY_DIR}/geonames-readme.txt")

  if (EXISTS "${MAPGET_GEONAMES_CITIES_TXT}" AND EXISTS "${MAPGET_GEONAMES_README_TXT}")
    set(MAPGET_GEONAMES_IMPORT_COMMAND
      "${Python3_EXECUTABLE}" "${MAPGET_GEONAMES_IMPORT_SCRIPT}" "${MAPGET_GEONAMES_CITIES_TXT}" "${MAPGET_LOCATION_DB}")
    set(MAPGET_GEONAMES_IMPORT_DEPENDS
      "${MAPGET_GEONAMES_IMPORT_SCRIPT}"
      "${MAPGET_GEONAMES_CITIES_TXT}")
    if (TARGET mapget-sqlite-shell)
      list(APPEND MAPGET_GEONAMES_IMPORT_COMMAND "$<TARGET_FILE:mapget-sqlite-shell>")
      list(APPEND MAPGET_GEONAMES_IMPORT_DEPENDS mapget-sqlite-shell)
    endif()

    add_custom_command(
      OUTPUT "${MAPGET_LOCATION_DB}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${MAPGET_DEPLOY_DIR}"
      COMMAND ${MAPGET_GEONAMES_IMPORT_COMMAND}
      DEPENDS ${MAPGET_GEONAMES_IMPORT_DEPENDS}
      VERBATIM)

    add_custom_command(
      OUTPUT "${MAPGET_LOCATION_README}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${MAPGET_DEPLOY_DIR}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MAPGET_GEONAMES_README_TXT}" "${MAPGET_LOCATION_README}"
      DEPENDS "${MAPGET_GEONAMES_README_TXT}"
      VERBATIM)

    add_custom_target(mapget-location-database ALL
      DEPENDS
        "${MAPGET_LOCATION_DB}"
        "${MAPGET_LOCATION_README}")

    # Consumers such as the Python wheel can bundle the generated files without
    # duplicating knowledge of their deployment paths.
    set_property(TARGET mapget-location-database PROPERTY
      MAPGET_LOCATION_DEPLOY_FILES
      "${MAPGET_LOCATION_DB};${MAPGET_LOCATION_README}")
  else()
    message(WARNING "GeoNames cities5000 data not found; bundled location database target was not created.")
  endif()
endif()
