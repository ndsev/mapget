#include <pybind11/pybind11.h>
#include <pybind11/stl_bind.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include "fmt/format.h"

namespace py = pybind11;
using namespace py::literals;
using namespace std::string_literals;

// Binding headers share helpers and the PackedTileId caster; preserve dependency order.
// clang-format off
#include "binding/py-tileid.h"
#include "binding/py-model.h"
#include "binding/py-schema.h"
#include "binding/py-layer.h"
#include "binding/py-datasource.h"
#include "binding/py-service.h"
#include "binding/py-client.h"
// clang-format on

#include "mapget/http-service/cli.h"

PYBIND11_MODULE(pymapget, m)
{
    m.doc() = "";

    bindTileId(m);
    bindPartitionId(m);
    bindModel(m);
    mapget::bindSchema(m);
    bindTileLayer(m);
    bindDataSourceServer(m);
    bindHttpService(m);
    bindHttpClient(m);
    // Familiar tile names are aliases, not separate model implementations.
    m.attr("TileLayer") = m.attr("PartitionLayer");
    m.attr("TileFeatureLayer") = m.attr("PartitionFeatureLayer");
    m.attr("TileSubsetLayer") = m.attr("PartitionSubsetLayer");
    m.attr("TileSourceDataLayer") = m.attr("PartitionSourceDataLayer");
    m.attr("MapTileKey") = m.attr("MapPartitionKey");

    // Only the first two CLI parameters are exposed. The C++ command-extension
    // callback depends on CLI11 types that this module does not bind.
    m.def("run",
        [](const std::vector<std::string>& args, bool requireSubcommand) {
            return mapget::runFromCommandLine(args, requireSubcommand);
        },
        "Run the mapget command-line interface.",
        py::arg("args"),
        py::arg("requireSubcommand") = true);
}
