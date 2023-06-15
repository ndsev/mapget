#pragma once

#include "mapget/model/feature.h"
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

    ScalarValueType value() { return node()->value(); }
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
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    shared_model_ptr<ModelNode> modelNodePtr_;
};

template <typename Function>
void dispatch_py_value(py::object const& py_value, Function&& func)
{
    if (py::isinstance<py::bool_>(py_value)) {
        func(py_value.cast<bool>());
    }
    else if (py::isinstance<py::int_>(py_value)) {
        auto value = py_value.cast<int64_t>();
        if (value >= INT16_MIN && value <= INT16_MAX) {
            func(static_cast<int16_t>(value));
        }
        else {
            func(value);
        }
    }
    else if (py::isinstance<py::float_>(py_value)) {
        func(py_value.cast<double>());
    }
    else if (py::isinstance<py::str>(py_value)) {
        func(py_value.cast<std::string_view>());
    }
    else if (py::isinstance<BoundModelNode>(py_value)) {
        func(py_value.cast<BoundModelNode&>().node());
    }
    else {
        throw std::runtime_error("Unsupported Python type");
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
            [](BoundObject& self, std::string_view const& name, py::object const& py_value)
            {
                dispatch_py_value(
                    py_value,
                    [&self, &name](auto&& value)
                    {
                        if constexpr (std::is_same<std::decay_t<decltype(value)>, bool>::value) {
                            self.modelNodePtr_->addBool(name, value);
                        }
                        else {
                            self.modelNodePtr_->addField(name, value);
                        }
                    });
            },
            py::arg("name"),
            py::arg("value"),
            "Add a field to the object.");
    }

    static void bind(py::module_& m)
    {
        auto boundClass = py::class_<BoundObject<>, BoundModelNode>(m, "Object");
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
                    dispatch_py_value(
                        py_value,
                        [&self](auto&& value) { self.modelNodePtr_->append(value); });
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
        py::enum_<Geometry::GeomType>(m, "GeomType")
            .value("LINE", Geometry::GeomType::Line)
            .value("MESH", Geometry::GeomType::Mesh)
            .value("POINTS", Geometry::GeomType::Points)
            .value("POLYGON", Geometry::GeomType::Polygon)
            .export_values();

        py::class_<BoundGeometry, BoundModelNode>(m, "Geometry")
            .def(
                "append",
                [](shared_model_ptr<Geometry>& node,
                   double const& lon,
                   double const& lat,
                   double const& alt) {
                    return node->append({lon, lat, alt});
                },
                py::arg("lon"),
                py::arg("lat"),
                py::arg("elevation") = .0,
                R"pbdoc(
                Append a point to the geometry.
            )pbdoc");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundGeometry(model_ptr<Geometry> const& ptr) : modelNodePtr_(ptr) {}

    shared_model_ptr<simfil::Geometry> modelNodePtr_;
};

struct BoundGeometryCollection : public BoundModelNode
{
    static void bind(py::module_& m)
    {
        py::class_<BoundGeometryCollection, BoundModelNode>(m, "GeometryCollection");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundGeometryCollection(model_ptr<GeometryCollection> const& ptr) : modelNodePtr_(ptr)
    {
    }

    shared_model_ptr<simfil::GeometryCollection> modelNodePtr_;
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
            .value("NONE", Attribute::Direction::None)
            .export_values();

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
                [](BoundAttributeLayer& self, std::string_view const& name, size_t initialCapacity)
                { return BoundAttribute{self.modelNodePtr_->newAttribute(name, initialCapacity)}; },
                py::arg("name"),
                py::arg("initial_capacity") = 8,
                "Create and insert a new attribute into the layer.")
            .def(
                "add_attribute",
                [](BoundAttributeLayer& self, BoundAttribute const& a)
                { self.modelNodePtr_->addAttribute(a.modelNodePtr_); },
                py::arg("a"),
                "Add an existing attribute to the layer.");

        // Other methods of AttributeLayer class
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
                [](BoundAttributeLayerList& self,
                   std::string_view const& name)
                { return self.modelNodePtr_->newLayer(name); },
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

        // Other methods of AttributeLayerList class
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
                "Convert the Feature to GeoJSON.");

        // TODO: Additional feature field accessors
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

}

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
