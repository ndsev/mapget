#pragma once

#include "mapget/model/featurelayer.h"

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "py-model.h"

namespace py = pybind11;
using namespace py::literals;
using namespace mapget;
using namespace simfil;

void bindTileLayer(py::module_& m)
{
    py::class_<TileLayer, std::shared_ptr<TileLayer>>(m, "TileLayer")
        .def(
            "tile_id",
            &TileLayer::tileId,
            R"pbdoc(
            Get the layer's tileId. This controls the rough geographic extent
            of the contained tile data.
        )pbdoc")
        .def(
            "map_id",
            &TileLayer::mapId,
            R"pbdoc(
            Get the identifier of the map which this tile layer belongs to.
        )pbdoc")
        .def(
            "layer_id",
            [](TileLayer& self) { return self.layerInfo()->layerId_; },
            R"pbdoc(
            Get the layer name for this TileLayer.
        )pbdoc")
        .def(
            "error",
            &TileLayer::error,
            R"pbdoc(
            Get the error occurred while the tile was filled.
        )pbdoc")
        .def(
            "set_error",
            &TileLayer::setError,
            py::arg("err"),
            R"pbdoc(
            Set the error occurred while the tile was filled.
        )pbdoc")
        .def(
            "timestamp",
            &TileLayer::timestamp,
            R"pbdoc(
            Get when this layer was created.
        )pbdoc")
        .def(
            "ttl",
            &TileLayer::ttl,
            R"pbdoc(
            Get how long this layer should live.
        )pbdoc")
        .def(
            "set_ttl",
            &TileLayer::setTtl,
            py::arg("time_to_live"),
            R"pbdoc(
            Set how long this layer should live.
        )pbdoc")
        .def(
            "set_info",
            &TileLayer::setInfo,
            py::arg("info"),
            R"pbdoc(
            Set the extra JSON document to store sizes, construction times,
            and other arbitrary meta-information.
        )pbdoc");

    py::class_<TileFeatureLayer, TileLayer, std::shared_ptr<TileFeatureLayer>>(
        m,
        "TileFeatureLayer")
        .def(
            "set_prefix",
            &TileFeatureLayer::setPrefix,
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
            { return BoundObject(self.newArray()); },
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
        )pbdoc");
}
