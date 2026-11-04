project(mapget-location CXX)

# Server-only dependency: model/WASM builds do not acquire a gazetteer or schema compiler.
set(MAPGET_PLAZS_SOURCE_DIR "" CACHE PATH "Local plazs checkout instead of the pinned dependency")
if(MAPGET_PLAZS_SOURCE_DIR)
  CPMAddPackage(NAME plazs SOURCE_DIR "${MAPGET_PLAZS_SOURCE_DIR}"
    OPTIONS "PLAZS_BUILD_TESTS OFF" "PLAZS_BUILD_TOOLS OFF")
else()
  CPMAddPackage(NAME plazs GITHUB_REPOSITORY Klebert-Engineering/plazs
    GIT_TAG 0707714a9dbc6d5a9caf2485d312add182cb0d58
    GIT_CONFIG "filter.lfs.required=true"
      "filter.lfs.smudge=git-lfs smudge -- %f" "filter.lfs.process=git-lfs filter-process"
    OPTIONS "PLAZS_BUILD_TESTS OFF" "PLAZS_BUILD_TOOLS OFF")
endif()

add_library(mapget-location STATIC include/mapget/location/location.h src/location.cpp)
target_include_directories(mapget-location PUBLIC include PRIVATE include/mapget/location)
target_link_libraries(mapget-location
  PUBLIC mapget-log nlohmann_json::nlohmann_json
  PRIVATE plazs::plazs ${CMAKE_DL_LIBS})

if (MSVC)
  target_compile_definitions(mapget-location PRIVATE _CRT_SECURE_NO_WARNINGS)
  set_target_properties(mapget-location PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
if (WITH_COVERAGE)
  append_coverage_compiler_flags_to_target(mapget-location)
endif()

install(TARGETS mapget-location)
install(DIRECTORY include/mapget/location DESTINATION ${MAPGET_INSTALL_INCLUDEDIR})

# Bundle a pinned prepared artifact by default. Importing WOF's raw multi-GB
# inventory remains an explicit offline plazs task, never part of this build.
option(MAPGET_BUNDLE_LOCATION_DB "Bundle the prepared offline place gazetteer" ON)
set(MAPGET_LOCATION_DATABASE_FILE "" CACHE FILEPATH "Prepared WOF location database to bundle with mapget")
if (MAPGET_BUNDLE_LOCATION_DB)
  if (MAPGET_LOCATION_DATABASE_FILE)
    set(_location_database "${MAPGET_LOCATION_DATABASE_FILE}")
  else()
    include("${plazs_SOURCE_DIR}/cmake/dataset.cmake")
    plazs_fetch_dataset(_location_database)
  endif()
  if (NOT EXISTS "${_location_database}")
    message(FATAL_ERROR "Prepared location database does not exist: ${_location_database}")
  endif()
  set(MAPGET_LOCATION_DB "${MAPGET_DEPLOY_DIR}/mapget-places.sqlite")
  add_custom_command(OUTPUT "${MAPGET_LOCATION_DB}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MAPGET_DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_location_database}" "${MAPGET_LOCATION_DB}"
    DEPENDS "${_location_database}" VERBATIM)
  add_custom_target(mapget-location-database ALL DEPENDS "${MAPGET_LOCATION_DB}")
  set_property(TARGET mapget-location-database PROPERTY MAPGET_LOCATION_DEPLOY_FILES "${MAPGET_LOCATION_DB}")
  install(FILES "${_location_database}" DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mapget-places.sqlite)
endif()
