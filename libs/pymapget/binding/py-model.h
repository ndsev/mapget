#pragma once

#include "mapget/log.h"
#include "mapget/model/feature.h"
#include "mapget/model/featurelayer.h"
#include "simfil/value.h"

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace py::literals;
using namespace simfil;

namespace mapget
{

struct BoundModelNode
{
    virtual ~BoundModelNode() = default;

    virtual ModelNode::Ptr node() = 0;

    TileFeatureLayer& featureLayer()
    {
        struct GetTileFeatureLayer : public simfil::ModelNode {
            explicit GetTileFeatureLayer(simfil::ModelNode const& n) : simfil::ModelNode(n) {}
            auto operator()() {
                return std::dynamic_pointer_cast<TileFeatureLayer>(
                    std::const_pointer_cast<simfil::Model>(model_));
            }
        };
        if (auto n = node()) {
            if (auto ptr = GetTileFeatureLayer(*n)())
                return *ptr;
            else
                throw pybind11::type_error("Unexpected model type");
        }
        throw pybind11::value_error("Node is NULL");
    }
};

struct BoundModelNodeBase : public BoundModelNode
{
    static void bind(py::module_& m)
    {
        py::class_<BoundModelNode>(m, "ModelNode")
            .def(
                "value",
                [](const shared_model_ptr<ModelNode>& node) { return node->value(); },
                R"pbdoc(
            Get the node's scalar value if it has one.
        )pbdoc");
        py::class_<BoundModelNodeBase, BoundModelNode>(m, "ModelNodeBase");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    shared_model_ptr<ModelNode> modelNodePtr_;
};

using ModelVariant =
    std::variant<bool, int16_t, int64_t, double, std::string_view, ModelNode::Ptr>;

ModelVariant pyValueToModel(py::object const& pyValue, TileFeatureLayer& model)
{
    if (py::isinstance<py::bool_>(pyValue)) {
        return pyValue.cast<bool>();
    }
    else if (py::isinstance<py::int_>(pyValue)) {
        auto value = pyValue.cast<int64_t>();
        if (value >= INT16_MIN && value <= INT16_MAX) {
            return static_cast<int16_t>(value);
        }
        else {
            return value;
        }
    }
    else if (py::isinstance<py::float_>(pyValue)) {
        return pyValue.cast<double>();
    }
    else if (py::isinstance<py::str>(pyValue)) {
        return pyValue.cast<std::string_view>();
    }
    else if (py::isinstance<BoundModelNode>(pyValue)) {
        return pyValue.cast<BoundModelNode&>().node();
    }
    else if (py::isinstance<py::list>(pyValue)) {
        // Recursively convert Python list to array.
        auto list = pyValue.cast<py::list>();
        auto arr = model.newArray(list.size());

        for (auto const& item : list) {
            auto value = pyValueToModel(py::reinterpret_borrow<py::object>(item), model);
            std::visit([&arr](auto&& vv){
                arr->append(vv);
            }, value);
        }

        return ModelNode::Ptr(arr);
    }
    else if (py::isinstance<py::dict>(pyValue)) {
        // Recursively convert Python dict to object.
        auto dict = pyValue.cast<py::dict>();
        auto obj = model.newObject(dict.size());

        for (auto const& [anyKey, anyValue] : dict) {
            std::string key = py::str(anyKey);
            auto vv = pyValueToModel(py::reinterpret_borrow<py::object>(anyValue), model);
            std::visit(
                [&obj, &key](auto&& value)
                {
                    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, bool>)
                        obj->addBool(key, value);
                    else
                        obj->addField(key, value);
                },
                vv);
        }

        return ModelNode::Ptr(obj);
    }
    else {
        mapget::raise("Unsupported Python type");
    }
}

template <typename NodeType = Object>
struct BoundObject : public BoundModelNode
{
    template <class ObjClass, class ParentClass>
    static void bindObjectMethods(py::class_<ObjClass, ParentClass>& c)
    {
        c.def(
            "add_field",
            [](ObjClass& self, std::string_view const& name, py::object const& py_value)
            {
                auto vv = pyValueToModel(py_value, self.featureLayer());
                std::visit(
                    [&self, &name](auto&& value)
                    {
                        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, bool>) {
                            self.modelNodePtr_->addBool(name, value);
                        }
                        else {
                            self.modelNodePtr_->addField(name, value);
                        }
                    },
                    vv);
            },
            py::arg("name"),
            py::arg("value"),
            "Add a field to the object.");
        c.def(
            "extend",
            [](ObjClass& self, BoundObject<> const& py_value)
            { self.modelNodePtr_->extend(py_value.modelNodePtr_); },
            py::arg("other_object"),
            "Add all fields from `other_object` to this object.");
    }

