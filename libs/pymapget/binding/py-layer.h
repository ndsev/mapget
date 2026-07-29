#pragma once

#include "mapget/model/featurelayer.h"
#include "mapget/model/subsetlayer.h"
#include "mapget/model/sourcedata.h"
#include "mapget/model/sourcedatalayer.h"

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "simfil/simfil.h"

#include "py-model.h"

namespace py = pybind11;
using namespace py::literals;

namespace mapget
{

inline py::object layerJsonToPython(nlohmann::json const& j)
{
    switch (j.type()) {
    case nlohmann::json::value_t::null:
        return py::none();
    case nlohmann::json::value_t::boolean:
        return py::bool_(j.get<bool>());
    case nlohmann::json::value_t::number_integer:
        return py::int_(j.get<int64_t>());
    case nlohmann::json::value_t::number_unsigned:
        return py::int_(j.get<uint64_t>());
    case nlohmann::json::value_t::number_float:
        return py::float_(j.get<double>());
    case nlohmann::json::value_t::string:
        return py::str(j.get<std::string>());
    case nlohmann::json::value_t::array: {
        py::list result;
        for (auto const& item : j) {
            result.append(layerJsonToPython(item));
        }
        return result;
    }
    case nlohmann::json::value_t::object: {
        py::dict result;
        for (auto const& [key, value] : j.items()) {
            result[py::str(key)] = layerJsonToPython(value);
        }
        return result;
    }
    default:
        raise("Unsupported JSON value type.");
    }
}

inline py::list diagnosticsToPython(simfil::Diagnostics const& diagnostics)
{
    py::list result;
    auto messages = simfil::diagnostics(diagnostics);
    if (!messages) {
        return result;
    }
    for (auto const& message : *messages) {
        py::dict item;
        item["message"] = message.message;
        py::dict location;
        location["offset"] = message.location.offset;
        location["size"] = message.location.size;
        item["location"] = std::move(location);
        if (message.fix) {
            item["fix"] = *message.fix;
        }
        result.append(std::move(item));
    }
    return result;
}

template<typename Node>
struct BoundSubsetNode : public BoundModelNode
{
    explicit BoundSubsetNode(model_ptr<Node> ptr)
        : modelNodePtr_(std::move(ptr))
    {}

