#pragma once

#include "mapget/http-datasource/datasource-client.h"
#include "mapget/http-service/http-service.h"

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace py::literals;

void bindHttpService(py::module_& m)
{
    using mapget::HttpService;
    using mapget::HttpServiceConfig;
    using mapget::RemoteDataSource;

    py::class_<HttpService>(m, "Service", R"pbdoc(
        Embedded mapget HTTP service.

        Use this class to host local or remote datasources from Python. It
        exposes the same HTTP API as the `mapget serve` command once `go()` has
        started the server thread.
    )pbdoc")
        .def(
            py::init(
                [](size_t workerCount)
                {
                    HttpServiceConfig config;
                    config.workerCount = workerCount;
                    return std::make_unique<HttpService>(
                        std::make_shared<mapget::MemCache>(),
                        config);
                }),
            py::arg("worker_count") = mapget::Service::defaultWorkerCount(),
            R"pbdoc(
                Construct a service with an in-memory cache and a global worker cap.

                Every worker can load datasource tiles or evaluate derived filter
                results. A datasource's ``maxParallelJobs`` independently limits
                concurrent calls into that datasource.
            )pbdoc")
        .def(
            "add_remote_datasource",
            [](HttpService& self, const std::string& host, uint16_t port)
            { self.add(std::make_shared<RemoteDataSource>(host, port)); },
            R"pbdoc(
                Add a remote DataSourceClient.
            )pbdoc",
            py::arg("host"),
            py::arg("port"))
        .def(
            "go",
            &HttpService::go,
            py::arg("interface") = "0.0.0.0",
            py::arg("port") = 0,
            py::arg("wait_ms") = HttpService::DefaultStartupWaitMs,
            R"pbdoc(
                Launch the HttpService server in its own thread. The default
                startup wait is 5000 milliseconds.
            )pbdoc")
        .def(
            "is_running",
            &HttpService::isRunning,
            R"pbdoc(
                Returns true if this HttpService instance is currently running.
            )pbdoc")
        .def(
            "stop",
            &HttpService::stop,
            R"pbdoc(
                Stop this HttpService instance.
            )pbdoc")
        .def(
            "wait_for_signal",
            &HttpService::waitForSignal,
            py::call_guard<py::gil_scoped_release>(),
            R"pbdoc(
                Blocks until SIGINT or SIGTERM is received, then shuts down the server.
            )pbdoc")
        .def(
            "port",
            &HttpService::port,
            R"pbdoc(
                Get the port currently used by the instance, or 0 if go() has never been called.
             )pbdoc")
        .def(
            "mount",
            &HttpService::mountFileSystem,
            R"pbdoc(
                Add a filesystem mount point in the format `<url-path-prefix>:<filesystem-path>`.
                Returns true if successful, false otherwise.
             )pbdoc");
}