    static void bind(py::module_& m)
    {
        auto boundClass = py::class_<BoundObject, BoundModelNode>(m, "Object");
        bindObjectMethods(boundClass);
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundObject(model_ptr<NodeType> const& ptr) : modelNodePtr_(ptr) {}

    shared_model_ptr<NodeType> modelNodePtr_;
};

struct BoundArray : public BoundModelNode
{
    static void bind(py::module_& m)
    {
        py::class_<BoundArray, BoundModelNode>(m, "Array")
            .def(
                "append",
                [](BoundArray& self, py::object const& py_value) {
                    auto vv = pyValueToModel(py_value, self.featureLayer());
                    std::visit([&self](auto&& value) { self.modelNodePtr_->append(value); }, vv);
                },
                py::arg("value"),
                "Append a value to the array.");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundArray(model_ptr<Array> const& ptr) : modelNodePtr_(ptr) {}

    shared_model_ptr<Array> modelNodePtr_;
};

struct BoundGeometry : public BoundModelNode
{
    static void bind(py::module_& m)
    {
        py::enum_<GeomType>(m, "GeomType")
            .value("LINE", GeomType::Line)
            .value("MESH", GeomType::Mesh)
            .value("POINTS", GeomType::Points)
            .value("POLYGON", GeomType::Polygon);

        py::class_<BoundGeometry, BoundModelNode>(m, "Geometry")
            .def(
                "append",
                [](BoundGeometry& node, double const& lon, double const& lat, double const& alt) {
                    node.modelNodePtr_->append({lon, lat, alt});
                },
                py::arg("lon"),
                py::arg("lat"),
                py::arg("elevation") = .0,
                R"pbdoc(
                Append a point to the geometry.
            )pbdoc")
            .def(
                "append",
                [](BoundGeometry& node, Point const& p) {
                    node.modelNodePtr_->append(p);
                },
                py::arg("point"),
                R"pbdoc(
                Append a point to the geometry.
            )pbdoc");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundGeometry(model_ptr<Geometry> const& ptr) : modelNodePtr_(ptr) {}

    shared_model_ptr<Geometry> modelNodePtr_;
};

struct BoundGeometryCollection : public BoundModelNode
{
    static void bind(py::module_& m)
    {
        py::class_<BoundGeometryCollection, BoundModelNode>(m, "GeometryCollection")
            .def(
                "new_geometry",
                [](BoundGeometryCollection& self, GeomType const& geomType)
                { return BoundGeometry(self.modelNodePtr_->newGeometry(geomType)); },
                py::arg("geom_type"),
                "Create and insert a new geometry into the collection.");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundGeometryCollection(model_ptr<GeometryCollection> const& ptr) : modelNodePtr_(ptr)
    {
    }

    shared_model_ptr<GeometryCollection> modelNodePtr_;
};

struct BoundAttribute : public BoundObject<Attribute>
{
    static void bind(py::module_& m)
    {
        py::enum_<Attribute::Direction>(m, "Direction")
            .value("EMPTY", Attribute::Direction::Empty)
            .value("POSITIVE", Attribute::Direction::Positive)
            .value("NEGATIVE", Attribute::Direction::Negative)
            .value("BOTH", Attribute::Direction::Both)
            .value("NONE", Attribute::Direction::None);

        auto boundClass =
            py::class_<BoundAttribute, BoundModelNode>(m, "Attribute")
                .def(
                    "direction",
                    [](BoundAttribute& self) { return self.modelNodePtr_->direction(); },
                    "Get the direction of the attribute.")
                .def(
                    "set_direction",
                    [](BoundAttribute& self, Attribute::Direction const& v)
                    { self.modelNodePtr_->setDirection(v); },
                    py::arg("v"),
                    "Set the direction of the attribute.")
                .def(
                    "validity",
                    [](BoundAttribute& self) { return self.modelNodePtr_->validity(); },
                    "Get the validity of the attribute.")
                .def(
                    "name",
                    [](BoundAttribute& self) { return self.modelNodePtr_->name(); },
                    "Get the name of the attribute.");

        bindObjectMethods(boundClass);
    }

