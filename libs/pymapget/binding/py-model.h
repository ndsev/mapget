#pragma once

#include "mapget/log.h"
#include "mapget/model/feature.h"
#include "mapget/model/featurelayer.h"
#include "simfil/value.h"
#include "mapget/model/sourcedatareference.h"

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace py::literals;
using namespace simfil;

namespace mapget
{

py::object nodeToPython(model_ptr<ModelNode> const& n, simfil::ModelPool& model, bool checkMultimap = false);

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

ModelVariant pyValueToModel(py::object const& pyValue, simfil::ModelPool& model)
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
                "Get total length in metres (for polylines).")
            .def("stage", [](BoundGeometry& self) {
                    return self.modelNodePtr_->stage();
                },
                "Get the geometry stage, or None if no stage is set.")
            .def("set_stage", [](BoundGeometry& self, std::optional<uint32_t> stage) {
                    self.modelNodePtr_->setStage(stage);
                },
                py::arg("stage") = std::nullopt,
                "Set or clear the geometry stage.");
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

struct BoundValidity : public BoundModelNode
{
    static void bind(py::module_& m);
    ModelNode::Ptr node() override;
    explicit BoundValidity(model_ptr<Validity> const& ptr);
    model_ptr<Validity> modelNodePtr_;
};

struct BoundMultiValidity : public BoundModelNode
{
    static void bind(py::module_& m);
    ModelNode::Ptr node() override;
    explicit BoundMultiValidity(model_ptr<MultiValidity> const& ptr);
    model_ptr<MultiValidity> modelNodePtr_;
};

struct BoundSourceDataReferenceCollection : public BoundModelNode
{
    static void bind(py::module_& m);
    ModelNode::Ptr node() override;
    explicit BoundSourceDataReferenceCollection(model_ptr<SourceDataReferenceCollection> const& ptr);
    model_ptr<SourceDataReferenceCollection> modelNodePtr_;
};

struct BoundRelation : public BoundModelNode
{
    static void bind(py::module_& m);
    ModelNode::Ptr node() override;
    explicit BoundRelation(model_ptr<Relation> const& ptr);
    model_ptr<Relation> modelNodePtr_;
};

