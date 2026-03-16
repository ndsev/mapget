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

py::object nodeToPython(model_ptr<ModelNode> const& n, TileFeatureLayer& fl, bool checkMultimap = false);

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
                [](BoundModelNode& self) {
                    if (auto n = self.node())
                        return n->value();
                    return ScalarValueType{};
                },
                R"pbdoc(
            Get the node's scalar value if it has one.
        )pbdoc")
            .def(
                "to_json",
                [](BoundModelNode& self) {
                    if (auto n = self.node())
                        return n->toJson().dump();
                    return std::string("null");
                },
                "Convert this node to a JSON string.")
            .def(
                "to_dict",
                [](BoundModelNode& self) -> py::object {
                    if (auto n = self.node()) {
                        auto& fl = self.featureLayer();
                        return nodeToPython(n, fl);
                    }
                    return py::none();
                },
                "Convert this node to a Python dict/list/scalar.");
        py::enum_<ValueType>(m, "ValueType")
            .value("UNDEF", ValueType::Undef)
            .value("NULL_", ValueType::Null)
            .value("BOOL", ValueType::Bool)
            .value("INT", ValueType::Int)
            .value("FLOAT", ValueType::Float)
            .value("STRING", ValueType::String)
            .value("BYTES", ValueType::Bytes)
            .value("OBJECT", ValueType::Object)
            .value("ARRAY", ValueType::Array);

        py::class_<BoundModelNodeBase, BoundModelNode>(m, "ModelNodeBase")
            .def("__len__", [](BoundModelNodeBase& self) {
                return self.modelNodePtr_->size();
            })
            .def("__getitem__", [](BoundModelNodeBase& self, std::string_view const& key) {
                auto& fl = self.featureLayer();
                for (auto const& [fieldId, child] : self.modelNodePtr_->fields()) {
                    if (auto resolved = fl.lookupStringId(fieldId)) {
                        if (*resolved == key) {
                            BoundModelNodeBase node;
                            node.modelNodePtr_ = child;
                            return node;
                        }
                    }
                }
                throw py::key_error(std::string(key));
            }, py::arg("key"))
            .def("__getitem__", [](BoundModelNodeBase& self, int64_t i) {
                auto sz = (int64_t)self.modelNodePtr_->size();
                if (i < 0) i += sz;
                if (i < 0 || i >= sz) throw py::index_error();
                BoundModelNodeBase node;
                node.modelNodePtr_ = self.modelNodePtr_->at(i);
                return node;
            }, py::arg("index"))
            .def("__iter__", [](BoundModelNodeBase& self) {
                auto n = self.modelNodePtr_;
                auto type = n->type();
                if (type == ValueType::Object) {
                    py::list result;
                    auto& fl = self.featureLayer();
                    for (auto const& [fieldId, child] : n->fields()) {
                        if (auto key = fl.lookupStringId(fieldId)) {
                            BoundModelNodeBase node;
                            node.modelNodePtr_ = child;
                            result.append(py::make_tuple(std::string(*key), node));
                        }
                    }
                    return py::iter(result);
                }
                else if (type == ValueType::Array) {
                    py::list result;
                    for (uint32_t i = 0; i < n->size(); ++i) {
                        BoundModelNodeBase node;
                        node.modelNodePtr_ = n->at(i);
                        result.append(node);
                    }
                    return py::iter(result);
                }
                py::list empty;
                return py::iter(empty);
            })
            .def("type", [](BoundModelNodeBase& self) {
                return self.modelNodePtr_->type();
            });
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    model_ptr<ModelNode> modelNodePtr_;
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
        auto arr = model.newArray(list.size(), true);

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
        auto obj = model.newObject(dict.size(), true);

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
        boundClass
            .def("__len__", [](BoundObject& self) { return self.modelNodePtr_->size(); })
            .def(
                "__getitem__",
                [](BoundObject& self, std::string_view const& key) {
                    auto result = self.modelNodePtr_->get(key);
                    if (!result) throw py::key_error(std::string(key));
                    BoundModelNodeBase node;
                    node.modelNodePtr_ = *result;
                    return node;
                },
                py::arg("key"),
                "Get a field by name.")
            .def("__iter__", [](BoundObject& self) {
                py::list result;
                auto& fl = self.featureLayer();
                for (auto const& [fieldId, childNode] : self.modelNodePtr_->fields()) {
                    if (auto resolved = fl.lookupStringId(fieldId)) {
                        BoundModelNodeBase node;
                        node.modelNodePtr_ = childNode;
                        result.append(py::make_tuple(std::string(*resolved), node));
                    }
                }
                return py::iter(result);
            });
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundObject(model_ptr<NodeType> const& ptr) : modelNodePtr_(ptr) {}

    model_ptr<NodeType> modelNodePtr_;
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
                "Append a value to the array.")
            .def("__len__", [](BoundArray& self) { return self.modelNodePtr_->size(); })
            .def(
                "__getitem__",
                [](BoundArray& self, int64_t i) {
                    auto sz = (int64_t)self.modelNodePtr_->size();
                    if (i < 0) i += sz;
                    if (i < 0 || i >= sz) throw py::index_error();
                    BoundModelNodeBase node;
                    node.modelNodePtr_ = self.modelNodePtr_->at(i);
                    return node;
                },
                py::arg("index"),
                "Get an element by index.")
            .def("__iter__", [](BoundArray& self) {
                py::list result;
                auto sz = self.modelNodePtr_->size();
                for (uint32_t i = 0; i < sz; ++i) {
                    BoundModelNodeBase node;
                    node.modelNodePtr_ = self.modelNodePtr_->at(i);
                    result.append(node);
                }
                return py::iter(result);
            });
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundArray(model_ptr<Array> const& ptr) : modelNodePtr_(ptr) {}

    model_ptr<Array> modelNodePtr_;
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
            )pbdoc")
            .def("geom_type", [](BoundGeometry& self) { return self.modelNodePtr_->geomType(); },
                "Get the type of the geometry.")
            .def("num_points", [](BoundGeometry& self) { return self.modelNodePtr_->numPoints(); },
                "Get the number of points.")
            .def("point_at", [](BoundGeometry& self, size_t i) { return self.modelNodePtr_->pointAt(i); },
                py::arg("index"), "Get a point at an index.")
            .def("__len__", [](BoundGeometry& self) { return self.modelNodePtr_->numPoints(); })
            .def("__getitem__", [](BoundGeometry& self, int64_t i) {
                auto n = (int64_t)self.modelNodePtr_->numPoints();
                if (i < 0) i += n;
                if (i < 0 || i >= n) throw py::index_error();
                return self.modelNodePtr_->pointAt(i);
            })
            .def("length", [](BoundGeometry& self) { return self.modelNodePtr_->length(); },
                "Get total length in metres (for polylines).");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundGeometry(model_ptr<Geometry> const& ptr) : modelNodePtr_(ptr) {}

    model_ptr<Geometry> modelNodePtr_;
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
                "Create and insert a new geometry into the collection.")
            .def("__len__", [](BoundGeometryCollection& self) {
                return self.modelNodePtr_->numGeometries();
            })
            .def("__iter__", [](BoundGeometryCollection& self) {
                py::list result;
                self.modelNodePtr_->forEachGeometry(
                    [&result](model_ptr<Geometry> const& geom) {
                        result.append(BoundGeometry(geom));
                        return true;
                    });
                return py::iter(result);
            });
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundGeometryCollection(model_ptr<GeometryCollection> const& ptr) : modelNodePtr_(ptr)
    {
    }

    model_ptr<GeometryCollection> modelNodePtr_;
};

