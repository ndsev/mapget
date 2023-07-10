#pragma once

#include "mapget/model/tileid.h"

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

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

    py::class_<TileId>(m, "TileId", R"pbdoc(
            The TileId struct represents a tile identifier for a specific map tile.
            It includes an x (column), y (row), and z (zoom level) components.
            )pbdoc")
        .def(
            py::init<uint16_t, uint16_t, uint16_t>(),
            R"pbdoc(
            Constructor to initialize TileId with x, y, and z values.
            )pbdoc",
            py::arg("x"),
            py::arg("y"),
            py::arg("z"))
        .def(
            py::init<uint64_t>(),
            R"pbdoc(
            Constructor to initialize TileId with a value in 0x00xxyyzz format.
            )pbdoc",
            py::arg("value"))
        .def_static(
            "from_wgs84",
            &TileId::fromWgs84,
            R"pbdoc(
            Create a TileId from WGS84 longitude, latitude, and zoom level.
            )pbdoc",
            py::arg("longitude"),
            py::arg("latitude"),
            py::arg("zoom_level"))
        .def("center", &TileId::center, R"pbdoc(
            Get the center of the tile in WGS84 coordinates.
            )pbdoc")
        .def("sw", &TileId::sw, R"pbdoc(
            Get the south-west (minimum) corner of the tile in WGS84 coordinates.
            )pbdoc")
        .def("ne", &TileId::ne, R"pbdoc(
            Get the north-east (maximum) corner of the tile in WGS84 coordinates.
            )pbdoc")
        .def("size", &TileId::size, R"pbdoc(
            Get the size of the tile in WGS84 coordinates.
            )pbdoc")
        .def("x", &TileId::x, R"pbdoc(
            Get the x (column) component of the TileId.
            )pbdoc")
        .def("y", &TileId::y, R"pbdoc(
            Get the y (row) component of the TileId.
            )pbdoc")
        .def("z", &TileId::z, R"pbdoc(
            Get the z (zoom level) component of the TileId.
            )pbdoc")
        .def(
            "__eq__",
            &TileId::operator==,
            R"pbdoc(
            Operator overload for equality comparison with another TileId.
            )pbdoc",
            py::arg("other"))
        .def(
            "__lt__",
            &TileId::operator<,
            R"pbdoc(
            Operator overload for less than comparison with another TileId.
            )pbdoc",
            py::arg("other"))
        .def_readwrite("value", &TileId::value_, R"pbdoc(
            The value representing the TileId, in 0x00xxyyzz format.
            )pbdoc");
}