struct BoundAttribute : public BoundObject<Attribute>
{
    static void bind(py::module_& m)
    {
        auto boundClass =
            py::class_<BoundAttribute, BoundModelNode>(m, "Attribute")
                .def(
                    "validity",
                    [](BoundAttribute& self) { return BoundMultiValidity(self.modelNodePtr_->validity()); },
                    "Get or create the attribute validity collection.")
                .def(
                    "validity_or_none",
                    [](BoundAttribute& self) -> py::object {
                        if (auto validity = self.modelNodePtr_->validityOrNull())
                            return py::cast(BoundMultiValidity(validity));
                        return py::none();
                    },
                    "Get the attribute validity collection if present.")
                .def(
                    "set_validity",
                    [](BoundAttribute& self, BoundMultiValidity const& validity) {
                        self.modelNodePtr_->setValidity(validity.modelNodePtr_);
                    },
                    py::arg("validity"),
                    "Assign an existing validity collection to this attribute.")
                .def(
                    "name",
                    [](BoundAttribute& self) { return self.modelNodePtr_->name(); },
                    "Get the name of the attribute.")
                .def(
                    "source_data_references",
                    [](BoundAttribute& self) -> py::object {
                        if (auto refs = self.modelNodePtr_->sourceDataReferences())
                            return py::cast(BoundSourceDataReferenceCollection(refs));
                        return py::none();
                    },
                    "Get source-data references attached to this attribute.")
                .def(
                    "set_source_data_references",
                    [](BoundAttribute& self, BoundSourceDataReferenceCollection const& refs) {
                        self.modelNodePtr_->setSourceDataReferences(refs.modelNodePtr_);
                    },
                    py::arg("refs"),
                    "Attach source-data references to this attribute.");

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
                "Get the feature ID's type ID.")
            .def(
                "map_id",
                [](BoundFeatureId& self) { return self.modelNodePtr_->mapId(); },
                "Get the effective map ID referenced by this feature ID.")
            .def(
                "external_map_id",
                [](BoundFeatureId& self) { return self.modelNodePtr_->externalMapId(); },
                "Get the explicitly stored external map ID, or None for local references.")
            .def(
                "key_value_pairs",
                [](BoundFeatureId& self) {
                    KeyValuePairVec result;
                    for (auto const& [key, value] : self.modelNodePtr_->keyValuePairs()) {
                        std::visit(
                            [&result, &key](auto&& vv) {
                                using V = std::decay_t<decltype(vv)>;
                                if constexpr (std::is_same_v<V, std::string_view>)
                                    result.emplace_back(std::string(key), std::string(vv));
                                else
                                    result.emplace_back(std::string(key), vv);
                            },
                            value);
                    }
                    return result;
                },
                "Get feature-id parts as name/value pairs.");
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
                "lod",
                [](BoundFeature& self) {
                    return static_cast<uint32_t>(self.modelNodePtr_->lod());
                },
                "Get this feature's level-of-detail value as an integer in [0, 7].")
            .def(
                "set_lod",
                [](BoundFeature& self, uint32_t lod) {
                    if (lod > static_cast<uint32_t>(Feature::MAX_LOD))
                        throw py::value_error("Feature LOD must be in the range [0, 7].");
                    self.modelNodePtr_->setLod(static_cast<Feature::LOD>(lod));
                },
                py::arg("lod"),
                "Set this feature's level-of-detail value as an integer in [0, 7].")
            .def(
                "relations",
                [](BoundFeature& self) {
                    py::list result;
                    self.modelNodePtr_->forEachRelation(
                        [&result](model_ptr<Relation> const& relation) {
                            result.append(BoundRelation(relation));
                            return true;
                        });
                    return result;
                },
                "Get this feature's relations as typed Relation objects.")
            .def(
                "num_relations",
                [](BoundFeature& self) { return self.modelNodePtr_->numRelations(); },
                "Get the number of relations attached to this feature.")
            .def(
                "relation_at",
                [](BoundFeature& self, int64_t i) {
                    auto sz = (int64_t)self.modelNodePtr_->numRelations();
                    if (i < 0) i += sz;
                    if (i < 0 || i >= sz) throw py::index_error();
                    return BoundRelation(self.modelNodePtr_->getRelation((uint32_t)i));
                },
                py::arg("index"),
                "Get a typed Relation at the given index.")
            .def(
                "add_relation",
                [](BoundFeature& self, std::string_view const& name, BoundFeatureId const& target) {
                    return BoundRelation(self.modelNodePtr_->addRelation(name, target.modelNodePtr_));
                },
                py::arg("name"),
                py::arg("target"),
                "Create and attach a named relation to an existing target FeatureId.")
            .def(
                "add_relation",
                [](BoundFeature& self, BoundRelation const& relation) {
                    return BoundRelation(self.modelNodePtr_->addRelation(relation.modelNodePtr_));
                },
                py::arg("relation"),
                "Attach an existing Relation object to this feature.")
            .def(
                "add_relation",
                [](BoundFeature& self,
                   std::string_view const& name,
                   std::string_view const& targetType,
                   KeyValuePairVec const& targetIdParts) {
                    return BoundRelation(self.modelNodePtr_->addRelation(
                        name,
                        targetType,
                        castToKeyValueView(targetIdParts)));
                },
                py::arg("name"),
                py::arg("target_type"),
                py::arg("target_id_parts"),
                "Create and attach a named relation by target type and id parts.")
            .def(
                "source_data_references",
                [](BoundFeature& self) -> py::object {
                    if (auto refs = self.modelNodePtr_->sourceDataReferences())
                        return py::cast(BoundSourceDataReferenceCollection(refs));
                    return py::none();
                },
                "Get source-data references attached to this feature.")
            .def(
                "set_source_data_references",
                [](BoundFeature& self, BoundSourceDataReferenceCollection const& refs) {
                    self.modelNodePtr_->setSourceDataReferences(refs.modelNodePtr_);
                },
                py::arg("refs"),
                "Attach source-data references to this feature.")
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

inline BoundValidity::BoundValidity(model_ptr<Validity> const& ptr) : modelNodePtr_(ptr) {}

inline ModelNode::Ptr BoundValidity::node() { return modelNodePtr_; }