    ModelNode::Ptr node() override { return modelNodePtr_; }
    model_ptr<Node> modelNodePtr_;
};

struct BoundFilterTrace : BoundSubsetNode<FilterTrace>
{
    using BoundSubsetNode::BoundSubsetNode;
};

struct BoundFeatureEntry : BoundSubsetNode<FeatureEntry>
{
    using BoundSubsetNode::BoundSubsetNode;
};

struct BoundAttributeValidityEntry
    : BoundSubsetNode<AttributeValidityEntry>
{
    using BoundSubsetNode::BoundSubsetNode;
};

struct BoundRelationEntry : BoundSubsetNode<RelationEntry>
{
    using BoundSubsetNode::BoundSubsetNode;
};

struct BoundGroupEntry : BoundSubsetNode<GroupEntry>
{
    using BoundSubsetNode::BoundSubsetNode;
};

struct BoundTileSubsetChannel : BoundSubsetNode<TileSubsetChannel>
{
    using BoundSubsetNode::BoundSubsetNode;
};

inline void bindSubsetNodes(py::module_& m)
{
    py::class_<BoundFilterTrace, BoundModelNode>(
        m,
        "FilterTrace")
        .def("name", [](BoundFilterTrace& self) {
            return self.modelNodePtr_->name();
        })
        .def("calls", [](BoundFilterTrace& self) {
            return self.modelNodePtr_->calls();
        })
        .def("total_us", [](BoundFilterTrace& self) {
            return self.modelNodePtr_->totalUs().count();
        })
        .def("values", [](BoundFilterTrace& self) {
            return BoundArray(self.modelNodePtr_->values());
        });

    py::class_<BoundFeatureEntry, BoundModelNode>(
        m,
        "FeatureEntry")
        .def("feature_id", [](BoundFeatureEntry& self) {
            return BoundFeatureId(self.modelNodePtr_->featureId());
        })
        .def("geometry", [](BoundFeatureEntry& self) {
            return BoundGeometryCollection(
                self.modelNodePtr_->geometry());
        })
        .def("values", [](BoundFeatureEntry& self) {
            return BoundArray(self.modelNodePtr_->values());
        });

    py::class_<BoundAttributeValidityEntry, BoundModelNode>(
        m,
        "AttributeValidityEntry")
        .def("feature_id", [](BoundAttributeValidityEntry& self) {
            return BoundFeatureId(self.modelNodePtr_->featureId());
        })
        .def("geometry", [](BoundAttributeValidityEntry& self) {
            return BoundGeometryCollection(
                self.modelNodePtr_->geometry());
        })
        .def("host_values", [](BoundAttributeValidityEntry& self) {
            return BoundArray(self.modelNodePtr_->hostValues());
        })
        .def("values", [](BoundAttributeValidityEntry& self) {
            return BoundArray(self.modelNodePtr_->values());
        })
        .def("attribute_layer", [](BoundAttributeValidityEntry& self) {
            return self.modelNodePtr_->attributeLayer();
        })
        .def("attribute_name", [](BoundAttributeValidityEntry& self) {
            return self.modelNodePtr_->attributeName();
        })
        .def("attribute_index", [](BoundAttributeValidityEntry& self) {
            return self.modelNodePtr_->attributeIndex();
        })
        .def("has_validity", [](BoundAttributeValidityEntry& self) {
            return self.modelNodePtr_->hasValidity();
        })
        .def("validity_index", [](BoundAttributeValidityEntry& self) {
            return self.modelNodePtr_->validityIndex();
        })
        .def("validity_count", [](BoundAttributeValidityEntry& self) {
            return self.modelNodePtr_->validityCount();
        });

    py::class_<BoundRelationEntry, BoundModelNode>(
        m,
        "RelationEntry")
        .def("relation_id", [](BoundRelationEntry& self) {
            return self.modelNodePtr_->relationId();
        })
        .def("name", [](BoundRelationEntry& self) {
            return self.modelNodePtr_->name();
        })
        .def("provenance", [](BoundRelationEntry& self) {
            return self.modelNodePtr_->provenance();
        })
        .def("direction", [](BoundRelationEntry& self) {
            return self.modelNodePtr_->direction();
        })
        .def("twoway", [](BoundRelationEntry& self) {
            return self.modelNodePtr_->twoway();
        })
        .def("source", [](BoundRelationEntry& self) {
            return BoundFeatureEntry(self.modelNodePtr_->source());
        })
        .def("target", [](BoundRelationEntry& self) {
            return BoundFeatureEntry(self.modelNodePtr_->target());
        })
        .def("source_geometry", [](BoundRelationEntry& self) {
            return BoundGeometryCollection(
                self.modelNodePtr_->sourceGeometry());
        })
        .def("target_geometry", [](BoundRelationEntry& self) {
            return BoundGeometryCollection(
                self.modelNodePtr_->targetGeometry());
        })
        .def("values", [](BoundRelationEntry& self) {
            return BoundArray(self.modelNodePtr_->values());
        });

    py::class_<BoundGroupEntry, BoundModelNode>(
        m,
        "GroupEntry")
        .def("group_key", [](BoundGroupEntry& self) {
            BoundModelNodeBase result;
            result.modelNodePtr_ = self.modelNodePtr_->groupKey();
            return result;
        })
        .def("representative_feature_id", [](BoundGroupEntry& self) {
            return BoundFeatureId(
                self.modelNodePtr_->representativeFeatureId());
        })
        .def("geometry", [](BoundGroupEntry& self) {
            return BoundGeometryCollection(
                self.modelNodePtr_->geometry());
        })
        .def("values", [](BoundGroupEntry& self) {
            return BoundArray(self.modelNodePtr_->values());
        })
        .def("member_feature_ids", [](BoundGroupEntry& self) {
            return BoundArray(
                self.modelNodePtr_->memberFeatureIds());
        });

    py::class_<BoundTileSubsetChannel, BoundModelNode>(
        m,
        "TileSubsetChannel")
        .def("channel_id", [](BoundTileSubsetChannel& self) {
            return self.modelNodePtr_->channelId();
        })
        .def("scope", [](BoundTileSubsetChannel& self) {
            return self.modelNodePtr_->scope();
        })
        .def("geometry_types", [](BoundTileSubsetChannel& self) {
            return self.modelNodePtr_->geometryTypes();
        })
        .def("geometry_name", [](BoundTileSubsetChannel& self) {
            return self.modelNodePtr_->geometryName();
        })
        .def("feature_fields", [](BoundTileSubsetChannel& self) {
            return self.modelNodePtr_->featureFields();
        })
        .def("entry_fields", [](BoundTileSubsetChannel& self) {
            return self.modelNodePtr_->entryFields();
        })
        .def("entry_count", [](BoundTileSubsetChannel& self) {
            return self.modelNodePtr_->entryCount();
        })
        .def("feature_entries", [](BoundTileSubsetChannel& self) {
            py::list result;
            self.modelNodePtr_->forEachFeatureEntry(
                [&](auto const& entry) {
                    result.append(BoundFeatureEntry(entry));
                    return true;
                });
            return result;
        })
        .def("attribute_validity_entries",
            [](BoundTileSubsetChannel& self) {
                py::list result;
                self.modelNodePtr_->forEachAttributeValidityEntry(
                    [&](auto const& entry) {
                        result.append(
                            BoundAttributeValidityEntry(entry));
                        return true;
                    });
                return result;
            })
        .def("relation_entries", [](BoundTileSubsetChannel& self) {
            py::list result;
            self.modelNodePtr_->forEachRelationEntry(
                [&](auto const& entry) {
                    result.append(BoundRelationEntry(entry));
                    return true;
                });
            return result;
        })
        .def("group_entries", [](BoundTileSubsetChannel& self) {
            py::list result;
            self.modelNodePtr_->forEachGroupEntry(
                [&](auto const& entry) {
                    result.append(BoundGroupEntry(entry));
                    return true;
                });
            return result;
        });
}

struct BoundSourceDataCompound : public BoundModelNode
{
    model_ptr<SourceDataCompoundNode>& ptr() { return *modelNodePtr_; }
    model_ptr<SourceDataCompoundNode> const& ptr() const { return *modelNodePtr_; }

