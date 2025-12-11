project(geojsonsource CXX)

add_library(geojsonsource STATIC
    include/geojsonsource/geojsonsource.h
    src/geojsonsource.cpp)

target_include_directories(geojsonsource
    PUBLIC
        include)

target_link_libraries(geojsonsource
    PUBLIC
        mapget-model
        mapget-service)

if (MSVC)
    target_compile_definitions(geojsonsource
        PRIVATE
            _CRT_SECURE_NO_WARNINGS)
    set_target_properties(geojsonsource PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if (WITH_COVERAGE)
    append_coverage_compiler_flags_to_target(geojsonsource)
endif()

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
    target_link_libraries(geojsonsource PUBLIC Threads::Threads)
endif()
