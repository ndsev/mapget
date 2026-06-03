project(mapget-location CXX)

option(MAPGET_BUNDLE_GEONAMES_LOCATION_DB "Generate bundled GeoNames cities1000 SQLite location database." ON)

add_library(mapget-location STATIC
  include/mapget/location/geonames-import.h
  include/mapget/location/location.h

  src/geonames-import.cpp
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
    SQLite::SQLite3)

add_executable(mapget-geonames-import
  src/geonames-import-main.cpp)

target_link_libraries(mapget-geonames-import
  PRIVATE
    mapget-location)

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

install(TARGETS mapget-location mapget-geonames-import)
install(DIRECTORY include/mapget/location
  DESTINATION ${MAPGET_INSTALL_INCLUDEDIR})

if (MAPGET_BUNDLE_GEONAMES_LOCATION_DB)
  set(MAPGET_GEONAMES_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../data/geonames")
  set(MAPGET_GEONAMES_CITIES_TXT "${MAPGET_GEONAMES_DATA_DIR}/cities1000.txt")
  set(MAPGET_GEONAMES_README_TXT "${MAPGET_GEONAMES_DATA_DIR}/geonames-readme.txt")
  set(MAPGET_LOCATION_DB "${MAPGET_DEPLOY_DIR}/geonames-cities1000.sqlite")
  set(MAPGET_LOCATION_README "${MAPGET_DEPLOY_DIR}/geonames-readme.txt")

  if (EXISTS "${MAPGET_GEONAMES_CITIES_TXT}" AND EXISTS "${MAPGET_GEONAMES_README_TXT}")
    add_custom_command(
      OUTPUT "${MAPGET_LOCATION_DB}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${MAPGET_DEPLOY_DIR}"
      COMMAND $<TARGET_FILE:mapget-geonames-import> "${MAPGET_GEONAMES_CITIES_TXT}" "${MAPGET_LOCATION_DB}"
      DEPENDS
        mapget-geonames-import
        "${MAPGET_GEONAMES_CITIES_TXT}")

    add_custom_command(
      OUTPUT "${MAPGET_LOCATION_README}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${MAPGET_DEPLOY_DIR}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MAPGET_GEONAMES_README_TXT}" "${MAPGET_LOCATION_README}"
      DEPENDS "${MAPGET_GEONAMES_README_TXT}")

    add_custom_target(mapget-location-database ALL
      DEPENDS
        "${MAPGET_LOCATION_DB}"
        "${MAPGET_LOCATION_README}")
  else()
    message(WARNING "GeoNames cities1000 data not found; bundled location database target was not created.")
  endif()
endif()
