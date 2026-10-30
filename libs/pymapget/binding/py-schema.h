#pragma once

#include "mapget/model/mapschema.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <sstream>

namespace pybind11::detail
{
// Register the non-cv Python type while retaining the native const shared owner.
// No mutable ModelPool base, constructors or schema mutators are exposed below.
template <>
struct type_caster<mapget::MapSchema::ConstPtr>
    : copyable_holder_caster<mapget::MapSchema, mapget::MapSchema::ConstPtr>
{
    static handle
    cast(mapget::MapSchema::ConstPtr const& schema, return_value_policy policy, handle parent)
    {
        if (!schema || !schema->finalized())
            throw value_error("A finalized MapSchema is required");
        return copyable_holder_caster::cast(schema, policy, parent);
    }
};
}  // namespace pybind11::detail

namespace mapget
{

/** Bind immutable schema inspection and exact binary I/O, not a second Python schema registry. */
inline void bindSchema(pybind11::module_& m)
{
    namespace py = pybind11;

    py::class_<simfil::SchemaReference>(m, "SchemaReference", R"pbdoc(
        Read-only exact domain identity retaining its immutable graph.
        domain is local to schema_id. An empty reference has domain zero and an empty
        identity; no semantics may be inferred from its raw scalar value.
    )pbdoc")
        .def_property_readonly(
            "schema_id",
            [](simfil::SchemaReference const& self)
            { return self ? std::string(self.binding->identity()) : std::string{}; },
            "Exact schema token, or an empty string for unknown provenance.")
        .def_readonly(
            "domain",
            &simfil::SchemaReference::id,
            "Local domain ID; zero means unknown.")
        .def(
            "__bool__",
            [](simfil::SchemaReference const& self) { return bool(self); },
            "Whether this reference identifies a retained domain.");

    py::class_<simfil::Bitmask>(m, "Bitmask", R"pbdoc(
        Read-only full-width flag value with intrinsic schema identity.
        bits is an exact Python integer, not a label-table index. Unknown bits and zero
        remain valid raw values. The domain retains its graph after source destruction.
    )pbdoc")
        .def_readonly(
            "bits",
            &simfil::Bitmask::bits,
            "Exact unsigned 64-bit bits, including unknown bits.")
        .def_readonly("domain", &simfil::Bitmask::domain, "Owner-retaining SchemaReference.")
        .def(
            "to_dict",
            [](simfil::Bitmask const& self)
            { return py::module_::import("json").attr("loads")(self.toJson().dump()); },
            "Return simfil's exact {_bitmask, bits, schema, domain} JSON representation.");

    py::class_<MapFormat>(m, "MapFormat", R"pbdoc(
        Read-only descriptive source-format tuple, not a schema cache key.

        name and version are opaque strings; an empty string means unspecified.
        subsystems maps direct content-module names to exact versions, not all
        transitive decoder dependencies. Equal formats need not have equal schemas.
    )pbdoc")
        .def(
            py::init(
                [](std::string name,
                   std::string version,
                   std::map<std::string, std::string, std::less<>> subsystems)
                {
                    return MapFormat::fromJson(
                        {{"name", name}, {"version", version}, {"subsystems", subsystems}});
                }),
            py::arg("name") = "",
            py::arg("version") = "",
            py::arg("subsystems") = std::map<std::string, std::string, std::less<>>{},
            "Create a format value. This does not select, fetch or construct a schema.")
        .def_readonly("name", &MapFormat::name_, "Source-format family, or an empty string.")
        .def_readonly("version", &MapFormat::version_, "Exact base version, or an empty string.")
        .def_readonly(
            "subsystems",
            &MapFormat::subsystems_,
            "A detached dictionary of direct subsystem names and versions.")
        .def(
            "to_dict",
            [](MapFormat const& self)
            {
                py::dict result;
                result["name"] = self.name_;
                result["version"] = self.version_;
                result["subsystems"] = py::cast(self.subsystems_);
                return result;
            },
            "Return a detached canonical mapFormat dictionary.")
        .def(
            "__eq__",
            &MapFormat::operator==,
            py::is_operator(),
            "Compare the complete tuple, including subsystem versions.");

    py::enum_<IdPartDataType>(m, "IdPartDataType", "Declared feature-ID part encoding.")
        .value("I32", IdPartDataType::I32, "Signed 32-bit integer.")
        .value("U32", IdPartDataType::U32, "Unsigned 32-bit integer.")
        .value("I64", IdPartDataType::I64, "Signed 64-bit integer.")
        .value("U64", IdPartDataType::U64, "Unsigned 64-bit integer.")
        .value("UUID128", IdPartDataType::UUID128, "128-bit UUID.")
        .value("STR", IdPartDataType::STR, "String identifier component.");

    py::class_<IdPart>(m, "IdPart", R"pbdoc(
        Read-only schema-owned identifier part. Retaining this view retains its schema.

        Parts are ordered within a composition. Synthetic parts are converter-added;
        optional parts do not distinguish otherwise identical primary feature IDs.
    )pbdoc")
        .def_readonly("name", &IdPart::idPartLabel_, "ID-part label (JSON partId).")
        .def_readonly("description", &IdPart::description_, "Producer description, possibly empty.")
        .def_readonly("datatype", &IdPart::datatype_, "Declared IdPartDataType.")
        .def_readonly(
            "is_synthetic",
            &IdPart::isSynthetic_,
            "Whether the converter adds this part.")
        .def_readonly(
            "is_optional",
            &IdPart::isOptional_,
            "Whether a reference may omit this part.");

    py::class_<FeatureTypeInfo>(m, "FeatureTypeInfo", R"pbdoc(
        Read-only view of one authoritative feature identity definition.

        The view retains its layer and immutable MapSchema; it is not a copied registry.
        A reference-only type can have an identity definition without being produced
        by this layer. Check LayerSchema.produced_feature_types before selecting it.
    )pbdoc")
        .def_readonly("name", &FeatureTypeInfo::name_, "Exact type name used in canonical IDs.")
        .def_property_readonly(
            "unique_id_compositions",
            [](py::object self)
            {
                auto const& type = self.cast<FeatureTypeInfo const&>();
                py::list compositions;
                for (auto const& composition : type.uniqueIdCompositions_) {
                    py::list parts;
                    for (auto const& part : composition)
                        parts.append(
                            py::cast(&part, py::return_value_policy::reference_internal, self));
                    compositions.append(py::tuple(parts));
                }
                return py::tuple(compositions);
            },
            R"pbdoc(
            Tuple of ordered tuples of IdPart views. The first composition is primary
            and required for produced features; later compositions are reference-only
            alternatives. All views keep the original schema alive.
        )pbdoc");

    py::class_<MapSchema, MapSchema::ConstPtr>(m, "MapSchema", R"pbdoc(
        An immutable, shared native schema artifact with exact binary identity.

        Obtain it from DataSourceServer.map_schema, Client.get_schema, MapSchema.read,
        or MapSchema.from_metadata for legacy inputs. No Python mutators or detached
        feature registry are exposed. Layers and identity-part views retain this graph
        independently of the client, datasource, or Python MapSchema object.
    )pbdoc")
        .def_static(
            "read",
            [](py::bytes const& data, std::string const& expectedId, size_t maxBytes)
            {
                auto size = static_cast<size_t>(py::len(data));
                if (size > maxBytes)
                    throw py::value_error("MapSchema input exceeds max_bytes");
                // Keep the bytes argument alive across the GIL release; no JSON/base64 conversion.
                auto* bytes = reinterpret_cast<uint8_t const*>(PyBytes_AsString(data.ptr()));
                MapSchema::ConstPtr schema;
                {
                    py::gil_scoped_release release;
                    schema = MapSchema::read({bytes, size}, expectedId, maxBytes);
                }
                return schema;
            },
            py::arg("data"),
            py::arg("expected_id") = "",
            py::arg("max_bytes") = 64 * 1024 * 1024,
            R"pbdoc(
                Decode native binary bytes and return a finalized shared artifact.

                expected_id, when nonempty, must equal the artifact's exact schemaId;
                a format tuple is not a substitute. max_bytes bounds the entire input
                (default 64 MiB). Unsupported versions, malformed data, over-limit input
                and identity mismatch raise; no partial graph is returned. No network I/O.
            )pbdoc")
        .def_static(
            "from_metadata",
            [](py::dict const& metadata)
            {
                auto text = py::module_::import("json").attr("dumps")(metadata).cast<std::string>();
                auto info = DataSourceInfo::fromJson(nlohmann::json::parse(text));
                return info.mapSchema_;
            },
            py::arg("info_dict"),
            R"pbdoc(
            Normalize legacy datasource metadata using the native input adapter.

            featureTypes and optional featureModelSchema are accepted unchanged.
            Identity-only inputs produce open property domains; missing or conflicting
            identities raise. This does not resolve canonical schemaId references:
            obtain their exact artifact with read() or Client.get_schema() instead.
        )pbdoc")
        .def(
            "write",
            [](MapSchema const& self)
            {
                std::ostringstream stream(std::ios::out | std::ios::binary);
                {
                    py::gil_scoped_release release;
                    auto result = self.write(stream);
                    if (!result)
                        throw std::runtime_error(result.error().message);
                }
                return py::bytes(stream.str());
            },
            R"pbdoc(
            Return exact native binary bytes, including schema identity and dictionary.
            A serialization failure raises instead of returning partial bytes. The output
            is accepted by MapSchema.read; it is not JSON Schema or a tile stream.
        )pbdoc")
        .def_property_readonly(
            "schema_id",
            [](MapSchema const& self) { return std::string(self.schemaId()); },
            "Exact opaque artifact identity. Local numeric domain IDs require this token.")
        .def_property_readonly(
            "format",
            [](MapSchema const& self) { return self.format(); },
            "Read-only MapFormat value; descriptive, not a cache key.")
        .def_property_readonly(
            "layer_names",
            [](MapSchema const& self) { return py::tuple(py::cast(self.layerNames())); },
            "Tuple of schema root names. Catalog availability and authorization are separate.")
        .def_property_readonly(
            "domain_count",
            [](MapSchema const& self) { return self.domainCount(); },
            "Number of domains in the shared graph, not a maximum valid domain ID.")
        .def(
            "layer",
            [](MapSchema const& self, std::string const& name)
            {
                auto layer = self.layer(name);
                if (!layer)
                    throw py::key_error(name);
                return layer;
            },
            py::arg("layer_id"),
            R"pbdoc(
            Return an owner-retaining LayerSchema selected by its exact canonical ID.
            Unknown names raise KeyError. Schema membership alone grants no source access
            and does not make an unadvertised layer available.
        )pbdoc");

    // model_ptr stores its node inline: bind the native value, not a relocating smart holder.
    py::class_<LayerSchema::Ptr>(m, "LayerSchema", R"pbdoc(
        Read-only layer view into a shared MapSchema, selected by canonical layer ID.
        Obtained from MapSchema.layer or a partition's layer_schema property.
    )pbdoc")
        .def_property_readonly(
            "name",
            [](LayerSchema::Ptr const& self) { return std::string(self->name()); },
            "Exact canonical layer ID, not a display name or alias.")
        .def_property_readonly(
            "type",
            [](LayerSchema::Ptr const& self) { return self->layerType(); },
            "LayerType required by the matching datasource LayerInfo.")
        .def_property_readonly(
            "map_schema",
            [](LayerSchema::Ptr const& self) { return self->mapSchema(); },
            "The immutable owner, retained independently of the datasource.")
        .def_property_readonly(
            "root_schema",
            [](LayerSchema::Ptr const& self) { return self->rootSchema(); },
            "Layer root's numeric domain ID; zero means unknown, never a fabricated domain.")
        .def_property_readonly(
            "produced_feature_types",
            [](LayerSchema::Ptr const& self) { return py::tuple(py::cast(self->featureTypes())); },
            "Tuple of produced feature-root names, excluding reference-only identity definitions.")
        .def_property_readonly(
            "feature_types",
            [](py::object self)
            {
                auto const& layer = self.cast<LayerSchema::Ptr const&>();
                py::list types;
                for (auto const& type : layer->featureTypeInfos())
                    types
                        .append(py::cast(&type, py::return_value_policy::reference_internal, self));
                return py::tuple(types);
            },
            "Tuple of schema-owned FeatureTypeInfo views, including reference-only types.")
        .def(
            "get_type_info",
            [](py::object self, std::string const& name)
            {
                auto const& layer = self.cast<LayerSchema::Ptr const&>();
                auto type = layer->getTypeInfo(name, false);
                if (!type)
                    throw py::key_error(name);
                return py::cast(type, py::return_value_policy::reference_internal, self);
            },
            py::arg("name"),
            "Return an owner-retaining FeatureTypeInfo; unknown names raise KeyError.")
        .def(
            "feature_schema",
            [](LayerSchema::Ptr const& self, std::string const& name)
            { return self->featureSchema(name); },
            py::arg("feature_type"),
            "Return the local feature-root domain ID, or zero when no root exists.")
        .def(
            "to_json_schema",
            [](LayerSchema::Ptr const& self)
            {
                auto text = self->toJsonSchema().dump();
                return py::module_::import("json").attr("loads")(text);
            },
            R"pbdoc(
            Explicitly export a detached JSON Schema dictionary for interoperability.
            This can be large; routine discovery should use native layer/type views.
            It is not the binary artifact and cannot replace exact schema identity.
        )pbdoc");
}

}  // namespace mapget
