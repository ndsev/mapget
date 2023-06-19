project(pymapget)

pybind11_add_module(mapget
  MODULE
    py-mapget.cpp
    binding/py-model.h
    binding/py-tileid.h
    binding/py-layer.h
    binding/py-datasource.h)

if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  target_compile_options(mapget PUBLIC /permissive-)
endif ()

target_link_libraries(mapget
  PRIVATE
    mapget-model
    mapget-http-datasource)

target_include_directories(mapget
  PRIVATE
    binding)

target_compile_features(mapget
  INTERFACE
    cxx_std_17)

if (MSVC)
  # Required because cpp-httplib speaks https via OpenSSL
  set(DEPLOY_FILES
    "${OPENSSL_INCLUDE_DIR}/../libcrypto-1_1-x64.dll"
    "${OPENSSL_INCLUDE_DIR}/../libssl-1_1-x64.dll")
endif ()

add_wheel(mapget
  AUTHOR "Klebert Engineering"
  URL "https://github.com/klebert-engineering/mapget"
  VERSION "${MAPGET_VERSION}"
  DESCRIPTION "Python bindings for the mapget library."
  TARGET_DEPENDENCIES mapget-model mapget-http-datasource
  LICENSE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../LICENSE"
  DEPLOY_FILES ${DEPLOY_FILES})
