# Python Schema Access

<!-- mcp:
title: "Python schema owners and legacy metadata"
keywords: [Python, MapSchema, MapFormat, datasource, feature identities, legacy metadata]
-->
## Metadata And Owners

`DataSourceServer.info()` and `Client.sources()` remain dictionary APIs. Canonical output
contains `mapFormat` and `schemaId` references, not inline `featureTypes`,
`featureModelSchema` or `schemaFeatureTypes`. Keep the exact `sourceId` from client discovery:
it is distinct from a map name, string-pool ID and schema identity.

Existing datasource configuration dictionaries with `featureTypes` and optional
`featureModelSchema` remain valid inputs. The native adapter normalizes them into one shared,
finalized graph. Identity-only inputs have open property types, not invented field semantics.
Missing/conflicting identity definitions fail explicitly. Neither tile construction nor ordinary
metadata access performs a hidden schema lookup.

```python
import mapget

legacy_info = {
    "mapId": "Example",
    "layers": {"Road": {"featureTypes": [{
        "name": "Road",
        "uniqueIdCompositions": [[{"partId": "id", "datatype": "U64"}]],
    }]}},
}
source = mapget.DataSourceServer(legacy_info)
schema = source.map_schema
road = schema.layer("Road")
identity = road.get_type_info("Road")
assert identity.unique_id_compositions[0][0].name == "id"

# Canonical references need their exact owner, not the old inline definitions.
replacement = mapget.DataSourceServer(source.info(), map_schema=schema)
```

`MapSchema.from_metadata(info_dict)` uses the same legacy adapter without constructing a
server. It is not a network resolver for canonical references. The optional `map_schema`
argument to `DataSourceServer` must match the input's exact identity, layer names and kinds.
A different schema cannot silently reinterpret the metadata. A matching explicit owner can
also accompany legacy input, but cannot override conflicting declarations.

Native schema authoring with the C++ `MapSchema`/`LayerSchema` builders is not exposed
in Python. Python producers continue to supply legacy metadata dictionaries, or reuse an
exact native artifact obtained through `read()` or client retrieval. `MapSchema` has no
Python constructor or mutators; `from_metadata()` returns an already finalized owner.

Every schema/layer/type/ID-part view retains its original owner. It remains valid after a client,
datasource, tile or higher-level Python wrapper is destroyed. There is no separately owned Python
feature-type registry. Properties are read-only; returned dictionary snapshots can be modified
without altering the graph. A schema can contain roots absent from a particular datasource;
only that source's catalog determines availability and authorization.

<!-- mcp:
title: "Python exact binary schema retrieval"
keywords: [Python, schemaId, sourceId, binary schema, Client, schema fetch]
-->
## Retrieval And Binary

`Client` hydrates each ready source's exact schema before exposing usable metadata or decoding
partitions. `sources()` returns canonical dictionaries including their catalog `sourceId`.
There is no map-name fallback for `get_schema`:

```python
client = mapget.Client("127.0.0.1", 8080)
entry = client.sources()[0]
schema = client.get_schema(entry["sourceId"], entry["schemaId"])
wire = schema.write()
assert isinstance(wire, bytes)
restored = mapget.MapSchema.read(wire, expected_id=entry["schemaId"])
```

`get_schema(source_id, expected_schema_id)` rechecks authorization and exact identity even for
a cached graph. It raises on transport, permission or identity failures. Client headers apply
to schema requests as well as source and tile requests. The returned schema is independently
owner-retaining; it does not require the client to stay alive.

`MapSchema.write()` returns native binary bytes, including the artifact identity and private
schema dictionary. Serialization failures raise instead of returning partial bytes.
`MapSchema.read(data, expected_id="", max_bytes=67108864)` accepts `bytes`, validates the native
envelope/graph and returns an immutable artifact. The default input bound is 64 MiB. A nonempty
`expected_id` must match exactly. Invalid versions, malformed/truncated data, identity mismatches
and oversized input raise; no partial schema is installed. Supply the expected token whenever
the bytes correspond to advertised metadata. This format is neither JSON Schema nor a tile stream.

The current wire formats are tile-stream protocol **6.2**, MapSchema codec **3** and
embedded simfil SchemaPool codec **2**. The `rawValue` static-dictionary addition shifts
mapget field IDs, so 6.1 streams are incompatible even though their framing is unchanged.
Stream compatibility requires equal major/minor versions and ignores patch. Rebuild the
Python wheel, native consumers and erdblick WASM together; old framed payloads and unsupported
schema artifacts are rejected rather than reinterpreted under the new IDs.

Feature, SourceData and subset partition objects expose `map_schema` and `layer_schema` for
their actual bound payload. These do not consult the latest catalog. Client decoding uses
already-installed schema owners, not unresolved dictionary references.

Compatible, independently typed feature add-ons are supported. Datasources convert against
their own native schemas; the service composes those schemas with the primary schema into an
effective artifact and remaps type references while merging the data. Discovery advertises
the effective artifact's exact `schemaId`, which may differ from any contributor's identity.
Conflicting declarations reject the registry update rather than stripping type information.

Each returned partition retains its actual bound schema owner. Later catalog changes or
recomposition do not reinterpret existing partitions or their Python views. Failed or
incompatible runtime contributions leave warnings on an otherwise usable composite;
incoming-relation results affected by missing contributions are partial, not complete.

<!-- mcp:
title: "Python schema inspection API"
keywords: [Python, LayerSchema, FeatureTypeInfo, IdPart, MapFormat, Bitmask, SchemaReference, type_id, type_descriptor, rawValue]
-->
## Inspection API

