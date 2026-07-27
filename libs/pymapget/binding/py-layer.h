#pragma once

#include "mapget/model/featurelayer.h"
#include "mapget/model/searchresultlayer.h"
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

struct BoundSearchResult : public BoundModelNode
{
    static void bind(py::module_& m)
    {
        py::class_<BoundSearchResult, BoundModelNode>(m, "SearchResult", R"pbdoc(
            One root entry in a `TileSearchResultLayer`.

            A result represents one matched feature or one matched attribute
            validity context. It carries a copied feature id, copied display
            geometry, optional attribute/validity indices, and the extracted
            `with_fields` values aligned to the parent layer's `result_fields()`.
        )pbdoc")
            .def("feature_id", [](BoundSearchResult& self) {
                    return BoundFeatureId(self.modelNodePtr_->featureId());
                },
                R"pbdoc(
                Return the matched feature id as a `FeatureId` object.
            )pbdoc")
            .def("geometry", [](BoundSearchResult& self) {
                    return BoundGeometryCollection(self.modelNodePtr_->geometry());
                },
                R"pbdoc(
                Return the copied display geometry for this search result.
            )pbdoc")
            .def("attribute_index", [](BoundSearchResult& self) {
                    return self.modelNodePtr_->attributeIndex();
                },
                R"pbdoc(
                Return the matched attribute index for attribute-scope searches.

                Returns `None` for feature-scope matches.
            )pbdoc")
            .def("validity_index", [](BoundSearchResult& self) {
                    return self.modelNodePtr_->validityIndex();
                },
                R"pbdoc(
                Return the matched validity index within the matched attribute.

                Returns `None` when the match is not tied to one validity.
            )pbdoc")
            .def("validity_count", [](BoundSearchResult& self) {
                    return self.modelNodePtr_->validityCount();
                },
                R"pbdoc(
                Return how many validity contexts were considered for the matched attribute.

                Returns `None` for feature-scope matches.
            )pbdoc")
            .def("values", [](BoundSearchResult& self) {
                    return BoundArray(self.modelNodePtr_->values());
                },
                R"pbdoc(
                Return the extracted `with_fields` values as an `Array`.

                The array order is the same as `TileSearchResultLayer.result_fields()`.
            )pbdoc")
            .def("value", [](BoundSearchResult& self, int64_t i) {
                    auto values = self.modelNodePtr_->values();
                    auto sz = static_cast<int64_t>(values->size());
                    if (i < 0) i += sz;
                    if (i < 0 || i >= sz) {
                        throw py::index_error();
                    }
                    BoundModelNodeBase node;
                    node.modelNodePtr_ = values->at(static_cast<uint32_t>(i));
                    return node;
                },
                py::arg("index"),
                R"pbdoc(
                Return one extracted `with_fields` value by index.

                Negative indices follow Python sequence semantics.
            )pbdoc")
            .def("values_list", [](BoundSearchResult& self) {
                    return nodeToPython(self.modelNodePtr_->values(), self.featureModelLayer());
                },
                R"pbdoc(
                Return extracted `with_fields` values as a plain Python list.
            )pbdoc")
            .def("to_dict", [](BoundSearchResult& self) {
                    return nodeToPython(self.node(), self.featureModelLayer());
                },
                R"pbdoc(
                Convert this result to a Python dictionary.
            )pbdoc")
            .def("to_json", [](BoundSearchResult& self) {
                    return self.modelNodePtr_->toJson().dump();
                },
                R"pbdoc(
                Convert this result to a compact JSON string.
            )pbdoc");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundSearchResult(model_ptr<SearchResult> const& ptr) : modelNodePtr_(ptr) {}

    model_ptr<SearchResult> modelNodePtr_;
};

struct BoundSearchTrace : public BoundModelNode
{
    static void bind(py::module_& m)
    {
        py::class_<BoundSearchTrace, BoundModelNode>(m, "SearchTrace", R"pbdoc(
            Typed aggregate produced by SIMFIL `trace(...)` while evaluating a search.

            Trace values are stored in the search-result layer's model pool, so
            they can be inspected without mutating datasource string pools.
        )pbdoc")
            .def("name", [](BoundSearchTrace& self) {
                    return self.modelNodePtr_->name();
                },
                "Return the trace name supplied by the SIMFIL expression.")
            .def("calls", [](BoundSearchTrace& self) {
                    return self.modelNodePtr_->calls();
                },
                "Return how often this trace expression was evaluated.")
            .def("total_us", [](BoundSearchTrace& self) {
                    return self.modelNodePtr_->totalUs().count();
                },
                "Return total evaluation time for this trace in microseconds.")
            .def("values", [](BoundSearchTrace& self) {
                    return BoundArray(self.modelNodePtr_->values());
                },
                "Return sampled trace values as an `Array`.")
            .def("values_list", [](BoundSearchTrace& self) {
                    return nodeToPython(self.modelNodePtr_->values(), self.featureModelLayer());
                },
                "Return sampled trace values as plain Python values.")
            .def("to_dict", [](BoundSearchTrace& self) {
                    return nodeToPython(self.node(), self.featureModelLayer());
                },
                "Convert this trace aggregate to a Python dictionary.")
            .def("to_json", [](BoundSearchTrace& self) {
                    return self.modelNodePtr_->toJson().dump();
                },
                "Convert this trace aggregate to a JSON string.");
    }