    explicit BoundAttribute(model_ptr<Attribute> const& ptr) : BoundObject<Attribute>(ptr) {}
};

struct BoundAttributeLayer : public BoundModelNode
{
    static void bind(py::module_& m)
    {
        py::class_<BoundAttributeLayer, BoundModelNode>(m, "AttributeLayer")
            .def(
                "new_attribute",
                [](BoundAttributeLayer& self, std::string_view const& name)
                { return BoundAttribute{self.modelNodePtr_->newAttribute(name)}; },
                py::arg("name"),
                "Create and insert a new attribute into the layer.")
            .def(
                "add_attribute",
                [](BoundAttributeLayer& self, BoundAttribute const& a)
                { self.modelNodePtr_->addAttribute(a.modelNodePtr_); },
                py::arg("a"),
                "Add an existing attribute to the layer.");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundAttributeLayer(model_ptr<AttributeLayer> const& ptr) : modelNodePtr_(ptr) {}

    shared_model_ptr<AttributeLayer> modelNodePtr_;
};

struct BoundAttributeLayerList : public BoundModelNode
{
    static void bind(py::module_& m)
    {
        py::class_<BoundAttributeLayerList, BoundModelNode>(m, "AttributeLayerList")
            .def(
                "new_layer",
                [](BoundAttributeLayerList& self, std::string_view const& name)
                { return BoundAttributeLayer(self.modelNodePtr_->newLayer(name)); },
                py::arg("name"),
                "Create and insert a new layer into the collection.")
            .def(
                "add_layer",
                [](BoundAttributeLayerList& self,
                   std::string_view const& name,
                   BoundAttributeLayer const& l)
                { self.modelNodePtr_->addLayer(name, l.modelNodePtr_); },
                py::arg("name"),
                py::arg("layer"),
                "Add an existing layer to the collection.");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundAttributeLayerList(model_ptr<AttributeLayerList> const& ptr) : modelNodePtr_(ptr)
    {
    }

    shared_model_ptr<AttributeLayerList> modelNodePtr_;
};

struct BoundFeature : public BoundModelNode
{
    static void bind(py::module_& m)
    {
        py::class_<BoundFeature, BoundModelNode>(m, "Feature")
            .def(
                "type_id",
                [](BoundFeature& self) { return self.modelNodePtr_->typeId(); },
                "Get the type ID of the feature.")
            .def(
                "evaluate",
                [](BoundFeature& self, std::string_view const& expression)
                { return self.modelNodePtr_->evaluate(expression).getScalar(); },
                py::arg("expression"),
                "Evaluate a filter expression on this feature.")
            .def(
                "to_geo_json",
                [](BoundFeature& self) { return self.modelNodePtr_->toGeoJson(); },
                "Convert the Feature to GeoJSON.")
            .def(
                "geom",
                [](BoundFeature& self)
                { return BoundGeometryCollection(self.modelNodePtr_->geom()); },
                "Access this feature's geometry collection.")
            .def(
                "attributes",
                [](BoundFeature& self) { return BoundObject(self.modelNodePtr_->attributes()); },
                "Access this feature's arbitrary attributes.")
            .def(
                "attribute_layers",
                [](BoundFeature& self)
                { return BoundAttributeLayerList(self.modelNodePtr_->attributeLayers()); },
                "Access this feature's attribute layer collection.")
            .def(
                "relations",
                [](BoundFeature& self) { return BoundArray(self.modelNodePtr_->relations()); },
                "Access this feature's relation list.")
            .def(
                "add_point",
                [](BoundFeature& self, Point const& p) {
                    self.modelNodePtr_->addPoint(p);
                },
                py::arg("p"),
                "Add a point to the feature.")
            .def(
                "add_points",
                [](BoundFeature& self, std::vector<Point> const& points) {
                    self.modelNodePtr_->addPoints(points);
                },
                py::arg("points"),
                "Add multiple points to the feature.")
            .def(
                "add_line",
                [](BoundFeature& self, std::vector<Point> const& points) {
                    self.modelNodePtr_->addLine(points);
                },
                py::arg("points"),
                "Add a line to the feature.")
            .def(
                "add_mesh",
                [](BoundFeature& self, std::vector<Point> const& points) {
                    self.modelNodePtr_->addMesh(points);
                },
                py::arg("points"),
                "Add a mesh to the feature, len(points) must be multiple of three.")
            .def(
                "add_poly",
                [](BoundFeature& self, std::vector<Point> const& points) {
                    self.modelNodePtr_->addPoly(points);
                },
                py::arg("points"),
                "Add a polygon to the feature.");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundFeature(model_ptr<Feature> const& ptr) : modelNodePtr_(ptr) {}

    shared_model_ptr<Feature> modelNodePtr_;
};

struct BoundFeatureId : public BoundModelNode
{
    static void bind(py::module_& m)
    {
        py::class_<BoundFeatureId, BoundModelNode>(m, "FeatureId")
            .def(
                "to_string",
                [](BoundFeatureId& self) { return self.modelNodePtr_->toString(); },
                "Convert the FeatureId to a string.")
            .def(
                "type_id",
                [](BoundFeatureId& self) { return self.modelNodePtr_->typeId(); },
                "Get the feature ID's type ID.");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundFeatureId(model_ptr<FeatureId> const& ptr) : modelNodePtr_(ptr) {}

    shared_model_ptr<FeatureId> modelNodePtr_;
};

}  // namespace mapget

void bindModel(py::module& m)
{
    mapget::BoundModelNodeBase::bind(m);
    mapget::BoundObject<>::bind(m);
    mapget::BoundArray::bind(m);
    mapget::BoundGeometry::bind(m);
    mapget::BoundGeometryCollection::bind(m);
    mapget::BoundAttribute::bind(m);
    mapget::BoundAttributeLayer::bind(m);
    mapget::BoundAttributeLayerList::bind(m);
    mapget::BoundFeature::bind(m);
    mapget::BoundFeatureId::bind(m);
}