inline void BoundValidity::bind(py::module_& m)
{
    py::enum_<Validity::Direction>(m, "Direction")
        .value("EMPTY", Validity::Direction::Empty)
        .value("POSITIVE", Validity::Direction::Positive)
        .value("NEGATIVE", Validity::Direction::Negative)
        .value("COMPLETE", Validity::Direction::Both)
        .value("NONE", Validity::Direction::None);

    py::enum_<Validity::GeometryDescriptionType>(m, "ValidityGeometryDescriptionType")
        .value("NO_GEOMETRY", Validity::NoGeometry)
        .value("SIMPLE_GEOMETRY", Validity::SimpleGeometry)
        .value("OFFSET_POINT", Validity::OffsetPointValidity)
        .value("OFFSET_RANGE", Validity::OffsetRangeValidity)
        .value("FEATURE_TRANSITION", Validity::FeatureTransition);

    py::enum_<Validity::GeometryOffsetType>(m, "ValidityGeometryOffsetType")
        .value("INVALID", Validity::InvalidOffsetType)
        .value("GEO_POSITION", Validity::GeoPosOffset)
        .value("BUFFER", Validity::BufferOffset)
        .value("RELATIVE_LENGTH", Validity::RelativeLengthOffset)
        .value("METRIC_LENGTH", Validity::MetricLengthOffset);

    py::enum_<Validity::TransitionEnd>(m, "TransitionEnd")
        .value("START", Validity::Start)
        .value("END", Validity::End);

    py::class_<BoundValidity, BoundModelNode>(m, "Validity")
        .def("direction", [](BoundValidity& self) { return self.modelNodePtr_->direction(); },
            "Get the direction in which this validity applies.")
        .def("set_direction", [](BoundValidity& self, Validity::Direction direction) {
                self.modelNodePtr_->setDirection(direction);
            },
            py::arg("direction"),
            "Set the direction in which this validity applies.")
        .def("geometry_description_type", [](BoundValidity& self) {
                return self.modelNodePtr_->geometryDescriptionType();
            },
            "Get the kind of geometry restriction stored by this validity.")
        .def("geometry_offset_type", [](BoundValidity& self) {
                return self.modelNodePtr_->geometryOffsetType();
            },
            "Get the offset interpretation used by point/range validities.")
        .def("geometry_stage", [](BoundValidity& self) {
                return self.modelNodePtr_->geometryStage();
            },
            "Get the referenced geometry stage if one is set.")
        .def("set_geometry_stage", [](BoundValidity& self, std::optional<uint32_t> stage) {
                self.modelNodePtr_->setGeometryStage(stage);
            },
            py::arg("stage"),
            "Set or clear the referenced geometry stage.")
        .def("feature_id", [](BoundValidity& self) -> py::object {
                if (auto featureId = self.modelNodePtr_->featureId())
                    return py::cast(BoundFeatureId(featureId));
                return py::none();
            },
            "Get the feature-id referenced by this validity, if present.")
        .def("set_feature_id", [](BoundValidity& self, BoundFeatureId const& featureId) {
                self.modelNodePtr_->setFeatureId(featureId.modelNodePtr_);
            },
            py::arg("feature_id"),
            "Reference another feature's geometry for this validity.")
        .def("offset_point", [](BoundValidity& self) {
                return self.modelNodePtr_->offsetPoint();
            },
            "Get the stored offset point if this is a point validity.")
        .def("set_offset_point", [](BoundValidity& self, Point const& point) {
                self.modelNodePtr_->setOffsetPoint(point);
            },
            py::arg("point"),
            "Set this validity to a geographic point restriction.")
        .def("set_offset_point", [](BoundValidity& self, Validity::GeometryOffsetType offsetType, double point) {
                self.modelNodePtr_->setOffsetPoint(offsetType, point);
            },
            py::arg("offset_type"),
            py::arg("point"),
            "Set this validity to a one-dimensional point offset restriction.")
        .def("offset_range", [](BoundValidity& self) {
                return self.modelNodePtr_->offsetRange();
            },
            "Get the stored offset range if this is a range validity.")
        .def("set_offset_range", [](BoundValidity& self, Point const& start, Point const& end) {
                self.modelNodePtr_->setOffsetRange(start, end);
            },
            py::arg("start"),
            py::arg("end"),
            "Set this validity to a geographic range restriction.")
        .def("set_offset_range", [](BoundValidity& self,
                                    Validity::GeometryOffsetType offsetType,
                                    double start,
                                    double end) {
                self.modelNodePtr_->setOffsetRange(offsetType, start, end);
            },
            py::arg("offset_type"),
            py::arg("start"),
            py::arg("end"),
            "Set this validity to a one-dimensional range offset restriction.")
        .def("simple_geometry", [](BoundValidity& self) -> py::object {
                if (auto geom = self.modelNodePtr_->simpleGeometry())
                    return py::cast(BoundGeometry(geom));
                return py::none();
            },
            "Get the explicit geometry stored by this validity, if present.")
        .def("set_simple_geometry", [](BoundValidity& self, BoundGeometry const& geometry) {
                self.modelNodePtr_->setSimpleGeometry(geometry.modelNodePtr_);
            },
            py::arg("geometry"),
            "Store an explicit geometry as this validity's restriction.")
        .def("transition_number", [](BoundValidity& self) {
                return self.modelNodePtr_->transitionNumber();
            },
            "Get the semantic transition number, if this validity is a transition.")
        .def("transition_from_connected_end", [](BoundValidity& self) {
                return self.modelNodePtr_->transitionFromConnectedEnd();
            },
            "Get the connected endpoint of the transition source feature.")
        .def("transition_to_connected_end", [](BoundValidity& self) {
                return self.modelNodePtr_->transitionToConnectedEnd();
            },
            "Get the connected endpoint of the transition target feature.");
}

