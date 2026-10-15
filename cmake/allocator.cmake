# Allocator replacement belongs to executables, never to a library loaded into
# an existing process (notably Python). Keep the model-only/WASM build untouched.
set(_mapget_jemalloc_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING
    AND (MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING))
  set(_mapget_jemalloc_default ON)
endif()
option(MAPGET_WITH_JEMALLOC "Use jemalloc in native Linux server executables" ${_mapget_jemalloc_default})
unset(_mapget_jemalloc_default)

if(MAPGET_WITH_JEMALLOC)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "MAPGET_WITH_JEMALLOC currently supports native Linux builds only")
  endif()

  # The release archive includes configure; Autoconf is not a build dependency.
  CPMAddPackage(
    NAME jemalloc
    VERSION 5.3.0
    URL https://github.com/jemalloc/jemalloc/releases/download/5.3.0/jemalloc-5.3.0.tar.bz2
    URL_HASH SHA256=2db82d1e7119df3e71b7640219b6dfe84789bc0537983c3b7ac4f7189aecfeaa
    DOWNLOAD_ONLY YES)
  find_program(MAPGET_JEMALLOC_MAKE NAMES gmake make REQUIRED)
  include(ExternalProject)
  ExternalProject_Add(mapget-jemalloc-build
    SOURCE_DIR "${jemalloc_SOURCE_DIR}"
    BINARY_DIR "${jemalloc_BINARY_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
      "CC=${CMAKE_C_COMPILER}"
      "CFLAGS=-O2 -g -fPIC"
      "${jemalloc_SOURCE_DIR}/configure"
      --disable-static --disable-cxx --enable-stats
      --with-malloc-conf=background_thread:true
    BUILD_COMMAND "${MAPGET_JEMALLOC_MAKE}" build_lib_shared
    INSTALL_COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      <BINARY_DIR>/lib/libjemalloc.so.2 "${MAPGET_DEPLOY_DIR}/libjemalloc.so.2"
    BUILD_BYPRODUCTS "${MAPGET_DEPLOY_DIR}/libjemalloc.so.2"
    LOG_CONFIGURE YES
    LOG_BUILD YES
    LOG_OUTPUT_ON_FAILURE YES)

  add_library(mapget-jemalloc SHARED IMPORTED GLOBAL)
  set_target_properties(mapget-jemalloc PROPERTIES
    IMPORTED_LOCATION "${MAPGET_DEPLOY_DIR}/libjemalloc.so.2")
  add_dependencies(mapget-jemalloc mapget-jemalloc-build)
  install(FILES "${MAPGET_DEPLOY_DIR}/libjemalloc.so.2" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  install(FILES "${jemalloc_SOURCE_DIR}/COPYING"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/jemalloc")
  configure_file("${jemalloc_SOURCE_DIR}/COPYING" "${MAPGET_DEPLOY_DIR}/jemalloc-COPYING" COPYONLY)
endif()

# Opt an executable into process-wide allocation, including its loaded DSOs.
# --as-needed must not discard the allocator merely because malloc is called
# only by dependencies. Scope the linker state so other libraries are unaffected.
function(mapget_target_use_allocator target)
  if(NOT MAPGET_WITH_JEMALLOC)
    return()
  endif()
  get_target_property(_type "${target}" TYPE)
  if(NOT _type STREQUAL "EXECUTABLE")
    message(FATAL_ERROR "mapget_target_use_allocator requires an executable")
  endif()
  target_link_libraries("${target}" PRIVATE
    "-Wl,--push-state,--no-as-needed" mapget-jemalloc "-Wl,--pop-state")
  set_property(TARGET "${target}" APPEND PROPERTY BUILD_RPATH "$ORIGIN")
endfunction()
