project(pymapget)

pybind11_add_module(pymapget
  MODULE
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
    mapget-model
    mapget-http-datasource
    mapget-http-service)

target_include_directories(pymapget
  PRIVATE
    binding)

target_compile_features(pymapget
  INTERFACE
    cxx_std_17)

FetchContent_GetProperties(cpp-httplib)
if (MSVC AND CPP-HTTPLIB_POPULATED)
  # Required because cpp-httplib speaks https via OpenSSL.
  # Only needed if httplib came via FetchContent.
  set(DEPLOY_FILES
    "${OPENSSL_INCLUDE_DIR}/../libcrypto-1_1-x64.dll"
    "${OPENSSL_INCLUDE_DIR}/../libssl-1_1-x64.dll"
    "${CMAKE_CURRENT_LIST_DIR}/__main__.py")
else()
  set(DEPLOY_FILES "${CMAKE_CURRENT_LIST_DIR}/__main__.py")
endif()

if (simfil_FOUND)
  # HACK: Workaround for Conan issue https://github.com/conan-io/conan/issues/12300
  block()
    if (GENERATOR_IS_MULTI_CONFIG)
      set(conan_libs "")
      set(conan_lib_dirs "")
      foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER "${type}" CFG)
        set(conan_libs
          ${conan_libs};$<$<CONFIG:{cfg}>:${simfil_LIBS_${CFG}}>)
        set(conan_lib_dirs
          ${conan_lib_dirs};$<$<CONFIG:${cfg}>:${simfil_LIB_DIRS_${CFG}}>)
      endforeach()
    else()
      set(type ${CMAKE_BUILD_TYPE})
      string(TOUPPER "${type}" type)
      set(conan_libs ${simfil_LIBS_${cfg}})
      set(conan_lib_dirs ${simfil_LIB_DIRS_${cfg}})
    endif()

    set(location "${conan_lib_dirs}/${conan_libs}")
    if (EXISTS "${location}${CMAKE_SHARED_LIBRARY_SUFFIX}")
      list(APPEND DEPLOY_FILES "${location}${CMAKE_SHARED_LIBRARY_SUFFIX}")
    else()
      list(APPEND DEPLOY_FILES "${location}${CMAKE_STATIC_LIBRARY_SUFFIX}")
    endif()
  endblock()
else()
  list(APPEND DEPLOY_FILES $<TARGET_FILE:simfil::simfil>)
endif()

add_wheel(pymapget
  NAME mapget
  AUTHOR "Klebert Engineering"
  URL "https://github.com/klebert-engineering/mapget"
  VERSION "${MAPGET_VERSION}"
  DESCRIPTION "Python bindings for the mapget library."
  TARGET_DEPENDENCIES mapget-model mapget-http-datasource
  LICENSE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../LICENSE"
  DEPLOY_FILES ${DEPLOY_FILES})
