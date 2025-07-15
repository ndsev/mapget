# sqlite.cmake - Reusable SQLite integration module
# This module provides a clean way to integrate SQLite into CMake projects

include(FetchContent)

#[=======================================================================[.rst:
add_sqlite
----------

Downloads and integrates SQLite amalgamation into your project.

Synopsis
^^^^^^^^
.. code-block:: cmake

  add_sqlite(
    [VERSION <version>]
    [RELEASE_YEAR <year>]
    [TARGET_NAME <name>]
    [NAMESPACE <namespace>]
    [ENABLE_FTS5 <ON|OFF>]
    [ENABLE_RTREE <ON|OFF>]
    [ENABLE_JSON1 <ON|OFF>]
    [ENABLE_MATH <ON|OFF>]
    [ENABLE_COLUMN_METADATA <ON|OFF>]
    [THREADSAFE <0|1|2>]
    [SHARED]
  )

Options
^^^^^^^
``VERSION``
  SQLite version to download (default: 3.50.2)

``RELEASE_YEAR``
  Year of SQLite release for download URL (default: 2025)
  SQLite uses year-based directories for downloads

``TARGET_NAME``
  Name of the created target (default: sqlite3)

``NAMESPACE``
  Namespace for alias target (default: SQLite)
  Creates ${NAMESPACE}::${TARGET_NAME} alias

``ENABLE_FTS5``
  Enable Full-Text Search 5 (default: ON)

``ENABLE_RTREE``
  Enable R*Tree index (default: ON)

``ENABLE_JSON1``
  Enable JSON1 extension (default: ON)

``ENABLE_MATH``
  Enable math functions (default: ON)

``ENABLE_COLUMN_METADATA``
  Enable column metadata functions (default: ON)

``THREADSAFE``
  Thread-safety level: 0=single-threaded, 1=serialized, 2=multi-threaded (default: 1)

``SHARED``
  Build as shared library instead of static

Example
^^^^^^^
.. code-block:: cmake

  include(cmake/sqlite.cmake)
  
  add_sqlite(
    VERSION 3.50.2
    RELEASE_YEAR 2025
    ENABLE_FTS5 ON
    ENABLE_RTREE ON
  )
  
  target_link_libraries(myapp PRIVATE SQLite::sqlite3)

#]=======================================================================]