| Object | Read-Only API |
| --- | --- |
| `MapFormat` | `name`, `version`, `subsystems`, `to_dict()`, tuple equality. `MapFormat(name="", version="", subsystems=())` constructs a descriptive value, not a schema. |
| `MapSchema` | `schema_id`, `format`, `layer_names`, `type_count`, `layer(layer_id)`, `type_reference(type_id)`, `type_descriptor(type_id)`, plus the binary/import operations above. |
| `LayerSchema` | `name`, `type`, `map_schema`, `root_schema`, `feature_types`, `produced_feature_types`, `get_type_info(name)`, `feature_schema(feature_type)`, `to_json_schema()`. |
| `FeatureTypeInfo` | `name`, `unique_id_compositions`: ordered tuples of owner-retaining `IdPart` views. The first composition is primary; subsequent compositions are reference alternatives. |
| `IdPart` | `name`, `description`, `datatype`, `is_optional`, `is_synthetic`. |
| `IdPartDataType` | `I32`, `U32`, `I64`, `U64`, `UUID128`, `STR`. |
| `SchemaReference` | `schema_id`, `type_id`, boolean validity. The reference retains its graph. Empty references have no interpretation. |
| `Bitmask` | `bits` as an exact Python integer, `type` as a `SchemaReference`, `to_dict()` using simfil's lossless tag. |

`MapFormat.subsystems` is a detached list of `{name, version}` pairs for direct content modules.
Pairs are sorted and deduplicated, but distinct versions of the same name are retained.
The constructor also accepts a legacy `{name: version}` dictionary as single-version input.
Empty format name/version strings mean unspecified metadata. Equal format tuples do not establish equal
projected schemas. `MapSchema.schema_id` is the exact cache/binding token, independent of the
datasource string dictionary. `type_count` counts graph definitions, including generated
query contexts but excluding the unknown sentinel; it is not a maximum valid type ID or a
count of feature classes. Numeric `type_id` values are local to that exact artifact, not
interchangeable with the string-valued `typeId` field of a Feature.
A nonempty version/subsystem map requires a format name; empty subsystem names are rejected.

`LayerSchema.feature_types` includes reference-only identity definitions; use
`produced_feature_types` to enumerate produced feature roots. These are tuples, not mutable
registries. Unknown `layer(...)` and `get_type_info(...)` names raise `KeyError`.
`feature_schema(...)` returns a local type ID, or zero when there is no root. `root_schema`
is likewise local to `map_schema.schema_id`. `to_json_schema()` is an explicit, potentially
large detached export for interoperability; routine discovery uses the native views instead.

`type_reference(type_id)` returns the exact owner-retaining reference for a known type.
`type_descriptor(type_id)` exports a detached dictionary through simfil's native `SchemaModel`,
not the JSON Schema exporter. Both methods reject unknown IDs, including zero, with `KeyError`.

```python
root_type_id = schema.layer("Road").root_schema
reference = schema.type_reference(root_type_id)
assert reference.schema_id == schema.schema_id
assert reference.type_id == root_type_id

descriptor = schema.type_descriptor(root_type_id)
assert descriptor["$ref"] == root_type_id
```

Descriptors expose `kind`, fields, alternatives, enum values and numeric/named-value metadata
where declared. `$ref` values refer to types in the same artifact. Recursive expansion and
native descriptor-budget limits are marked by `truncated`; request the referenced type
separately when needed. Expansion can be large, so inspect individual types rather than
exporting every root. Mutating the returned dictionary does not change the native graph.

In `namedValues`, `value` describes the model value; optional `rawValue` retains the original
integer encoding of a named enum, independently of its string-valued representation.
For example, a symbolic model value can retain its numeric decoder ordinal. Python preserves
integer descriptor values above JavaScript's exact-number range. The native descriptor uses
decimal strings for unsigned values above signed 64-bit range and hexadecimal strings for
flag masks; the binding preserves those representations. Binary schema roundtrips retain
the same metadata and type references.

`ModelNode.schema_reference` exposes explicit qualification. An ordinary leaf may instead
derive its meaning from its parent field/element; a detached unqualified node cannot recover
that context. Plain JSON export of qualified integers/floats/strings remains the underlying
primitive. Bitmasks retain full-width bits and exact type identity; node conversion returns a typed
`Bitmask`, not a rendered label. Copying a `Bitmask` through Python model-value input uses native
schema-aware storage. `Bitmask.to_dict()` returns `{_bitmask, bits, schema, typeId}`: `bits` is
a hexadecimal string, `schema` is the exact artifact identity and `typeId` is graph-local.
`Bitmask.type.type_id` and `Bitmask.type.schema_id` expose that identity without JSON conversion.
Null and unknown provenance are never assigned inferred labels.

Incoming manifests use version 2. `IncomingRelationCollection.status` is an
`IncomingRelationStatus` (`NO_POSSIBLE_FAMILY`, `COMPLETE`, `PARTIAL` or `UNSUPPORTED_SCOPE`),
and `scope` contains admitted provider partition keys, including providers returning no
matches. `complete` is scoped to those providers, never a claim about the whole map.
`from_json()` validates the version and status/completeness consistency.

MCP projections use an explicit qualified-scalar envelope; see
[the MCP value contract](mapget-mcp.md#results-and-budgets).

## Validation

Run `python3 test/integration/python-schema2-smoke.py` against the rebuilt mapget Python package.
It covers legacy normalization, canonical reconstruction, immutable owner lifetimes, exact
type references/counts, detached descriptors, named-value `rawValue`, shared/cyclic type
references, bitmask metadata, binary failures/roundtrip, source-qualified client retrieval
and partition schema retention.
It needs only loopback listeners, not an external datasource.