struct BoundAttribute : public BoundObject<Attribute>
{
    static void bind(py::module_& m)
    {
        py::enum_<Validity::Direction>(m, "Direction")
            .value("EMPTY", Validity::Direction::Empty)
            .value("POSITIVE", Validity::Direction::Positive)
            .value("NEGATIVE", Validity::Direction::Negative)
            .value("BOTH", Validity::Direction::Both)
            .value("NONE", Validity::Direction::None);

        auto boundClass =
            py::class_<BoundAttribute, BoundModelNode>(m, "Attribute")
                .def(
                    "validity",
                    [](BoundAttribute& self) { return self.modelNodePtr_->validityOrNull(); },
                    "Get the validity of the attribute.")
                .def(
                    "name",
                    [](BoundAttribute& self) { return self.modelNodePtr_->name(); },
                    "Get the name of the attribute.");

        bindObjectMethods(boundClass);
        boundClass.def("__iter__", [](BoundAttribute& self) {
            py::list result;
            auto& fl = self.featureLayer();
            for (auto const& [fieldId, childNode] : self.modelNodePtr_->fields()) {
                if (auto resolved = fl.lookupStringId(fieldId)) {
                    BoundModelNodeBase node;
                    node.modelNodePtr_ = childNode;
                    result.append(py::make_tuple(std::string(*resolved), node));
                }
            }
            return py::iter(result);
        });
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
                "Add an existing attribute to the layer.")
            .def("__iter__", [](BoundAttributeLayer& self) {
                py::list result;
                self.modelNodePtr_->forEachAttribute(
                    [&result](model_ptr<Attribute> const& attr) {
                        result.append(BoundAttribute(attr));
                        return true;
                    });
                return py::iter(result);
            })
            .def("to_dict", [](BoundAttributeLayer& self) -> py::object {
                if (auto n = self.node()) {
                    auto& fl = self.featureLayer();
                    return nodeToPython(n, fl, true);
                }
                return py::none();
            }, "Convert this layer to a Python dict (handles duplicate attribute names).");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundAttributeLayer(model_ptr<AttributeLayer> const& ptr) : modelNodePtr_(ptr) {}

