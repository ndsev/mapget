include(FetchContent)

if (MAPGET_WITH_SERVICE OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
  set(WANTS_ROCKSDB YES)
else()
  set(WANTS_ROCKSDB NO)
endif()

if (MAPGET_CONAN)
  find_package(spdlog        CONFIG REQUIRED)
  find_package(Bitsery       CONFIG REQUIRED)
  find_package(simfil        CONFIG REQUIRED)
  find_package(nlohmann_json CONFIG REQUIRED)
  if (MAPGET_WITH_HTTPLIB)
    find_package(httplib     CONFIG REQUIRED)
    find_package(yaml-cpp    CONFIG REQUIRED)
    find_package(CLI11       CONFIG REQUIRED)
  endif()
  if (MAPGET_WITH_WHEEL)
    find_package(pybind11    CONFIG REQUIRED)
  endif()
  if (WANTS_ROCKSDB)
    find_package(RocksDB     CONFIG REQUIRED)
  endif()
else()
  FetchContent_Declare(fmt
    GIT_REPOSITORY "https://github.com/fmtlib/fmt.git"
    GIT_TAG        "10.0.0"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(fmt)

  FetchContent_Declare(spdlog
    GIT_REPOSITORY "https://github.com/gabime/spdlog.git"
    GIT_TAG        "v1.x"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(spdlog)

  FetchContent_Declare(bitsery
    GIT_REPOSITORY "https://github.com/fraillt/bitsery.git"
    GIT_TAG        "master"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(bitsery)

  FetchContent_Declare(cpp-httplib
    GIT_REPOSITORY "https://github.com/yhirose/cpp-httplib.git"
    GIT_TAG        "v0.14.3"
    GIT_SHALLOW    ON)

  FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY "https://github.com/jbeder/yaml-cpp.git"
    GIT_TAG        "master"
    GIT_SHALLOW    ON)

  FetchContent_Declare(cli11
    GIT_REPOSITORY "https://github.com/CLIUtils/CLI11"
    GIT_TAG        v2.3.2
    GIT_SHALLOW    ON)

  if (MAPGET_WITH_WHEEL AND NOT TARGET pybind11)
    FetchContent_Declare(pybind11
      GIT_REPOSITORY "https://github.com/pybind/pybind11.git"
      GIT_TAG        v2.11.1
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

  if (NOT TARGET simfil)
    set(SIMFIL_WITH_MODEL_JSON YES CACHE BOOL "Simfil with JSON support")
    set(SIMFIL_SHARED          NO  CACHE BOOL "Simfil as static library")
    FetchContent_Declare(simfil
      GIT_REPOSITORY "https://github.com/Klebert-Engineering/simfil.git"
      GIT_TAG        "v0.2.1"
      GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(simfil)
  endif()

  if (MAPGET_WITH_WHEEL OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
    FetchContent_MakeAvailable(cpp-httplib yaml-cpp cli11)
  endif()

  FetchContent_GetProperties(cpp-httplib)
  if (cpp_httplib_POPULATED)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(cpp-httplib
      INTERFACE
        CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(cpp-httplib INTERFACE OpenSSL::SSL)
  endif()
endif()

if (MAPGET_WITH_WHEEL)
  FetchContent_Declare(python-cmake-wheel
    GIT_REPOSITORY "https://github.com/klebert-engineering/python-cmake-wheel.git"
    GIT_TAG        "v0.9.0"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(python-cmake-wheel)
endif()

set(BUILD_TESTING NO CACHE BOOL "")
FetchContent_Declare(tiny-process-library
  GIT_REPOSITORY "https://gitlab.com/eidheim/tiny-process-library"
  GIT_TAG        v2.0.4
  GIT_SHALLOW    ON)
FetchContent_MakeAvailable(tiny-process-library)