inline BoundMultiValidity::BoundMultiValidity(model_ptr<MultiValidity> const& ptr) : modelNodePtr_(ptr) {}

inline ModelNode::Ptr BoundMultiValidity::node() { return modelNodePtr_; }

inline void BoundMultiValidity::bind(py::module_& m)
{
    py::class_<BoundMultiValidity, BoundModelNode>(m, "MultiValidity")
        .def("new_point", [](BoundMultiValidity& self,
                             Point const& point,
                             std::optional<uint32_t> geometryStage,
                             Validity::Direction direction) {
                return BoundValidity(self.modelNodePtr_->newPoint(point, geometryStage, direction));
            },
            py::arg("point"),
            py::arg("geometry_stage") = std::nullopt,
            py::arg("direction") = Validity::Empty,
            "Append a geographic point validity.")
        .def("new_offset_point", [](BoundMultiValidity& self,
                                    Validity::GeometryOffsetType offsetType,
                                    double point,
                                    std::optional<uint32_t> geometryStage,
                                    Validity::Direction direction) {
                return BoundValidity(self.modelNodePtr_->newPoint(offsetType, point, geometryStage, direction));
            },
            py::arg("offset_type"),
            py::arg("point"),
            py::arg("geometry_stage") = std::nullopt,
            py::arg("direction") = Validity::Empty,
            "Append a one-dimensional point-offset validity.")
        .def("new_range", [](BoundMultiValidity& self,
                             Point const& start,
                             Point const& end,
                             std::optional<uint32_t> geometryStage,
                             Validity::Direction direction) {
                return BoundValidity(self.modelNodePtr_->newRange(start, end, geometryStage, direction));
            },
            py::arg("start"),
            py::arg("end"),
            py::arg("geometry_stage") = std::nullopt,
            py::arg("direction") = Validity::Empty,
            "Append a geographic range validity.")
        .def("new_offset_range", [](BoundMultiValidity& self,
                                    Validity::GeometryOffsetType offsetType,
                                    double start,
                                    double end,
                                    std::optional<uint32_t> geometryStage,
                                    Validity::Direction direction) {
                return BoundValidity(self.modelNodePtr_->newRange(offsetType, start, end, geometryStage, direction));
            },
            py::arg("offset_type"),
            py::arg("start"),
            py::arg("end"),
            py::arg("geometry_stage") = std::nullopt,
            py::arg("direction") = Validity::Empty,
            "Append a one-dimensional range-offset validity.")
        .def("new_geometry", [](BoundMultiValidity& self,
                                BoundGeometry const& geometry,
                                Validity::Direction direction) {
                return BoundValidity(self.modelNodePtr_->newGeometry(geometry.modelNodePtr_, direction));
            },
            py::arg("geometry"),
            py::arg("direction") = Validity::Empty,
            "Append a validity that stores an explicit geometry.")
        .def("new_feature_id", [](BoundMultiValidity& self,
                                  BoundFeatureId const& featureId,
                                  Validity::Direction direction) {
                return BoundValidity(self.modelNodePtr_->newFeatureId(featureId.modelNodePtr_, direction));
            },
            py::arg("feature_id"),
            py::arg("direction") = Validity::Empty,
            "Append a validity that references another feature's full geometry.")
        .def("new_geom_stage", [](BoundMultiValidity& self,
                                  uint32_t geometryStage,
                                  Validity::Direction direction) {
                return BoundValidity(self.modelNodePtr_->newGeomStage(geometryStage, direction));
            },
            py::arg("geometry_stage"),
            py::arg("direction") = Validity::Empty,
            "Append a validity that references one staged geometry.")
        .def("new_complete", [](BoundMultiValidity& self, Validity::Direction direction) {
                return BoundValidity(self.modelNodePtr_->newComplete(direction));
            },
            py::arg("direction") = Validity::Empty,
            "Append a validity covering the complete referenced geometry.")
        .def("new_direction", [](BoundMultiValidity& self, Validity::Direction direction) {
                return BoundValidity(self.modelNodePtr_->newDirection(direction));
            },
            py::arg("direction") = Validity::Empty,
            "Append a direction-only validity.")
        .def("__len__", [](BoundMultiValidity& self) { return self.modelNodePtr_->size(); })
        .def("__getitem__", [](BoundMultiValidity& self, int64_t i) {
            auto sz = (int64_t)self.modelNodePtr_->size();
            if (i < 0) i += sz;
            if (i < 0 || i >= sz) throw py::index_error();
            auto node = self.modelNodePtr_->at(i);
            BoundModelNodeBase base;
            base.modelNodePtr_ = node;
            return BoundValidity(base.featureLayer().resolve<Validity>(*node));
        })
        .def("__iter__", [](BoundMultiValidity& self) {
            py::list result;
            for (auto i = 0u; i < self.modelNodePtr_->size(); ++i) {
                auto node = self.modelNodePtr_->at(i);
                BoundModelNodeBase base;
                base.modelNodePtr_ = node;
                result.append(BoundValidity(base.featureLayer().resolve<Validity>(*node)));
            }
            return py::iter(result);
        });
}