    static void bind(py::module_& m)
    {
        py::class_<BoundSourceDataCompound, BoundModelNode>(m, "SourceDataCompoundNode", R"pbdoc(
            Mutable compound source-data node.

            Source-data compounds can carry a schema name, a source-data address,
            and arbitrary fields used to represent raw/source payload metadata.
        )pbdoc")
            .def("schema_name", [](BoundSourceDataCompound& self) {
                    return self.ptr()->schemaName();
                },
                "Get the schema/type name associated with this source-data node.")
            .def("set_schema_name", [](BoundSourceDataCompound& self, std::string_view const& name) {
                    self.ptr()->setSchemaName(name);
                },
                py::arg("name"),
                "Set the schema/type name associated with this source-data node.")
            .def("source_data_address", [](BoundSourceDataCompound& self) {
                    return self.ptr()->sourceDataAddress();
                },
                "Get the source-data address associated with this node.")
            .def("set_source_data_address", [](BoundSourceDataCompound& self, SourceDataAddress const& address) {
                    self.ptr()->setSourceDataAddress(address);
                },
                py::arg("address"),
                "Set the source-data address associated with this node.")
            .def("is_source_data_address_scope", [](BoundSourceDataCompound& self) {
                    return self.ptr()->isSourceDataAddressScope();
                },
                "Return whether this node starts a presentation address scope.")
            .def("set_source_data_address_scope", [](BoundSourceDataCompound& self, bool enabled) {
                    self.ptr()->setSourceDataAddressScope(enabled);
                },
                py::arg("enabled") = true,
                "Mark this node as the origin of an independently addressed payload.")
            .def("add_field", [](BoundSourceDataCompound& self,
                                  std::string_view const& name,
                                  py::object const& pyValue) {
                    auto cppValue = pyValueToModel(pyValue, self.ptr()->model());
                    auto object = self.ptr()->object();
                    std::visit(
                        [&object, &name](auto&& value) {
                            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, bool>)
                                object->addBool(name, value);
                            else
                                object->addField(name, value);
                        },
                        cppValue);
                },
                py::arg("name"),
                py::arg("value"),
                "Add a field to this source-data compound object.")
            .def("to_dict", [](BoundSourceDataCompound& self) {
                    return nodeToPython(self.node(), self.ptr()->model());
                },
                "Convert this source-data compound object to Python values.");
    }

