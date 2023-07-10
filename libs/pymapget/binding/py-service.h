#pragma once

#include "mapget/http-service/http-service.h"
#include "mapget/http-datasource/datasource-client.h"

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace py::literals;

void bindHttpService(py::module_& m)
{
    using mapget::HttpService;
    using mapget::RemoteDataSource;

    py::class_<HttpService>(m, "Service")
        .def(py::init<>(), R"pbdoc(
            Construct a Service with a default Cache instance.
        )pbdoc")
        .def("add_remote_datasource", 
            [](HttpService& self, const std::string& host, uint16_t port) 
            {
                self.add(std::make_shared<RemoteDataSource>(host, port));
            },
            R"pbdoc(
                Add a remote DataSourceClient.
            )pbdoc",
            py::arg("host"), py::arg("port"))
        .def("go", &HttpService::go,
             py::arg("interface") = "0.0.0.0",
             py::arg("port") = 0,
             py::arg("wait_ms") = 100,
             R"pbdoc(
                Launch the HttpService server in its own thread.
            )pbdoc")
        .def("is_running", &HttpService::isRunning,
             R"pbdoc(
                Returns true if this HttpService instance is currently running.
            )pbdoc")
        .def("stop", &HttpService::stop,
             R"pbdoc(
                Stop this HttpService instance.
            )pbdoc")
        .def("wait_for_signal", &HttpService::waitForSignal,
             py::call_guard<py::gil_scoped_release>(),
             R"pbdoc(
                Blocks until SIGINT or SIGTERM is received, then shuts down the server.
            )pbdoc")
        .def("port", &HttpService::port,
             R"pbdoc(
                Get the port currently used by the instance, or 0 if go() has never been called.
             )pbdoc")
        .def("mount", &HttpService::mountFileSystem,
             R"pbdoc(
                Add a filesystem mount point in the format `<url-path-prefix>:<filesystem-path>`.
                Returns true if successful, false otherwise.
             )pbdoc");
}