inline BoundSourceDataReferenceCollection::BoundSourceDataReferenceCollection(
    model_ptr<SourceDataReferenceCollection> const& ptr)
    : modelNodePtr_(ptr)
{
}

inline ModelNode::Ptr BoundSourceDataReferenceCollection::node() { return modelNodePtr_; }

inline void BoundSourceDataReferenceCollection::bind(py::module_& m)
{
    py::class_<SourceDataAddress>(m, "SourceDataAddress")
        .def(py::init<>(), "Construct an empty source-data address.")
        .def(py::init<uint32_t, uint32_t>(),
            py::arg("bit_offset"),
            py::arg("bit_size"),
            "Construct a source-data address from bit offset and bit size.")
        .def(py::init<uint64_t>(), py::arg("value"), "Construct a source-data address from its packed integer.")
        .def("value", &SourceDataAddress::u64, "Get the packed 64-bit address value.")
        .def("bit_offset", &SourceDataAddress::bitOffset, "Get the bit offset.")
        .def("bit_size", &SourceDataAddress::bitSize, "Get the bit size.");

    py::class_<BoundSourceDataReferenceCollection, BoundModelNode>(m, "SourceDataReferenceCollection")
        .def("__len__", [](BoundSourceDataReferenceCollection& self) { return self.modelNodePtr_->size(); })
        .def("__iter__", [](BoundSourceDataReferenceCollection& self) {
            py::list result;
            self.modelNodePtr_->forEachReference([&result](SourceDataReferenceItem const& item) {
                py::dict entry;
                entry["layer_id"] = py::str(std::string(item.layerId()));
                entry["qualifier"] = py::str(std::string(item.qualifier()));
                entry["address"] = item.address();
                result.append(entry);
            });
            return py::iter(result);
        })
        .def("to_list", [](BoundSourceDataReferenceCollection& self) {
            py::list result;
            self.modelNodePtr_->forEachReference([&result](SourceDataReferenceItem const& item) {
                py::dict entry;
                entry["layer_id"] = py::str(std::string(item.layerId()));
                entry["qualifier"] = py::str(std::string(item.qualifier()));
                entry["address"] = py::int_(item.address().u64());
                result.append(entry);
            });
            return result;
        }, "Convert references to Python dictionaries with packed integer addresses.");
}

