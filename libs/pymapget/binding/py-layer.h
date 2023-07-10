#pragma once

#include "mapget/model/featurelayer.h"

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "py-model.h"

namespace py = pybind11;
using namespace py::literals;

void bindTileLayer(py::module_& m)
{
    using namespace mapget;
    using namespace simfil;

    py::class_<TileFeatureLayer, TileFeatureLayer::Ptr>(
        m,
        "TileFeatureLayer")
        .def(
            "tile_id",
            [](TileFeatureLayer const& self){return self.tileId();},
            R"pbdoc(
            Get the layer's tileId. This controls the rough geographic extent
            of the contained tile data.
            )pbdoc")
        .def(
            "map_id",
            [](TileFeatureLayer const& self){return self.mapId();},
            R"pbdoc(
            Get the identifier of the map which this tile layer belongs to.
            )pbdoc")
        .def(
            "layer_id",
            [](TileFeatureLayer const& self) { return self.layerInfo()->layerId_; },
            R"pbdoc(
            Get the layer name for this TileLayer.
            )pbdoc")
        .def(
            "error",
            [](TileFeatureLayer const& self) { return self.error(); },
            R"pbdoc(
            Get the error occurred while the tile was filled.
            )pbdoc")
        .def(
            "set_error",
            [](TileFeatureLayer& self, std::string const& e) { self.setError(e); },
            py::arg("err"),
            R"pbdoc(
            Set the error occurred while the tile was filled.
            )pbdoc")
        .def(
            "timestamp",
            [](TileFeatureLayer const& self) {return self.timestamp(); },
            R"pbdoc(
            Get when this layer was created.
            )pbdoc")
        .def(
            "ttl",
            [](TileFeatureLayer const& self) {return self.ttl() ? self.ttl()->count() : -1; },
            R"pbdoc(
            Get how long this layer should live, or -1 if unset.
            )pbdoc")
        .def(
            "set_ttl",
            [](TileFeatureLayer& self, int64_t ms) {
                if (ms >= 0)
                    self.setTtl(std::chrono::milliseconds(ms));
                else
                    self.setTtl(std::nullopt);
            },
            py::arg("time_to_live_in_ms"),
            R"pbdoc(
            Set how long this layer should live, in ms, or -1 for unset.
            )pbdoc")
        .def(
            "set_info",
            [](TileFeatureLayer& self, std::string const& k, simfil::ScalarValueType const& v) {
                std::visit(
                    [&](auto&& vv)
                    {
                        if constexpr (!std::is_same_v<std::decay_t<decltype(vv)>, std::monostate>)
                            self.setInfo(k, vv);
                    },
                    v);
            },
            py::arg("key"),
            py::arg("value"),
            R"pbdoc(
            Set a JSON field to store sizes, construction times,
            and other arbitrary meta-information. The value may be
            bool, int, double or string.
        )pbdoc")
        .def(
            "set_prefix",
            [](TileFeatureLayer& self, KeyValuePairs const& v) {self.setPrefix(v); },
            py::arg("prefix"),
            R"pbdoc(
            Set common id prefix for all features in this layer.
        )pbdoc")
        .def(
            "new_feature",
            [](TileFeatureLayer& self, std::string const& typeId, KeyValuePairs const& idParts)
            { return BoundFeature(self.newFeature(typeId, idParts)); },
            py::arg("type_id"),
            py::arg("feature_id_parts"),
            R"pbdoc(
            Creates a new feature and insert it into this tile layer. The unique identifying
            information, prepended with the featureIdPrefix, must conform to an existing
            UniqueIdComposition for the feature typeId within the associated layer.
        )pbdoc")
        .def(
            "new_feature_id",
            [](TileFeatureLayer& self, std::string const& typeId, KeyValuePairs const& idParts)
            { return BoundFeatureId(self.newFeatureId(typeId, idParts)); },
            py::arg("type_id"),
            py::arg("feature_id_parts"),
            R"pbdoc(
            Create a new feature id. Use this function to create a reference to another
            feature. The created feature id will not use the common feature id prefix
            from this tile feature layer.
        )pbdoc")
        .def(
            "new_attribute",
            [](TileFeatureLayer& self, std::string const& name)
            { return BoundAttribute(self.newAttribute(name)); },
            py::arg("name"),
            R"pbdoc(
            Create a new named attribute, which may be inserted into an attribute layer.
        )pbdoc")
        .def(
            "new_attribute_layer",
            [](TileFeatureLayer& self)
            { return BoundAttributeLayer(self.newAttributeLayer()); },
            R"pbdoc(
            Create a new attribute layer, which may be inserted into a feature.
        )pbdoc")
        .def(
            "new_object",
            [](TileFeatureLayer& self)
            { return BoundObject(self.newObject()); },
            R"pbdoc(
            Adopt members from the given vector and obtain a new object model index which has these members.
        )pbdoc")
        .def(
            "new_array",
            [](TileFeatureLayer& self)
            { return BoundArray(self.newArray()); },
            R"pbdoc(
            Adopt members from the given vector and obtain a new array model index which has these members.
        )pbdoc")
        .def(
            "new_geometry_collection",
            [](TileFeatureLayer& self)
            { return BoundGeometryCollection(self.newGeometryCollection()); },
            R"pbdoc(
            Create a new geometry collection.
        )pbdoc")
        .def(
            "new_geometry",
            [](TileFeatureLayer& self, Geometry::GeomType const& geomType)
            { return BoundGeometry(self.newGeometry(geomType)); },
            py::arg("geom_type"),
            R"pbdoc(
            Create a new geometry of the given type.
        )pbdoc")
        .def(
            "geojson",
            [](TileFeatureLayer& self)
            { return self.toGeoJson().dump(); },
            R"pbdoc(
            Convert this tile to a GeoJSON feature collection.
        )pbdoc");
}
