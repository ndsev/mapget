#pragma once

#include "mapget/model/point.h"
#include "mapget/model/tileid.h"

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <limits>

namespace pybind11::detail
{

template <>
struct type_caster<ndsmath::PackedTileId>
{
public:
    PYBIND11_TYPE_CASTER(ndsmath::PackedTileId, const_name("ndslive.math.PackedTileId"));

    bool load(handle src, bool)
    {
        if (!src || src.is_none())
            return false;

        try {
            auto const obj = pybind11::reinterpret_borrow<pybind11::object>(src);
            auto const packedTileIdClass = pybind11::module_::import("ndslive.math").attr("PackedTileId");
            if (!pybind11::isinstance(obj, packedTileIdClass))
                return false;

            auto const rawValue = obj.attr("value").cast<int64_t>();
            if (rawValue < std::numeric_limits<int32_t>::min() ||
                rawValue > std::numeric_limits<int32_t>::max())
                return false;

            value = ndsmath::PackedTileId::fromValue(static_cast<int32_t>(rawValue));
            return true;
        }
        catch (pybind11::error_already_set const& e) {
            if (e.matches(PyExc_ImportError) || e.matches(PyExc_AttributeError) ||
                e.matches(PyExc_TypeError) || e.matches(PyExc_ValueError)) {
                return false;
            }
            throw;
        }
        catch (std::exception const&) {
            return false;
        }
    }

    static handle cast(ndsmath::PackedTileId const& src, return_value_policy, handle)
    {
        if (!src.isValid())
            return pybind11::none().release();

        auto const packedTileIdClass = pybind11::module_::import("ndslive.math").attr("PackedTileId");
        return packedTileIdClass.attr("from_value")(src.value()).release();
    }
};

}  // namespace pybind11::detail

void bindTileId(py::module_& m)
{
    using namespace mapget;

    py::class_<Point>(m, "Point", R"pbdoc(
            The Point class represents a point in 3D space with x, y, and z components.
        )pbdoc")
        .def(py::init<>(), R"pbdoc(
            Default constructor initializing a Point with x, y, and z set to 0.
            )pbdoc")
        .def(
            py::init<double, double, double>(),
            R"pbdoc(
            Constructor initializing a Point with given x, y, and z.
            )pbdoc",
            py::arg("x") = 0,
            py::arg("y") = 0,
            py::arg("z") = 0)
        .def_readwrite("x", &Point::x, "The x-component of the point.")
        .def_readwrite("y", &Point::y, "The y-component of the point.")
        .def_readwrite("z", &Point::z, "The z-component of the point.")
        .def(
            "__eq__",
            &Point::operator==,
            R"pbdoc(
            Operator overload for equality comparison with another Point.
            )pbdoc",
            py::arg("other"))
        .def("__str__", &Point::toString, R"pbdoc(
            Convert the Point to a string representation.
            )pbdoc");

    auto const packedTileIdClass = py::module_::import("ndslive.math").attr("PackedTileId");
    m.attr("TileId") = packedTileIdClass;
    m.attr("PackedTileId") = packedTileIdClass;
}