inline BoundRelation::BoundRelation(model_ptr<Relation> const& ptr) : modelNodePtr_(ptr) {}

inline ModelNode::Ptr BoundRelation::node() { return modelNodePtr_; }

inline void BoundRelation::bind(py::module_& m)
{
    py::class_<BoundRelation, BoundModelNode>(m, "Relation")
        .def("name", [](BoundRelation& self) { return self.modelNodePtr_->name(); },
            "Get the relation name.")
        .def("target", [](BoundRelation& self) { return BoundFeatureId(self.modelNodePtr_->target()); },
            "Get the target feature id.")
        .def("source_validity", [](BoundRelation& self) {
                return BoundMultiValidity(self.modelNodePtr_->sourceValidity());
            },
            "Get or create the source-side validity collection.")
        .def("source_validity_or_none", [](BoundRelation& self) -> py::object {
                if (auto validity = self.modelNodePtr_->sourceValidityOrNull())
                    return py::cast(BoundMultiValidity(validity));
                return py::none();
            },
            "Get the source-side validity collection if present.")
        .def("set_source_validity", [](BoundRelation& self, BoundMultiValidity const& validity) {
                self.modelNodePtr_->setSourceValidity(validity.modelNodePtr_);
            },
            py::arg("validity"),
            "Assign an existing source-side validity collection.")
        .def("target_validity", [](BoundRelation& self) {
                return BoundMultiValidity(self.modelNodePtr_->targetValidity());
            },
            "Get or create the target-side validity collection.")
        .def("target_validity_or_none", [](BoundRelation& self) -> py::object {
                if (auto validity = self.modelNodePtr_->targetValidityOrNull())
                    return py::cast(BoundMultiValidity(validity));
                return py::none();
            },
            "Get the target-side validity collection if present.")
        .def("set_target_validity", [](BoundRelation& self, BoundMultiValidity const& validity) {
                self.modelNodePtr_->setTargetValidity(validity.modelNodePtr_);
            },
            py::arg("validity"),
            "Assign an existing target-side validity collection.")
        .def("source_data_references", [](BoundRelation& self) -> py::object {
                if (auto refs = self.modelNodePtr_->sourceDataReferences())
                    return py::cast(BoundSourceDataReferenceCollection(refs));
                return py::none();
            },
            "Get source-data references attached to this relation.")
        .def("set_source_data_references", [](BoundRelation& self, BoundSourceDataReferenceCollection const& refs) {
                self.modelNodePtr_->setSourceDataReferences(refs.modelNodePtr_);
            },
            py::arg("refs"),
            "Attach source-data references to this relation.");
}

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
py::object nodeToPython(model_ptr<ModelNode> const& n, simfil::ModelPool& model, bool checkMultimap)
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
                    if (auto key = model.lookupStringId(fieldId)) {
                        auto pyKey = py::str(std::string(*key));
                        if (d.contains(pyKey))
                            d[pyKey].cast<py::list>().append(nodeToPython(child, model));
                        else
                            d[pyKey] = py::list(py::make_tuple(nodeToPython(child, model)));
                    }
                }
                d[py::str("_multimap")] = py::bool_(true);
                return d;
            }
        }
        for (auto const& [fieldId, child] : n->fields()) {
            if (auto key = model.lookupStringId(fieldId))
                d[py::str(std::string(*key))] = nodeToPython(child, model);
        }
        return d;
    }
    if (type == ValueType::Array) {
        py::list l;
        for (uint32_t i = 0; i < n->size(); ++i)
            l.append(nodeToPython(n->at(i), model));
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
    mapget::BoundFeatureId::bind(m);
    mapget::BoundValidity::bind(m);
    mapget::BoundMultiValidity::bind(m);
    mapget::BoundSourceDataReferenceCollection::bind(m);
    mapget::BoundAttribute::bind(m);
    mapget::BoundAttributeLayer::bind(m);
    mapget::BoundAttributeLayerList::bind(m);
    mapget::BoundRelation::bind(m);
    mapget::BoundFeature::bind(m);
}
