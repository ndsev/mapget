include(FetchContent)

if (MAPGET_WITH_SERVICE OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
  set(WANTS_ROCKSDB YES)
else()
  set(WANTS_ROCKSDB NO)
endif()

if (MAPGET_CONAN)
  find_package(spdlog        CONFIG REQUIRED)
  find_package(Bitsery       CONFIG REQUIRED)
  find_package(nlohmann_json CONFIG REQUIRED)
  find_package(glm           CONFIG REQUIRED)
  if (MAPGET_WITH_HTTPLIB)
    find_package(httplib     CONFIG REQUIRED)
    find_package(yaml-cpp    CONFIG REQUIRED)
    find_package(CLI11       CONFIG REQUIRED)
    find_package(nlohmann_json_schema_validator CONFIG REQUIRED)
    find_package(picosha2    CONFIG REQUIRED)
  endif()
  if (MAPGET_WITH_WHEEL)
    find_package(pybind11    CONFIG REQUIRED)
  endif()
  if (WANTS_ROCKSDB)
    find_package(RocksDB     CONFIG REQUIRED)
  endif()
else()
  FetchContent_Declare(glm
    GIT_REPOSITORY "https://github.com/g-truc/glm.git"
    GIT_TAG        "1.0.1"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(glm)

  FetchContent_Declare(fmt
    GIT_REPOSITORY "https://github.com/fmtlib/fmt.git"
    GIT_TAG        "11.1.3"
    GIT_SHALLOW    ON
    CMAKE_ARGS     -DFMT_HEADER_ONLY=OFF)
  FetchContent_MakeAvailable(fmt)

  set (SPDLOG_FMT_EXTERNAL ON)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY "https://github.com/gabime/spdlog.git"
    GIT_TAG        "v1.x"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(spdlog)

  if (NOT TARGET Bitsery::bitsery)
    FetchContent_Declare(bitsery
      GIT_REPOSITORY "https://github.com/fraillt/bitsery.git"
      GIT_TAG        "v5.2.4"
      GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(bitsery)
  endif()

  FetchContent_Declare(cpp-httplib
    GIT_REPOSITORY "https://github.com/yhirose/cpp-httplib.git"
    GIT_TAG        "v0.15.3"
    GIT_SHALLOW    ON)

  FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY "https://github.com/jbeder/yaml-cpp.git"
    GIT_TAG        "0.8.0"
    GIT_SHALLOW    ON)

  FetchContent_Declare(cli11
    GIT_REPOSITORY "https://github.com/CLIUtils/CLI11"
    GIT_TAG        "v2.3.2"
    GIT_SHALLOW    ON)

  FetchContent_Declare(nlohmann_json_schema_validator
    GIT_REPOSITORY "https://github.com/pboettch/json-schema-validator"
    GIT_TAG        "2.3.0"
    GIT_SHALLOW    ON)

  FetchContent_Declare(picosha2
    GIT_REPOSITORY "https://github.com/okdshin/PicoSHA2"
    GIT_TAG        "v1.0.1"
    GIT_SHALLOW    ON)

  if (MAPGET_WITH_WHEEL AND NOT TARGET pybind11)
    FetchContent_Declare(pybind11
      GIT_REPOSITORY "https://github.com/pybind/pybind11.git"
      GIT_TAG        "v2.13.6"
      GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(pybind11)
  endif()

  if (WANTS_ROCKSDB AND NOT TARGET rocksdb)
    block()
      set(WITH_GFLAGS NO CACHE BOOL "rocksdb without gflags")
      set(WITH_TESTS NO CACHE BOOL "rocksdb without tests")
      set(WITH_BENCHMARK_TOOLS NO CACHE BOOL "rocksdb without benchmarking")
      set(BENCHMARK_ENABLE_GTEST_TESTS NO CACHE BOOL "rocksdb without gtest")
      set(WITH_TOOLS NO CACHE BOOL "rocksdb without tools")
      if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # Due to a problem compiling rocksdb on GCC 14.1.1 we need to disable
        # deprecated declaration errors
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated-declarations")
      endif()
      set(FAIL_ON_WARNINGS YES CACHE BOOL "rocksdb warnings are ok")
      FetchContent_Declare(RocksDB
        GIT_REPOSITORY "https://github.com/facebook/rocksdb.git"
        GIT_TAG        "v9.1.0"
        GIT_SHALLOW    OFF)
      FetchContent_MakeAvailable(RocksDB)
      add_library(RocksDB::rocksdb ALIAS rocksdb)
    endblock()
  endif()
endif()

if (NOT MAPGET_CONAN)
  if (MAPGET_WITH_WHEEL OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
    FetchContent_MakeAvailable(cpp-httplib yaml-cpp cli11 nlohmann_json_schema_validator picosha2)
    add_library(picosha2::picosha2 ALIAS picosha2)
  endif()

  # Using option CPPHTTPLIB_USE_POLL to bypass hardcoded file descriptor limit on linux.
  # FD_SETSIZE is fixed to 1024 on linux, which is also used for sockets. Once an
  # application has opened more than 1024 files, cpp-httplib cannot respond to requests
  # anymore, even if some files are closed inbetween.
  # For details, see https://github.com/yhirose/cpp-httplib/issues/215
  FetchContent_GetProperties(cpp-httplib)
  if (cpp_httplib_POPULATED)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(cpp-httplib
      INTERFACE
        CPPHTTPLIB_OPENSSL_SUPPORT
        CPPHTTPLIB_USE_POLL)
    target_link_libraries(cpp-httplib INTERFACE OpenSSL::SSL)  
  endif()
endif()

if (NOT TARGET simfil)
  set(SIMFIL_WITH_MODEL_JSON YES CACHE BOOL "Simfil with JSON support")
  set(SIMFIL_SHARED          NO  CACHE BOOL "Simfil as static library")
  FetchContent_Declare(simfil
    GIT_REPOSITORY "https://github.com/Klebert-Engineering/simfil.git"
    # TODO: We want to have the simfil diagnostics feature, there is
    #       not yet an official release containing it -> activate main branch
    GIT_TAG        "main"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(simfil)
endif()

if (MAPGET_WITH_WHEEL)
  FetchContent_Declare(python-cmake-wheel
    GIT_REPOSITORY "https://github.com/klebert-engineering/python-cmake-wheel.git"
    GIT_TAG        "v0.9.0"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(python-cmake-wheel)
endif()

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(tiny-process-library
  GIT_REPOSITORY "https://gitlab.com/eidheim/tiny-process-library"
  GIT_TAG        "v2.0.4"
  GIT_SHALLOW    ON)
FetchContent_MakeAvailable(tiny-process-library)
