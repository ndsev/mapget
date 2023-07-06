#include <pybind11/pybind11.h>
#include <pybind11/stl_bind.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include "stx/format.h"

namespace py = pybind11;
using namespace py::literals;
using namespace std::string_literals;

#include "binding/py-tileid.h"
#include "binding/py-model.h"
#include "binding/py-layer.h"
#include "binding/py-datasource.h"
#include "binding/py-service.h"

PYBIND11_MODULE(mapget, m)
{
    m.doc() = "";

    bindTileId(m);
    bindModel(m);
    bindTileLayer(m);
    bindDataSourceServer(m);
    bindHttpService(m);
    bindHttpClient(m);
}
