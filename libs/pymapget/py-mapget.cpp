#include <pybind11/pybind11.h>
#include <pybind11/stl_bind.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include "fmt/format.h"

namespace py = pybind11;
using namespace py::literals;
using namespace std::string_literals;

#include "binding/py-tileid.h"
#include "binding/py-model.h"
#include "binding/py-layer.h"
#include "binding/py-datasource.h"
#include "binding/py-service.h"
#include "binding/py-client.h"

#include "mapget/http-service/cli.h"

PYBIND11_MODULE(pymapget, m)
{
    m.doc() = "";

    bindTileId(m);
    bindModel(m);
    bindTileLayer(m);
    bindDataSourceServer(m);
    bindHttpService(m);
    bindHttpClient(m);

    // Note: We only expose the first two parameters of runFromCommandLine.
    // The third parameter (additionalCommandLineSetupFun) is an advanced C++
    // extension point for applications embedding mapget to add custom CLI11
    // commands. It's not exposed to Python as it would require CLI11 Python
    // bindings and has no practical use case for Python API users.
    m.def("run",
        [](const std::vector<std::string>& args, bool requireSubcommand) {
            return mapget::runFromCommandLine(args, requireSubcommand);
        },
        "Run the mapget command-line interface.",
        py::arg("args"),
        py::arg("requireSubcommand") = true);
}
