include(FetchContent)

if (MAPGET_CONAN)
  find_package(spdlog        REQUIRED)
  find_package(Bitsery       REQUIRED)
  find_package(RocksDB       REQUIRED)
  find_package(httplib       REQUIRED)
  find_package(yaml-cpp      REQUIRED)
  find_package(CLI11         REQUIRED)
  find_package(pybind11      REQUIRED)
  find_package(simfil        REQUIRED)
  find_package(nlohmann_json REQUIRED)
else()
  FetchContent_Declare(spdlog
    GIT_REPOSITORY "https://github.com/gabime/spdlog.git"
    GIT_TAG        "v1.x"
    GIT_SHALLOW    ON
    FIND_PACKAGE_ARGS)

  FetchContent_Declare(bitsery
    GIT_REPOSITORY "https://github.com/fraillt/bitsery.git"
    GIT_TAG        "master"
    GIT_SHALLOW    ON
    FIND_PACKAGE_ARGS)

  FetchContent_Declare(cpp-httplib
    GIT_REPOSITORY "https://github.com/yhirose/cpp-httplib.git"
    GIT_TAG        "v0.14.3"
    GIT_SHALLOW    ON
    FIND_PACKAGE_ARGS NAMES)

  FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY "https://github.com/jbeder/yaml-cpp.git"
    GIT_TAG        "master"
    GIT_SHALLOW    ON
    FIND_PACKAGE_ARGS)

  FetchContent_Declare(cli11
    GIT_REPOSITORY "https://github.com/CLIUtils/CLI11"
    GIT_TAG        v2.3.2
    GIT_SHALLOW    ON
    FIND_PACKAGE_ARGS)

  FetchContent_Declare(pybind11
    GIT_REPOSITORY "https://github.com/pybind/pybind11.git"
    GIT_TAG        v2.11.1
    GIT_SHALLOW    ON
    FIND_PACKAGE_ARGS)

  set(WITH_GFLAGS NO CACHE BOOL "rocksdb without gflags")
  set(WITH_TESTS NO CACHE BOOL "rocksdb without tests")
  set(WITH_BENCHMARK_TOOLS NO CACHE BOOL "rocksdb without benchmarking")
  set(BENCHMARK_ENABLE_GTEST_TESTS NO CACHE BOOL "rocksdb without gtest")
  set(DISABLE_WARNING_AS_ERROR 1 CACHE BOOL "rocksdb warnings are ok")
  FetchContent_Declare(rocksdb
    GIT_REPOSITORY "https://github.com/facebook/rocksdb.git"
    GIT_TAG        dc87847e65449ef1cb6f787c5d753cbe8562bff1 # Use version greater than v8.6.7 once released.
    GIT_SHALLOW    OFF
    FIND_PACKAGE_ARGS NAMES RocksDB)


  set(SIMFIL_WITH_MODEL_JSON YES CACHE BOOL "Simfil with JSON support")
  set(SIMFIL_SHARED          NO  CACHE BOOL "Simfil as static library")
  FetchContent_Declare(simfil
    GIT_REPOSITORY "https://github.com/Klebert-Engineering/simfil.git"
    GIT_TAG        "main"
    GIT_SHALLOW    ON
    FIND_PACKAGE_ARGS)

  FetchContent_MakeAvailable(spdlog bitsery tiny-process-library stx simfil)

  if (MAPGET_WITH_WHEEL OR MAPGET_WITH_HTTPLIB OR MAPGET_ENABLE_TESTING)
    FetchContent_MakeAvailable(cpp-httplib yaml-cpp cli11 rocksdb pybind11)
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

FetchContent_Declare(python-cmake-wheel
  GIT_REPOSITORY "https://github.com/klebert-engineering/python-cmake-wheel.git"
  GIT_TAG        "v0.9.0"
  GIT_SHALLOW    ON)
FetchContent_MakeAvailable(python-cmake-wheel)

FetchContent_Declare(stx
  GIT_REPOSITORY "https://github.com/Klebert-Engineering/stx.git"
  GIT_TAG        "main"
  GIT_SHALLOW    ON)
FetchContent_MakeAvailable(stx)

set(BUILD_TESTING NO CACHE BOOL "")
FetchContent_Declare(tiny-process-library
  GIT_REPOSITORY "https://gitlab.com/eidheim/tiny-process-library"
  GIT_TAG        v2.0.4
  GIT_SHALLOW    ON)
FetchContent_MakeAvailable(tiny-process-library)