    model_ptr<AttributeLayer> modelNodePtr_;
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
                "Add an existing layer to the collection.")
            .def("__iter__", [](BoundAttributeLayerList& self) {
                py::list result;
                self.modelNodePtr_->forEachLayer(
                    [&result](std::string_view name, model_ptr<AttributeLayer> const& layer) {
                        result.append(py::make_tuple(std::string(name), BoundAttributeLayer(layer)));
                        return true;
                    });
                return py::iter(result);
            })
            .def("to_dict", [](BoundAttributeLayerList& self) -> py::object {
                py::dict d;
                auto& fl = self.featureLayer();
                self.modelNodePtr_->forEachLayer(
                    [&](std::string_view name, model_ptr<AttributeLayer> const& layer) {
                        d[py::str(std::string(name))] = nodeToPython(layer, fl, true);
                        return true;
                    });
                return d;
            }, "Convert all layers to a Python dict (handles duplicate attribute names).");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundAttributeLayerList(model_ptr<AttributeLayerList> const& ptr) : modelNodePtr_(ptr)
    {
    }

    model_ptr<AttributeLayerList> modelNodePtr_;
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

    model_ptr<FeatureId> modelNodePtr_;
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
                "id",
                [](BoundFeature& self) { return BoundFeatureId(self.modelNodePtr_->id()); },
                "Get the feature's unique ID.")
            .def(
                "to_json",
                [](BoundFeature& self) { return self.modelNodePtr_->toJson().dump(); },
                "Convert the Feature to a JSON string.")
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

    model_ptr<Feature> modelNodePtr_;
};

/// Recursively convert a ModelNode tree to native Python objects.
/// Mirrors simfil's ModelNode::toJson() (nodes.cpp) but builds
/// py::dict/py::list/scalars directly, avoiding JSON serialization.
///
/// Handles two special cases that toJson() also handles:
///  - Multimap objects: when checkMultimap is true and an Object has
///    duplicate keys, values are grouped into lists with a "_multimap"
///    marker. Only AttributeLayer-level objects can have duplicates,
///    so callers should only pass true at that level to avoid overhead.
///  - ByteArray scalars: converted to {"_bytes": True, "hex": ..., "number": ...}.
py::object nodeToPython(model_ptr<ModelNode> const& n, TileFeatureLayer& fl, bool checkMultimap)
{
    auto type = n->type();
    if (type == ValueType::Object) {
        py::dict d;
        if (checkMultimap) {
            // Pre-scan for duplicate field IDs using stack-allocated
            // array with cheap uint16_t comparison. Only called for
            // AttributeLayer-level objects, not recursively.
            bool isMultiMap = false;
            {
                uint16_t seen[32];
                size_t count = 0;
                for (auto const& [fieldId, _] : n->fields()) {
                    for (size_t j = 0; j < count; ++j) {
                        if (seen[j] == fieldId) {
                            isMultiMap = true;
                            break;
                        }
                    }
                    if (isMultiMap) break;
                    if (count < 32)
                        seen[count++] = fieldId;
                }
            }
            if (isMultiMap) {
                for (auto const& [fieldId, child] : n->fields()) {
                    if (auto key = fl.lookupStringId(fieldId)) {
                        auto pyKey = py::str(std::string(*key));
                        if (d.contains(pyKey))
                            d[pyKey].cast<py::list>().append(nodeToPython(child, fl));
                        else
                            d[pyKey] = py::list(py::make_tuple(nodeToPython(child, fl)));
                    }
                }
                d[py::str("_multimap")] = py::bool_(true);
                return d;
            }
        }
        for (auto const& [fieldId, child] : n->fields()) {
            if (auto key = fl.lookupStringId(fieldId))
                d[py::str(std::string(*key))] = nodeToPython(child, fl);
        }
        return d;
    }
    if (type == ValueType::Array) {
        py::list l;
        for (uint32_t i = 0; i < n->size(); ++i)
            l.append(nodeToPython(n->at(i), fl));
        return l;
    }
    auto v = n->value();
    return std::visit([](auto&& val) -> py::object {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, bool>)
            return py::bool_(val);
        else if constexpr (std::is_same_v<T, int64_t>)
            return py::int_(val);
        else if constexpr (std::is_same_v<T, double>)
            return py::float_(val);
        else if constexpr (std::is_same_v<T, std::string>)
            return py::str(val);
        else if constexpr (std::is_same_v<T, std::string_view>)
            return py::str(std::string(val));
        else if constexpr (std::is_same_v<T, ByteArray>) {
            py::dict d;
            d["_bytes"] = py::bool_(true);
            d["hex"] = py::str(val.toHex(false));
            if (auto decoded = val.decodeBigEndianI64())
                d["number"] = py::int_(*decoded);
            else
                d["number"] = py::none();
            return d;
        }
        else
            return py::none();
    }, v);
}

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
    mapget::BoundFeatureId::bind(m);
    mapget::BoundFeature::bind(m);
}
