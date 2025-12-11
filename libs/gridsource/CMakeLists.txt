project(gridsource CXX)

add_library(gridsource STATIC
    include/gridsource/gridsource.h
    src/gridsource.cpp)

target_include_directories(gridsource
    PUBLIC
        include)

target_link_libraries(gridsource
    PUBLIC
        mapget-model
        mapget-service)

if (MSVC)
    target_compile_definitions(gridsource
        PRIVATE
            _CRT_SECURE_NO_WARNINGS)
    set_target_properties(gridsource PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if (WITH_COVERAGE)
    append_coverage_compiler_flags_to_target(gridsource)
endif()

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
    target_link_libraries(gridsource PUBLIC Threads::Threads)
endif()