    ModelNode::Ptr node() override { return ModelNode::Ptr(ptr()); }

    explicit BoundSourceDataCompound(model_ptr<SourceDataCompoundNode> ptr)
        : modelNodePtr_(std::make_shared<model_ptr<SourceDataCompoundNode>>(std::move(ptr)))
    {
    }

    std::shared_ptr<model_ptr<SourceDataCompoundNode>> modelNodePtr_;
};

inline SourceDataAddress sourceDataAddressFromPython(py::handle value)
{
    if (py::isinstance<SourceDataAddress>(value)) {
        return value.cast<SourceDataAddress>();
    }
    return SourceDataAddress(value.cast<uint64_t>());
}

inline model_ptr<SourceDataReferenceCollection> makeSourceDataReferences(
    TileFeatureLayer& self,
    py::iterable const& entries)
{
    std::vector<QualifiedSourceDataReference> refs;
    for (auto const& item : entries) {
        std::string layerId;
        std::string qualifier;
        SourceDataAddress address;

        if (py::isinstance<py::dict>(item)) {
            auto dict = py::reinterpret_borrow<py::dict>(item);
            py::object layerIdObject;
            if (dict.contains("layer_id")) {
                layerIdObject = dict["layer_id"];
            } else {
                layerIdObject = dict["layerId"];
            }
            layerId = py::str(layerIdObject);
            qualifier = py::str(dict["qualifier"]);
            address = sourceDataAddressFromPython(dict["address"]);
        } else {
            auto tuple = py::reinterpret_borrow<py::tuple>(item);
            if (tuple.size() != 3) {
                throw py::value_error("source-data references must be dicts or (layer_id, qualifier, address) tuples");
            }
            layerId = py::str(tuple[0]);
            qualifier = py::str(tuple[1]);
            address = sourceDataAddressFromPython(tuple[2]);
        }

        auto layerStringId = self.strings()->emplace(layerId);
        if (!layerStringId)
            raise(layerStringId.error().message);
        auto qualifierStringId = self.strings()->emplace(qualifier);
        if (!qualifierStringId)
            raise(qualifierStringId.error().message);

        refs.push_back(QualifiedSourceDataReference{
            address,
            *layerStringId,
            *qualifierStringId,
        });
    }

    return self.newSourceDataReferenceCollection(std::span<QualifiedSourceDataReference>{refs});
}

}  // namespace mapget

