#!/usr/bin/env python3

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy, get
import os


required_conan_version = ">=1.62.0"


# NOTE: The recipe used for conan-center-index is a copy of this
#       file with the version removed! Make sure to copy changes
#       in this file over to the conan-center-index recipe.
class MapgetRecipe(ConanFile):
    name = "mapget"
    version = "dev"
    url = "https://github.com/conan-io/conan-center-index"
    homepage = "https://github.com/ndsev/mapget"
    license = "BSD-3-Clause"
    topics = ["map"]

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_service": [True, False],
        "with_httplib": [True, False],
        "with_wheel": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_service": True,
        "with_httplib": True,
        "with_wheel": False,
        "simfil/*:with_json": True,
        "simfil/*:shared": False,
        "cpp-httplib/*:with_openssl": True,
        "cpp-httplib/*:with_zlib": True,
    }

    exports_sources = "CMakeLists.txt", "*.cmake", "libs/*", "apps/*", "test/*", "examples/*", "LICENSE"
    def validate(self):
        check_min_cppstd(self, "20")

    def requirements(self):
        self.requires("fmt/10.2.1", override=True)
        self.requires("simfil/0.1.2", transitive_headers=True)
        self.requires("spdlog/[~1]", transitive_headers=True)
        self.requires("bitsery/[~5]")
        self.requires("nlohmann_json/3.11.2", transitive_headers=True)
        if self.options.with_httplib:
            self.requires("cli11/2.3.2")
            self.requires("cpp-httplib/0.15.3", transitive_headers=True)
            self.requires("yaml-cpp/0.8.0")
        if self.options.with_service or self.options.with_httplib:
            self.requires("rocksdb/8.8.1")
        if self.options.with_wheel:
            self.requires("pybind11/2.11.1")

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["MAPGET_CONAN"] = True
        tc.cache_variables["MAPGET_ENABLE_TESTING"] = False
        tc.cache_variables["MAPGET_BUILD_EXAMPLES"] = False
        tc.cache_variables["MAPGET_WITH_SERVICE"] = self.options.with_service
        tc.cache_variables["MAPGET_WITH_HTTPLIB"] = self.options.with_httplib
        tc.cache_variables["MAPGET_WITH_WHEEL"] = self.options.with_wheel
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.components["log"].libs = ["mapget-log"]
        self.cpp_info.components["log"].set_property("cmake_target_name", "mapget::log")
        self.cpp_info.components["log"].requires = ["spdlog::spdlog"]
        self.cpp_info.components["model"].libs = ["mapget-model"]
        self.cpp_info.components["model"].requires = ["log", "simfil::simfil", "nlohmann_json::nlohmann_json", "bitsery::bitsery"]
        self.cpp_info.components["model"].set_property("cmake_target_name", "mapget::model")
        if self.options.with_service or self.options.with_httplib:
            self.cpp_info.components["service"].libs = ["mapget-service"]
            self.cpp_info.components["service"].set_property("cmake_target_name", "mapget::service")
            self.cpp_info.components["service"].requires = ["log", "model", "rocksdb::rocksdb", "yaml-cpp::yaml-cpp"]
        if self.options.with_httplib:
            self.cpp_info.components["http-service"].libs = ["mapget-http-service"]
            self.cpp_info.components["http-service"].set_property("cmake_target_name", "mapget::http-service")
            self.cpp_info.components["http-service"].requires = ["service", "http-datasource", "cpp-httplib::cpp-httplib", "cli11::cli11"]
            self.cpp_info.components["http-datasource"].libs = ["mapget-http-datasource"]
            self.cpp_info.components["http-datasource"].set_property("cmake_target_name", "mapget::http-datasource")
            self.cpp_info.components["http-datasource"].requires = ["model", "service", "cpp-httplib::cpp-httplib"]
            if self.options.with_wheel:
                self.cpp_info.components["pymapget"].libs = []
                self.cpp_info.components["pymapget"].requires = ["model", "http-datasource", "pybind11::pybind11"]
