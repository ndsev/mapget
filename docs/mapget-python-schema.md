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
finalized graph. Identity-only inputs have open property domains, not invented field semantics.
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

Feature, SourceData and subset partition objects expose `map_schema` and `layer_schema` for
their actual bound payload. These do not consult the latest catalog. Client decoding uses
already-installed schema owners, not unresolved dictionary references.

Native feature and source-data layers retain one exact graph. Add-ons can merge only when
they use that same schema identity; composing independent typed artifacts is not supported yet.
The service skips incompatible contributions before loading or merging them, keeps the primary
data, and records an actionable warning on the base tile. Incoming-relation results affected by
such a skipped contribution are partial, not complete. Domains are never stripped to force a merge.

<!-- mcp:
title: "Python schema inspection API"
keywords: [Python, LayerSchema, FeatureTypeInfo, IdPart, MapFormat, Bitmask, SchemaReference]
-->
## Inspection API

| Object | Read-Only API |
| --- | --- |
| `MapFormat` | `name`, `version`, `subsystems`, `to_dict()`, tuple equality. `MapFormat(name="", version="", subsystems={})` constructs a descriptive value, not a schema. |
| `MapSchema` | `schema_id`, `format`, `layer_names`, `domain_count`, `layer(layer_id)`, plus the binary/import operations above. |
| `LayerSchema` | `name`, `type`, `map_schema`, `root_schema`, `feature_types`, `produced_feature_types`, `get_type_info(name)`, `feature_schema(feature_type)`, `to_json_schema()`. |
| `FeatureTypeInfo` | `name`, `unique_id_compositions`: ordered tuples of owner-retaining `IdPart` views. The first composition is primary; subsequent compositions are reference alternatives. |
| `IdPart` | `name`, `description`, `datatype`, `is_optional`, `is_synthetic`. |
| `IdPartDataType` | `I32`, `U32`, `I64`, `U64`, `UUID128`, `STR`. |
| `SchemaReference` | `schema_id`, `domain`, boolean validity. The reference retains its graph. Empty references have no interpretation. |
| `Bitmask` | `bits` as an exact Python integer, `domain` as a `SchemaReference`, `to_dict()` using simfil's lossless tag. |

`MapFormat.subsystems` is a detached dictionary of direct content-module versions. Empty format
name/version strings mean unspecified metadata. Equal format tuples do not establish equal
projected schemas. `MapSchema.schema_id` is the exact cache/binding token, independent of the
datasource string dictionary. `domain_count` is a count, not a maximum valid domain ID.
A nonempty version/subsystem map requires a format name; empty subsystem names are rejected.

`LayerSchema.feature_types` includes reference-only identity definitions; use
`produced_feature_types` to enumerate produced feature roots. These are tuples, not mutable
registries. Unknown `layer(...)` and `get_type_info(...)` names raise `KeyError`.
`feature_schema(...)` returns a local domain ID, or zero when there is no root. `root_schema`
is likewise local to `map_schema.schema_id`. `to_json_schema()` is an explicit, potentially
large detached export for interoperability; routine discovery uses the native views instead.

`ModelNode.schema_reference` exposes explicit qualification. An ordinary leaf may instead
derive its meaning from its parent field/element; a detached unqualified node cannot recover
that context. Plain JSON export of qualified integers/floats/strings remains the underlying
primitive. Bitmasks retain full-width bits and domain identity; node conversion returns a typed
`Bitmask`, not a rendered label. Copying a `Bitmask` through Python model-value input uses native
domain-aware storage. Null and unknown provenance are never assigned inferred labels.

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
binary failures/roundtrip, source-qualified client retrieval and partition schema retention.
It needs only loopback listeners, not an external datasource.