void bindTileLayer(py::module_& m)
{
    using namespace mapget;
    using namespace simfil;

    py::class_<TileLayer, TileLayer::Ptr>(m, "TileLayer", R"pbdoc(
        Common base class for mapget tile payloads.

        Exposes tile identity, layer identity, error metadata, TTL metadata, and
        arbitrary scalar info fields shared by feature, source-data, and subset
        layers.
    )pbdoc")
        .def("tile_id", &TileLayer::tileId, "Get the layer tile id.")
        .def("map_id", &TileLayer::mapId, "Get the map id.")
        .def("layer_id", [](TileLayer const& self) { return self.layerInfo()->layerId_; },
            "Get the layer id.")
        .def("error", &TileLayer::error, "Get the tile error if one was set.")
        .def("set_error", [](TileLayer& self, std::string const& e) { self.setError(e); },
            py::arg("err"),
            "Set the tile error.")
        .def("error_code", &TileLayer::errorCode, "Get the tile error code if one was set.")
        .def("set_error_code", [](TileLayer& self, int code) { self.setErrorCode(code); },
            py::arg("code"),
            "Set the tile error code.")
        .def("ttl", [](TileLayer const& self) { return self.ttl() ? self.ttl()->count() : -1; },
            "Get the tile TTL in milliseconds, or -1 if unset.")
        .def("set_ttl", [](TileLayer& self, int64_t ms) {
                if (ms >= 0)
                    self.setTtl(std::chrono::milliseconds(ms));
                else
                    self.setTtl(std::nullopt);
            },
            py::arg("time_to_live_in_ms"),
            "Set the tile TTL in milliseconds, or -1 to clear it.")
        .def("set_info", [](TileLayer& self, std::string const& key, simfil::ScalarValueType const& value) {
                std::visit(
                    [&](auto&& vv) {
                        using V = std::decay_t<decltype(vv)>;
                        if constexpr (std::is_same_v<V, std::monostate>) {
                            return;
                        } else if constexpr (std::is_same_v<V, ByteArray>) {
                            self.setInfo(key, vv.toHex());
                        } else {
                            self.setInfo(key, vv);
                        }
                    },
                    value);
            },
            py::arg("key"),
            py::arg("value"),
            "Set a JSON metadata field on this tile.");

    py::class_<TileFeatureLayer, TileLayer, TileFeatureLayer::Ptr>(
        m,
        "TileFeatureLayer",
        R"pbdoc(
        Feature tile payload.

        Datasource callbacks receive this object and fill it with features,
        geometry, attributes, relations, validity data, and optional
        source-data references.
    )pbdoc")
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
            "error_code",
            [](TileFeatureLayer const& self) { return self.errorCode(); },
            R"pbdoc(
            Get the error code (e.g., HTTP status code, SQLite error code)
            if an error occurred while the tile was filled.
            )pbdoc")
        .def(
            "set_error_code",
            [](TileFeatureLayer& self, int code) { self.setErrorCode(code); },
            py::arg("code"),
            R"pbdoc(
            Set the error code (e.g., HTTP status code, SQLite error code)
            for an error that occurred while the tile was filled.
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
                        using V = std::decay_t<decltype(vv)>;
                        if constexpr (std::is_same_v<V, std::monostate>) {
                            return;
                        }
                        else if constexpr (std::is_same_v<V, ByteArray>) {
                            // Store bytes in hex to keep JSON valid and readable.
                            self.setInfo(k, vv.toHex());
                        }
                        else {
                            self.setInfo(k, vv);
                        }
                    },
                    v);
            },
            py::arg("key"),
            py::arg("value"),
            R"pbdoc(
            Set a JSON field to store sizes, construction times,
            and other arbitrary meta-information. The value may be
            bool, int, double or string. ByteArray values are stored as hex strings.
        )pbdoc")
        .def(
            "set_prefix",
            [](TileFeatureLayer& self, KeyValuePairVec const& v) {
                self.setIdPrefix(castToKeyValueView(v)); },
            py::arg("prefix"),
            R"pbdoc(
            Set common id prefix for all features in this layer.
        )pbdoc")
        .def(
            "new_value",
            [](TileFeatureLayer& self, py::object const& pyValue) -> BoundModelNodeBase
            {
                auto cppValue = pyValueToModel(pyValue, self);
                BoundModelNodeBase result;
                std::visit(
                    [&self, &result](auto&& value)
                    {
                        if constexpr (
                            std::is_same_v<std::decay_t<decltype(value)>, bool> ||
                            std::is_same_v<std::decay_t<decltype(value)>, int16_t>)
                            result.modelNodePtr_ = self.newSmallValue(value);
                        else if constexpr (
                            std::is_same_v<std::decay_t<decltype(value)>, ModelNode::Ptr>)
                            result.modelNodePtr_ = value;
                        else
                            result.modelNodePtr_ = self.newValue(value);
                    },
                    cppValue);
                return result;
            },
            py::arg("value"),
            R"pbdoc(
            Create a new model value from any Python value (Supported are:
            List, Dict with string-convertible key, Int, Str, Float, Bool).
        )pbdoc")
        .def(
            "new_feature",
            [](TileFeatureLayer& self, std::string const& typeId, KeyValuePairVec const& idParts)
            { return BoundFeature(self.newFeature(typeId, castToKeyValueView(idParts))); },
            py::arg("type_id"),
            py::arg("feature_id_parts"),
            R"pbdoc(
            Creates a new feature and insert it into this tile layer. The unique identifying
            information, prepended with the getIdPrefix, must conform to an existing
            UniqueIdComposition for the feature typeId within the associated layer.
        )pbdoc")
        .def(
            "new_feature_id",
            [](TileFeatureLayer& self,
               std::string const& typeId,
               KeyValuePairVec const& idParts,
               std::optional<std::string> const& mapId)
            {
                auto externalMapId = mapId
                    ? std::optional<std::string_view>(*mapId)
                    : std::nullopt;
                return BoundFeatureId(
                    self.newFeatureId(
                        typeId,
                        castToKeyValueView(idParts),
                        externalMapId));
            },
            py::arg("type_id"),
            py::arg("feature_id_parts"),
            py::arg("map_id") = py::none(),
            R"pbdoc(
            Create a new feature id. Use this function to create a reference to another
            feature. The created feature id will not use the common feature id prefix
            from this tile feature layer. Pass `map_id` to reference a feature in
            another map.
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
            [](TileFeatureLayer& self, GeomType const& geomType)
            { return BoundGeometry(self.newGeometry(geomType)); },
            py::arg("geom_type"),
            R"pbdoc(
            Create a new geometry of the given type.
        )pbdoc")
        .def(
            "new_relation",
            [](TileFeatureLayer& self, std::string_view const& name, BoundFeatureId const& target)
            { return BoundRelation(self.newRelation(name, target.modelNodePtr_)); },
            py::arg("name"),
            py::arg("target"),
            R"pbdoc(
            Create a new relation object. Attach it to a feature with
            Feature.add_relation(relation) or Feature.add_relation(name, target).
        )pbdoc")
        .def(
            "new_validity_collection",
            [](TileFeatureLayer& self, size_t initialCapacity)
            { return BoundMultiValidity(self.newValidityCollection(initialCapacity)); },
            py::arg("initial_capacity") = 2,
            R"pbdoc(
            Create a new validity collection which can be attached to attributes or relations.
        )pbdoc")
        .def(
            "new_source_data_references",
            [](TileFeatureLayer& self, py::iterable const& entries)
            { return BoundSourceDataReferenceCollection(makeSourceDataReferences(self, entries)); },
            py::arg("entries"),
            R"pbdoc(
            Create qualified source-data references from dictionaries or
            (layer_id, qualifier, address) tuples.
        )pbdoc")
        .def(
            "geojson",
            [](TileFeatureLayer& self)
            { return self.toJson().dump(); },
            R"pbdoc(
            Convert this tile to a GeoJSON feature collection.
        )pbdoc")
        .def_property(
            "glb_attachment_name",
            [](TileFeatureLayer const& self) {
                return self.glbAttachmentName();
            },
            [](TileFeatureLayer& self,
               std::optional<std::string> name) {
                self.setGlbAttachmentName(
                    std::move(name));
            },
            "Optional name of the separately transferred tile GLB.")
        .def("__len__", [](TileFeatureLayer const& self) { return self.size(); })
        .def("__getitem__", [](TileFeatureLayer const& self, int64_t i) {
            auto sz = (int64_t)self.size();
            if (i < 0) i += sz;
            if (i < 0 || i >= sz) throw py::index_error();
            return BoundFeature(self.at((size_t)i));
        });

    py::enum_<TileSourceDataLayer::SourceDataAddressFormat>(m, "SourceDataAddressFormat", R"pbdoc(
        Addressing scheme used by source-data references in a source-data tile.
    )pbdoc")
        .value("UNKNOWN", TileSourceDataLayer::SourceDataAddressFormat::Unknown)
        .value("BIT_RANGE", TileSourceDataLayer::SourceDataAddressFormat::BitRange);

    BoundSourceDataCompound::bind(m);
    bindSubsetNodes(m);

    py::enum_<Scope>(m, "SubsetScope")
        .value("FEATURE", Scope::Feature)
        .value("ATTRIBUTE", Scope::Attribute)
        .value("RELATION", Scope::Relation)
        .value("GROUP", Scope::Group);

    py::enum_<RelationDirection>(m, "RelationDirection")
        .value("FORWARD", RelationDirection::Forward)
        .value("REVERSE", RelationDirection::Reverse);

    py::class_<TileSourceDataLayer, TileLayer, TileSourceDataLayer::Ptr>(
        m,
        "TileSourceDataLayer",
        R"pbdoc(
        Source-data tile payload.

        Source-data layers store root compound nodes that can be referenced by
        features, attributes, relations, or geometry in feature tiles.
    )pbdoc")
        .def("new_compound", [](TileSourceDataLayer& self, size_t initialSize) {
                return BoundSourceDataCompound(self.newCompound(initialSize));
            },
            py::arg("initial_size") = 2,
            "Create a new source-data compound node.")
        .def("add_root", [](TileSourceDataLayer& self, BoundSourceDataCompound const& node) {
                self.addRoot(ModelNode::Ptr(node.ptr()));
            },
            py::arg("node"),
            "Add a source-data compound node as a root of this layer.")
        .def("source_data_address_format", &TileSourceDataLayer::sourceDataAddressFormat,
            "Get the source-data address format.")
        .def("set_source_data_address_format", &TileSourceDataLayer::setSourceDataAddressFormat,
            py::arg("format"),
            "Set the source-data address format.")
        .def("to_json", [](TileSourceDataLayer& self) { return self.toJson().dump(); },
            "Convert this source-data layer to JSON.");

    py::class_<TileSubsetLayer, TileLayer, TileSubsetLayer::Ptr>(
        m,
        "TileSubsetLayer",
        R"pbdoc(
        Immutable multi-channel result returned by server-side `/filter`.
        )pbdoc")
        .def("filter_id", &TileSubsetLayer::filterId)
        .def("generation", &TileSubsetLayer::generation)
        .def("diagnostics", [](TileSubsetLayer const& self) {
                return diagnosticsToPython(self.diagnostics());
            })
        .def("dependencies", [](TileSubsetLayer const& self) {
                py::list result;
                for (auto const& dependency : self.dependencies()) {
                    py::dict item;
                    item["source_tile_key"] =
                        dependency.sourceTileKey_.toString();
                    item["source_feature_count"] =
                        dependency.sourceFeatureCount_;
                    result.append(std::move(item));
                }
                return result;
            })
        .def("issues", [](TileSubsetLayer const& self) {
                py::list result;
                for (auto const& issue : self.issues()) {
                    py::dict item;
                    item["channel_id"] = issue.channelId_;
                    item["expression"] = issue.expression_;
                    item["scope"] = issue.scope_;
                    item["message"] = issue.message_;
                    item["occurrence_count"] =
                        issue.occurrenceCount_;
                    result.append(std::move(item));
                }
                return result;
            })
        .def("glb_attachment_name",
            &TileSubsetLayer::glbAttachmentName)
        .def("trace_count", &TileSubsetLayer::traceCount)
        .def("trace_at", [](TileSubsetLayer const& self, int64_t i) {
                auto sz = static_cast<int64_t>(self.traceCount());
                if (i < 0) i += sz;
                if (i < 0 || i >= sz) {
                    throw py::index_error();
                }
                return BoundFilterTrace(
                    self.traceAt(static_cast<size_t>(i)));
            },
            py::arg("index"))
        .def("traces", [](TileSubsetLayer const& self) {
                py::list result;
                for (size_t i = 0; i < self.traceCount(); ++i) {
                    result.append(BoundFilterTrace(self.traceAt(i)));
                }
                return result;
            })
        .def("to_dict", [](TileSubsetLayer& self) {
                return layerJsonToPython(self.toJson());
            })
        .def("to_json", [](TileSubsetLayer& self) {
                return self.toJson().dump();
            })
        .def("__len__", [](TileSubsetLayer const& self) {
                return self.size();
            })
        .def("__getitem__", [](TileSubsetLayer const& self, int64_t i) {
                auto sz = static_cast<int64_t>(self.size());
                if (i < 0) i += sz;
                if (i < 0 || i >= sz) {
                    throw py::index_error();
                }
                return BoundTileSubsetChannel(
                    self.at(static_cast<size_t>(i)));
            },
            py::arg("index"))
        .def("__iter__", [](TileSubsetLayer const& self) {
                py::list result;
                for (size_t i = 0; i < self.size(); ++i) {
                    result.append(BoundTileSubsetChannel(self.at(i)));
                }
                return py::iter(result);
            });
}