function(add_sqlite)
    set(options SHARED ENABLE_FTS5 ENABLE_RTREE ENABLE_JSON1 ENABLE_MATH ENABLE_COLUMN_METADATA)
    set(oneValueArgs VERSION RELEASE_YEAR TARGET_NAME NAMESPACE THREADSAFE)
    cmake_parse_arguments(SQLITE "${options}" "${oneValueArgs}" "" ${ARGN})
    
    # Set defaults
    if(NOT DEFINED SQLITE_VERSION)
        set(SQLITE_VERSION "3.50.2")
    endif()
    
    if(NOT DEFINED SQLITE_RELEASE_YEAR)
        set(SQLITE_RELEASE_YEAR "2025")
    endif()
    
    if(NOT DEFINED SQLITE_TARGET_NAME)
        set(SQLITE_TARGET_NAME "sqlite3")
    endif()
    
    if(NOT DEFINED SQLITE_NAMESPACE)
        set(SQLITE_NAMESPACE "SQLite")
    endif()
    
    if(NOT DEFINED SQLITE_THREADSAFE)
        set(SQLITE_THREADSAFE "1")
    endif()
    
    # Default features to ON
    foreach(feature IN ITEMS ENABLE_FTS5 ENABLE_RTREE ENABLE_JSON1 ENABLE_MATH ENABLE_COLUMN_METADATA)
        if(NOT DEFINED SQLITE_${feature})
            set(SQLITE_${feature} ON)
        endif()
    endforeach()
    
    # Determine library type
    if(SQLITE_SHARED)
        set(SQLITE_LIB_TYPE SHARED)
    else()
        set(SQLITE_LIB_TYPE STATIC)
    endif()
    
    # Check if target already exists
    if(TARGET ${SQLITE_TARGET_NAME})
        message(WARNING "add_sqlite: Target '${SQLITE_TARGET_NAME}' already exists. Skipping SQLite configuration.")
        return()
    endif()
    
    if(TARGET ${SQLITE_NAMESPACE}::${SQLITE_TARGET_NAME})
        message(WARNING "add_sqlite: Target '${SQLITE_NAMESPACE}::${SQLITE_TARGET_NAME}' already exists. Skipping SQLite configuration.")
        return()
    endif()
    
    # Convert version to amalgamation format (3.46.1 -> 3460100)
    string(REPLACE "." ";" VERSION_LIST ${SQLITE_VERSION})
    list(GET VERSION_LIST 0 VERSION_MAJOR)
    list(GET VERSION_LIST 1 VERSION_MINOR)
    list(GET VERSION_LIST 2 VERSION_PATCH)
    math(EXPR AMALGAMATION_VERSION "${VERSION_MAJOR} * 1000000 + ${VERSION_MINOR} * 10000 + ${VERSION_PATCH} * 100")
    
    # Download URL using the provided release year
    set(SQLITE_URL "https://www.sqlite.org/${SQLITE_RELEASE_YEAR}/sqlite-amalgamation-${AMALGAMATION_VERSION}.zip")
    
    # Declare SQLite
    FetchContent_Declare(
        sqlite_amalgamation
        URL ${SQLITE_URL}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    
    # Get the source
    FetchContent_GetProperties(sqlite_amalgamation)
    if(NOT sqlite_amalgamation_POPULATED)
        message(STATUS "Downloading SQLite ${SQLITE_VERSION} (${SQLITE_RELEASE_YEAR}) from ${SQLITE_URL}")
        FetchContent_Populate(sqlite_amalgamation)
        
        # Create a minimal CMakeLists.txt for SQLite
        set(SQLITE_CMAKE_CONTENT "
cmake_minimum_required(VERSION 3.14)
project(${SQLITE_TARGET_NAME} C)

# Create SQLite library
add_library(${SQLITE_TARGET_NAME} ${SQLITE_LIB_TYPE} sqlite3.c)

# Set include directories
target_include_directories(${SQLITE_TARGET_NAME} PUBLIC 
    $<BUILD_INTERFACE:\${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# Set compile definitions
target_compile_definitions(${SQLITE_TARGET_NAME} PRIVATE
    SQLITE_THREADSAFE=${SQLITE_THREADSAFE}
")
        
        # Add optional features
        if(SQLITE_ENABLE_FTS5)
            string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_ENABLE_FTS5=1\n")
        endif()
        
        if(SQLITE_ENABLE_RTREE)
            string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_ENABLE_RTREE=1\n")
            string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_ENABLE_GEOPOLY=1\n")
        endif()
        
        if(SQLITE_ENABLE_JSON1)
            string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_ENABLE_JSON1=1\n")
        endif()
        
        if(SQLITE_ENABLE_MATH)
            string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_ENABLE_MATH_FUNCTIONS=1\n")
        endif()
        
        if(SQLITE_ENABLE_COLUMN_METADATA)
            string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_ENABLE_COLUMN_METADATA=1\n")
        endif()
        
        # Add common useful defines
        string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_DQS=0\n")
        string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_DEFAULT_MEMSTATUS=0\n")
        string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1\n")
        string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_LIKE_DOESNT_MATCH_BLOBS\n")
        string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_MAX_EXPR_DEPTH=0\n")
        string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_OMIT_DEPRECATED\n")
        string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_OMIT_SHARED_CACHE\n")
        string(APPEND SQLITE_CMAKE_CONTENT "    SQLITE_USE_ALLOCA\n")
        
        string(APPEND SQLITE_CMAKE_CONTENT ")

# Platform-specific settings
if(WIN32)
    target_compile_definitions(${SQLITE_TARGET_NAME} PRIVATE SQLITE_OS_WIN=1)
elseif(UNIX)
    target_compile_definitions(${SQLITE_TARGET_NAME} PRIVATE SQLITE_OS_UNIX=1)
    
    # Check for and link required libraries
    include(CheckLibraryExists)
    
    # Math library
    if(${SQLITE_ENABLE_MATH})
        check_library_exists(m sqrt \"\" HAVE_LIB_M)
        if(HAVE_LIB_M)
            target_link_libraries(${SQLITE_TARGET_NAME} PUBLIC m)
        endif()
    endif()
    
    # Dynamic loading library
    check_library_exists(dl dlopen \"\" HAVE_LIB_DL)
    if(HAVE_LIB_DL)
        target_link_libraries(${SQLITE_TARGET_NAME} PUBLIC dl)
    endif()
    
    # Threading library
    if(${SQLITE_THREADSAFE} GREATER 0)
        find_package(Threads REQUIRED)
        target_link_libraries(${SQLITE_TARGET_NAME} PUBLIC Threads::Threads)
    endif()
endif()

# Set properties
set_target_properties(${SQLITE_TARGET_NAME} PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# Create alias target
add_library(${SQLITE_NAMESPACE}::${SQLITE_TARGET_NAME} ALIAS ${SQLITE_TARGET_NAME})
")
        
        # Write the CMakeLists.txt
        file(WRITE ${sqlite_amalgamation_SOURCE_DIR}/CMakeLists.txt "${SQLITE_CMAKE_CONTENT}")
    endif()
    
    # Add the subdirectory
    add_subdirectory(${sqlite_amalgamation_SOURCE_DIR} ${sqlite_amalgamation_BINARY_DIR} EXCLUDE_FROM_ALL)
    
    # Make the targets available in parent scope
    set(${SQLITE_TARGET_NAME}_FOUND TRUE PARENT_SCOPE)
    
    message(STATUS "SQLite ${SQLITE_VERSION} (${SQLITE_RELEASE_YEAR}) configured successfully")
    message(STATUS "  Target: ${SQLITE_TARGET_NAME}")
    message(STATUS "  Alias: ${SQLITE_NAMESPACE}::${SQLITE_TARGET_NAME}")
    message(STATUS "  Type: ${SQLITE_LIB_TYPE}")
    message(STATUS "  Features:")
    message(STATUS "    FTS5: ${SQLITE_ENABLE_FTS5}")
    message(STATUS "    RTree: ${SQLITE_ENABLE_RTREE}")
    message(STATUS "    JSON1: ${SQLITE_ENABLE_JSON1}")
    message(STATUS "    Math: ${SQLITE_ENABLE_MATH}")
    message(STATUS "    Column Metadata: ${SQLITE_ENABLE_COLUMN_METADATA}")
    message(STATUS "    Thread Safety: ${SQLITE_THREADSAFE}")
endfunction()
