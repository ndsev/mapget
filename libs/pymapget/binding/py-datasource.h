#pragma once

#include "mapget/http-datasource/datasource-server.h"

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace py::literals;

void bindDataSourceServer(py::module_& m)
{
    using namespace mapget;
    using namespace simfil;

    py::class_<DataSourceServer, std::shared_ptr<DataSourceServer>>(m, "DataSourceServer")
        .def(
            py::init(
                [](py::dict const& dict)
                {
                    // import json.dumps
                    py::module json_module = py::module::import("json");
                    py::function json_dumps = json_module.attr("dumps");

                    // convert py::dict to JSON string
                    auto json_str = json_dumps(dict).cast<std::string>();

                    // parse JSON string into nlohmann::json
                    nlohmann::json j = nlohmann::json::parse(json_str);

                    // construct DataSource
                    return std::make_unique<DataSourceServer>(DataSourceInfo::fromJson(j));
                }),
            R"pbdoc(
                Construct a DataSource with a DataSourceInfo metadata instance.
            )pbdoc",
            py::arg("info_dict"))
        .def(
            "on_tile_request",
            &DataSourceServer::onTileRequest,
            py::arg("callback"),
            py::call_guard<py::gil_scoped_acquire>(),
            R"pbdoc(
            Set the Callback which will be invoked when a `/tile`-request is received.
            The callback argument is a fresh TileFeatureLayer, which the callback must
            fill according to the set TileFeatureLayer's layer info and tile id. If an
            error occurs while filling the tile, the callback can use
            TileFeatureLayer::setError(...) to signal the error downstream.
        )pbdoc")
        .def(
            "go",
            &DataSourceServer::go,
            py::arg("interfaceAddr") = "0.0.0.0",
            py::arg("port") = 0,
            py::arg("waitMs") = 100,
            R"pbdoc(
            Launch the DataSource server in its own thread. Use the stop-function to
            stop the thread. The server will also be stopped automatically, if the
            DataSource object is destroyed. An exception will be thrown if this
            instance is already running, or if the server fails to launch within waitMs.
        )pbdoc")
        .def(
            "is_running",
            &DataSourceServer::isRunning,
            R"pbdoc(
            Returns true if this instance is currently running (go() was called and not stopped).
        )pbdoc")
        .def(
            "stop",
            &DataSourceServer::stop,
            R"pbdoc(
            Stop this instance. Will be a no-op if this instance is not running.
        )pbdoc")
        .def(
            "wait_for_signal",
            &DataSourceServer::waitForSignal,
            py::call_guard<py::gil_scoped_release>(),
            R"pbdoc(
            Blocks until SIGINT or SIGTERM is received, then shuts down the server.
            Note: You can never run this function in parallel for multiple sources
            within the same process!
        )pbdoc")
        .def(
            "port",
            &DataSourceServer::port,
            R"pbdoc(
            Get the port currently used by the instance, or 0 if go() has never been called.
        )pbdoc")
        .def(
            "info",
            &DataSourceServer::info,
            R"pbdoc(
            Get the DataSourceInfo metadata which this instance was constructed with.
        )pbdoc");
}