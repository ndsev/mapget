project(mapget-model CXX)

add_library(mapget-model STATIC
  include/mapget/model/stringpool.h
  include/mapget/model/layer.h
  include/mapget/model/featurelayer.h
  include/mapget/model/info.h
  include/mapget/model/feature.h
  include/mapget/model/attr.h
  include/mapget/model/attrlayer.h
  include/mapget/model/tileid.h
  include/mapget/model/featureid.h
  include/mapget/model/stream.h
  include/mapget/model/relation.h
  include/mapget/model/geometry.h
  include/mapget/model/simfil-geometry.h
  include/mapget/model/sourcedata.h
  include/mapget/model/sourcedatalayer.h
  include/mapget/model/sourceinfo.h
  include/mapget/model/sourcedataaddress.h

  src/stringpool.cpp
  src/layer.cpp
  src/featurelayer.cpp
  src/info.cpp
  src/feature.cpp
  src/attr.cpp
  src/attrlayer.cpp
  src/tileid.cpp
  src/featureid.cpp
  src/relation.cpp
  src/stream.cpp
  src/geometry.cpp
  src/point.cpp
  src/simfil-geometry.cpp
  src/simfilutil.h
  src/sourcedata.cpp
  src/sourcedatalayer.cpp
  src/sourcedataaddress.cpp)

target_include_directories(mapget-model
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${MAPGET_INSTALL_INCLUDEDIR}>
  PRIVATE
    include/mapget/model)

target_link_libraries(mapget-model
  PUBLIC
    glm::glm
    mapget-log
    sfl::sfl
    Bitsery::bitsery
    simfil::simfil
    nlohmann_json::nlohmann_json)

if (MSVC)
  target_compile_definitions(mapget-model
    PRIVATE
      _CRT_SECURE_NO_WARNINGS)

  set_target_properties(mapget-model PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if (WITH_COVERAGE)
  append_coverage_compiler_flags_to_target(mapget-model)
endif()

install(TARGETS mapget-model)
install(DIRECTORY include/mapget/model
  DESTINATION ${MAPGET_INSTALL_INCLUDEDIR})