    ModelNode::Ptr node() override { return modelNodePtr_; }

    explicit BoundSearchTrace(model_ptr<SearchTrace> const& ptr) : modelNodePtr_(ptr) {}

    model_ptr<SearchTrace> modelNodePtr_;
};

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
        arbitrary scalar info fields shared by feature, source-data, and search
        result layers.
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
            "stage",
            [](TileFeatureLayer const& self) { return self.stage(); },
            R"pbdoc(
            Get the staged-loading index requested for this feature tile, or None
            for unstaged requests.
            )pbdoc")
        .def(
            "set_stage",
            [](TileFeatureLayer& self, std::optional<uint32_t> stage) { self.setStage(stage); },
            py::arg("stage") = std::nullopt,
            R"pbdoc(
            Set or clear the staged-loading index for this feature tile.
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
    BoundSearchResult::bind(m);
    BoundSearchTrace::bind(m);

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

    py::class_<TileSearchResultLayer, TileLayer, TileSearchResultLayer::Ptr>(
        m,
        "TileSearchResultLayer",
        R"pbdoc(
        Tile layer returned by server-side search-as-map requests.

        The layer contains `SearchResult` roots instead of source features.
        Each result carries a copied feature id, display geometry, optional
        attribute-scope indices, and extracted values aligned to
        `result_fields()`.
    )pbdoc")
        .def("stage", [](TileSearchResultLayer const& self) { return self.stage(); },
            R"pbdoc(
            Return the source stage for single-stage results, or `None` for
            assembled staged results.
        )pbdoc")
        .def("result_fields", &TileSearchResultLayer::resultFields,
            R"pbdoc(
            Return the `with_fields` expressions aligned to every result's `values()`.
        )pbdoc")
        .def("diagnostics", [](TileSearchResultLayer const& self) {
                return diagnosticsToPython(self.diagnostics());
            },
            R"pbdoc(
            Return SIMFIL diagnostics collected while evaluating this result chunk.

            The returned value is a list of dictionaries with `message`,
            `location`, and optional `fix` fields.
        )pbdoc")
        .def("trace_count", &TileSearchResultLayer::traceCount,
            "Return the number of typed SIMFIL trace aggregates on this result chunk.")
        .def("trace_at", [](TileSearchResultLayer const& self, int64_t i) {
                auto sz = static_cast<int64_t>(self.traceCount());
                if (i < 0) i += sz;
                if (i < 0 || i >= sz) {
                    throw py::index_error();
                }
                return BoundSearchTrace(self.traceAt(static_cast<size_t>(i)));
            },
            py::arg("index"),
            R"pbdoc(
            Return one `SearchTrace` by index; negative indices are supported.
        )pbdoc")
        .def("traces", [](TileSearchResultLayer const& self) {
                py::list result;
                for (size_t i = 0; i < self.traceCount(); ++i) {
                    result.append(BoundSearchTrace(self.traceAt(i)));
                }
                return result;
            },
            R"pbdoc(
            Return all typed SIMFIL trace aggregates as `SearchTrace` objects.
        )pbdoc")
        .def("to_dict", [](TileSearchResultLayer& self) {
                return layerJsonToPython(self.toJson());
            },
            R"pbdoc(
            Convert this search-result layer to a Python dictionary.
        )pbdoc")
        .def("to_json", [](TileSearchResultLayer& self) { return self.toJson().dump(); },
            R"pbdoc(
            Convert this search-result layer to a JSON string.
        )pbdoc")
        .def("__len__", [](TileSearchResultLayer const& self) { return self.size(); },
            "Return the number of search results in this layer.")
        .def("__getitem__", [](TileSearchResultLayer const& self, int64_t i) {
                auto sz = static_cast<int64_t>(self.size());
                if (i < 0) i += sz;
                if (i < 0 || i >= sz) {
                    throw py::index_error();
                }
                return BoundSearchResult(self.at(static_cast<size_t>(i)));
            },
            py::arg("index"),
            R"pbdoc(
            Return one `SearchResult` by index; negative indices are supported.
        )pbdoc")
        .def("__iter__", [](TileSearchResultLayer const& self) {
                py::list result;
                for (size_t i = 0; i < self.size(); ++i) {
                    result.append(BoundSearchResult(self.at(i)));
                }
                return py::iter(result);
            },
            R"pbdoc(
            Iterate over `SearchResult` objects in this layer.
        )pbdoc");
}
