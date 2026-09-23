#pragma once

#include "mapget/model/partitionid.h"
#include "mapget/model/point.h"

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

/** Convert the tile-compatible Python input without silently accepting untagged integers. */
inline mapget::PartitionId partitionIdFromPython(py::handle value)
{
    if (py::isinstance<mapget::PartitionId>(value))
        return value.cast<mapget::PartitionId>();
    return mapget::PartitionId::tile(value.cast<mapget::TileId>());
}

/** Keep Python tile requests convenient while accepting explicit object partitions. */
inline std::vector<mapget::PartitionId> partitionIdsFromPython(py::iterable const& values)
{
    std::vector<mapget::PartitionId> result;
    for (auto value : values)
        result.push_back(partitionIdFromPython(value));
    return result;
}

/** Bind tagged identities separately from ndslive.math's spatial tile class. */
inline void bindPartitionId(py::module_& m)
{
    using namespace mapget;
    py::enum_<PartitionKind>(
        m,
        "PartitionKind",
        "Addressing scheme independent of the layer payload type.")
        .value("TILE", PartitionKind::Tile)
        .value("OBJECT", PartitionKind::Object);
    py::class_<PartitionId>(
        m,
        "PartitionId",
        "Tagged tile or unsigned 64-bit object identity. Objects have no spatial tile operations.")
        .def(py::init<TileId>(), py::arg("tile_id"), "Wrap a PackedTileId as a tile partition.")
        .def_static("tile", &PartitionId::tile, py::arg("tile_id"), "Wrap a spatial tile ID.")
        .def_static(
            "object",
            &PartitionId::object,
            py::arg("object_id"),
            "Create an object identity from an integer in 0..2**64-1.")
        .def_property_readonly(
            "kind",
            &PartitionId::kind,
            "Identity tag; never inferred from the numeric value.")
        .def_property_readonly(
            "tile_id",
            &PartitionId::tileId,
            "PackedTileId; raises for object identities.")
        .def_property_readonly(
            "object_id",
            &PartitionId::objectId,
            "Unsigned object ID; raises for tile identities.")
        .def(
            "to_json",
            [](PartitionId const& id) { return id.toJson().dump(); },
            "Serialize with decimal strings for lossless object IDs.")
        .def_static(
            "from_json",
            [](std::string const& json)
            { return PartitionId::fromJson(nlohmann::json::parse(json)); },
            py::arg("json"),
            "Parse tagged JSON, validating kind and range.")
        .def(
            "__str__",
            &PartitionId::toString,
            "Decimal identifier; use kind to distinguish tile and object.")
        .def(
            "__eq__",
            [](PartitionId const& a, PartitionId const& b) { return a == b; },
            py::is_operator(),
            "Compare tag and value.")
        .def(
            "__hash__",
            [](PartitionId const& id) { return std::hash<PartitionId>{}(id); },
            "Hash tag and value.");
}
