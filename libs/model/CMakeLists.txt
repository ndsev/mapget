project(mapget-model)

add_library(mapget-model STATIC
  include/mapget/model/fields.h
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

  src/fields.cpp
  src/layer.cpp
  src/featurelayer.cpp
  src/info.cpp
  src/feature.cpp
  src/attr.cpp
  src/attrlayer.cpp
  src/tileid.cpp
  src/featureid.cpp
  src/stream.cpp
  src/relation.cpp)

target_include_directories(mapget-model
  PUBLIC
    include
  PRIVATE
    include/mapget/model)

target_link_libraries(mapget-model
  PUBLIC
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
