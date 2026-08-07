# HTTP / WebSocket API Guide

Mapget protocol 3 exposes complete source tiles to trusted clients and
server-evaluated `TileSubsetLayer` values to interactive renderers. There is no
staged loading, backend feature LOD, `/search`, or
`TileSearchResultLayer`. Search, styling, selection, and relation
visualization all use `/filter`.

## Base URL and stream formats

JSON requests use `Content-Type: application/json`. `/tiles` and `/filter`
support:

- `application/binary`: the versioned `TileLayerStream` format.
- `application/jsonl`: one JSON value per line.
- a top-level `responseType` of `binary`, `jsonl`, or `json`; `json` is an
  alias for JSON Lines.

`Accept-Encoding: gzip` enables response compression. Binary clients may send
`stringPoolOffsets`, keyed by datasource `stringPoolId`, to suppress string
pool entries they already possess.

Protocol-3 `MapTileKey` values contain exactly four parts:

```text
<LayerType>:<percent-escaped mapId>:<percent-escaped layerId>:<signed tileId>
```

The removed stage suffix is not accepted.

## `GET /sources`

`/sources` returns the datasource catalog as a JSON array. Ready entries
contain `sourceId`, `stringPoolId`, `mapId`, layer metadata, feature ID
compositions, feature-model schemas, coverage, and supported zoom levels.
`stringPoolId` is the serialized string namespace; it is not datasource
routing identity.

Only one primary datasource may advertise a given map. Add-on datasources may
augment that primary source. Requests normally identify a map and layer;
`sourceId` is an optional assertion/routing override for clients which already
know the catalog entry.

By default the endpoint waits for the current datasource reload. Use
`?blocking=false` to receive initializing and failed catalog placeholders.
Catalog entries include:

- `status`: `initializing`, `ready`, or `failed`;
- `statusMessage` and optional `progress`;
- `configIndex` and configured datasource `type`.

Responses carry `X-Mapget-Sources-Revision` and an `ETag` of
`"sources-<revision>"`. `If-None-Match` may produce `304 Not Modified`.

## `POST /tiles`

`/tiles` streams complete feature/source-data tiles. Erdblick uses this path
only for explicit, short-lived inspection fetches; normal map rendering uses
`/filter`.

```json
{
  "responseType": "binary",
  "stringPoolOffsets": {"my-string-pool": 312},
  "requests": [
    {
      "mapId": "Tropico",
      "layerId": "WayLayer",
      "tileIds": [545554572],
      "priorityTileIds": [545554572]
    }
  ]
}
```

Each request has:

- `mapId`, `layerId`, and an ordered `tileIds` array;
- optional `sourceId`;
- optional `priorityTileIds`, which must be a subset of `tileIds` and affect
  scheduling only;
- optional `featureIds`: tile/id groups used by the inspection boundary.

The restricted form is:

```json
{
  "requests": [{
    "mapId": "Tropico",
    "layerId": "WayLayer",
    "tileIds": [545554572],
    "featureIds": [{
      "tileId": 545554572,
      "ids": ["Road.545554572.1001"]
    }]
  }]
}
```

At most 4096 distinct canonical feature IDs are accepted per envelope. The
service may load/cache the complete source tile internally, but only the named
features cross this response boundary.

`tileIdsByNextStage` and filter fields are rejected. Close the HTTP connection
to cancel an in-flight stream.

## `POST /filter`

`/filter` evaluates an ordered multi-channel definition over one or more
complete source-tile sets and streams immutable `TileSubsetLayer` outputs. A
REST definition lives on the envelope; source requests only select coverage:

```json
{
  "responseType": "jsonl",
  "channels": [
    {
      "channelId": "roads",
      "scope": "feature",
      "featureTypes": ["Road"],
      "featureFilter": "properties.frc <= 4",
      "entryFilter": "typeId == 'Road'",
      "featureFields": [
        "properties.name",
        "properties.category"
      ],
      "entryFields": [],
      "geometryTypes": 6,
      "geometryName": "centerline",
      "rewrite": false
    }
  ],
  "bindings": {
    "selected": false,
    "threshold": 12.5
  },
  "requests": [
    {
      "mapId": "Tropico",
      "layerId": "WayLayer",
      "tileIds": [545554572]
    }
  ]
}
```

REST identity defaults to an empty `filterId` and generation zero.
Interactive filters require both fields.

### Channel evaluation

Every channel has a unique `channelId` and a request scope:

- `feature`: evaluate terminal feature rows;
- `attribute`: expand each admitted feature into attribute/validity contexts;
- `relation`: traverse stored relations;
- `auto`: normalize `entryFilter` using `LayerSchema` and select feature or
  attribute scope before constructing the result.

The returned channel stores one concrete terminal scope: `feature`,
`attribute`, `relation`, or `group`.

Candidate evaluation is deliberately split:

1. `featureTypes`, geometry selectors, and `featureFilter` gate feature roots.
2. Scope expansion creates the terminal candidate.
3. `entryFilter` gates that feature, attribute-validity, or relation context.
4. `featureFields` are evaluated on the owning feature; `entryFields` are
   evaluated on the terminal context.

All filters and fields are schema-compiled in their actual context.
`rewrite: true` only enables `LayerSchema::normalizeSearchQuery()` for
`entryFilter`; `rewrite: false` does not disable ordinary schema compilation.
Relation channels require `rewrite: false`.

SIMFIL truthiness is used: zero results, `false`, `null`, and undefined are
false; every other successfully evaluated value is true. A projection with no
result becomes null. If an expression yields several values, the first is
used. Candidate-local evaluation failures reject that candidate and are
aggregated into structured channel issues instead of aborting the viewport.

`bindings` accepts null, boolean, signed integer, finite floating-point, and
string values. Bindings are available as SIMFIL constants and overlay fields.

### Attribute contexts

Attribute rows expose the attribute as their root and add:

- `$feature`: owning feature;
- `$layer` and `$name`: attribute layer/name;
- `$attributeIndex`;
- `$hasValidity`;
- `$validityIndex` and `$validityCount`.

The explicit validity bit distinguishes an attribute with no validity from the
first validity of an attribute which has one. Effective validity geometry is
copied into the returned `AttributeValidityEntry`.

### Geometry selectors

`geometryName` is either a concrete semantic name such as `centerline`, or
`"*"` for all applicable names. Omission also means wildcard. `geometryTypes`
is an independent bit mask.

Geometry names are model semantics. Mapget does not interpret names as
high/low fidelity and has no LOD gate. A presentation which wants fewer
features uses an ordinary attribute filter such as FRC/PRC.

### Point-grid grouping

The initial grouping operator is feature-only:

```json
{
  "channelId": "merged-points",
  "scope": "feature",
  "featureFilter": "typeId == 'Sign'",
  "geometryName": "position",
  "group": {
    "kind": "point-grid",
    "origin": [0, 0, 0],
    "cellSize": [1.5, 1.5, 1.5]
  },
  "entryFields": [
    "count($features.*)"
  ]
}
```

The service scans the ordered request plus its required contribution halo once
in source-major order. Each scanned tile submits point contributions to their
canonical output cells. An output is emitted atomically after its own source
and every required halo contribution are terminal.

The group context is rooted at the deterministic representative feature and
adds `$features`, a direct array of participating feature model pointers.
`GroupEntry` contains representative geometry, representative feature ID, all
member IDs, the stable cell key, and projected values. Attribute grouping and
multi-input grouping are not supported in protocol 3.0.

### Stored relations

A relation channel carries relation options:

```json
{
  "channelId": "topology",
  "scope": "relation",
  "featureFilter": "typeId == 'Intersection'",
  "entryFilter": "name == 'connects'",
  "geometryName": "*",
  "relation": {
    "namePattern": "connects.*",
    "recursive": true,
    "mergeTwoway": true
  },
  "featureFields": ["typeId"],
  "entryFields": ["name"]
}
```

`recursive: true` follows stored relations through local tiles and resolves at
most one hop across a tile border. Target tiles are fetched synchronously
inside the sparse relation-resolution job; their features become endpoint
entries in the origin output subset. They are not emitted as independent
output tiles unless requested.

`mergeTwoway` pairs reverse descriptors. Exact-root/selection traversal is
owned by the selected origin; if both endpoints are explicit roots, the first
root in request order owns the pair. Generic display uses permanent
south-west endpoint ownership. If the permanent owner tile is outside the
requested output set, the pair is omitted rather than temporarily reassigned,
so panning cannot display the same relation under changing owners.

Generic cross-layer or cross-level targets remain owned by the source output.
Exactly-once ownership is per filter definition, not global across independent
clients or presentations.

Relation contexts expose the stored relation as root and add `$source`,
`$target`, `$twoway`, and `$relationIndex`. The index is the descriptor's
stable ordinal in its source feature and lets a focused presentation select
one exact relation without treating a frontend pseudo-ID as a feature ID.

Exact roots are supplied on a source request:

```json
{
  "mapId": "Tropico",
  "layerId": "WayLayer",
  "tileIds": [545554572],
  "roots": [{
    "tileId": 545554572,
    "featureId": "Intersection.545554572.42"
  }]
}
```

An alternating key/value ID array plus `typeId` is also accepted.

### Result contract

Each output `TileSubsetLayer` inherits the source tile's ordinary `TileLayer`
metadata and `info()`, then carries `filterId` and `generation`. It contains:

- ordered channels and their concrete scope/field schemas;
- typed `FeatureEntry`, `AttributeValidityEntry`, `RelationEntry`, or
  `GroupEntry` arrays;
- source dependencies with `MapTileKey` and `sourceFeatureCount`;
- scalar `Filter/Entries/*#count` statistics and
  `Filter/Geometry/Vertices#count` for cheap pre-deserialization accounting;
- aggregated issues and SIMFIL traces;
- optional GLB attachment name.

Definition/root changes and forced refreshes advance the interactive
generation. Ordinary coverage amendments retain it so overlapping values are
not refetched. A frame is accepted only while its output tile remains in
current coverage; removing and later re-adding a tile may produce another
value with the same semantic `(filterId, generation, output MapTileKey)`.

## `GET /attachment`

Attachments use a separate data channel:

```http
GET /attachment?mapId=Tropico&layerId=Display&tileId=545554572&name=island.glb
```

`sourceId` is optional. A successful response uses the datasource-supplied
MIME type and an ETag. Clients should revalidate with `If-None-Match`; the
server returns `304` when unchanged. `404` means no such attachment, `403`
means unauthorized, and `503` means the request was aborted.

Mapget coalesces in-flight production. The Erdblick transport retains bytes
only while active `TileAttachmentRef` users exist; there is no unpinned warm
attachment cache in the initial implementation.

## `GET /interactive`

`/interactive` is a WebSocket control channel. `/interactive/payload` carries
the binary frames so a client can apply explicit backpressure.

An interactive message contains `requests`, `filterId`, `generation`,
`channels`, optional `bindings`, and optional `stringPoolOffsets`. The filter
definition may be on the envelope and is inherited by each request.
Sending another message replaces the active logical request on that
connection; processing preserves request tile order, while result arrival
order may differ.

Server control messages are binary VTLV frames:

- `RequestContext`: JSON with `requestId`, `clientId`, and catalog revision;
- `Status`: per-request state and final `allDone`;
- `SourceCatalogChange`: catalog revision/progress notifications.

`GET /tiles` remains a WebSocket alias for stale reverse-proxy deployments.
It does not change `POST /tiles` semantics.

## `GET|POST /interactive/payload`

Query parameters:

- `clientId`: required ID from `RequestContext`;
- `waitMs`: long-poll timeout, up to 30 seconds;
- `maxBytes`: pre-compression batch budget, capped at 64 MiB;
- `compress=1`: allow gzip when `Accept-Encoding` also permits it.

Responses are `200 application/octet-stream`, `204` on timeout, or `410` when
the session has gone away. `/tiles/next` remains a deployment-compatibility
alias.

## `POST /locate`

`/locate` resolves secondary or canonical IDs:

```json
{
  "requests": [
    {
      "mapId": "Tropico",
      "typeId": "Road",
      "featureId": ["roadLocationId", 64774998151332387]
    },
    {
      "mapId": "Tropico",
      "featureId": "Road.545554572.1001"
    }
  ]
}
```

The response contains a parallel `responses` array. Each result includes the
four-part `tileId` key and the resolved `canonicalFeatureId`. Canonical input
is resolved against the ID compositions
advertised by the layer before datasource dispatch. Datasources only plan
candidate tiles and portable selectors; mapget loads candidates through its
normal cache/coalescing path and returns concrete primary feature identities.

## `GET /location`

`/location?name=munich&limit=10` searches the configured place-name database.
Results contain `name`, WGS84 `lonLat`, and an `aabb`. The endpoint returns
`503` when no location database is available. Native deployments and the
Python wheel bundle the default GeoNames database beside their mapget binary;
`mapget serve --location-db` can select a different SQLite database.

## `GET /status` and `GET /status-data`

`/status` is a development HTML dashboard. `/status-data` is its JSON source
and contains service/cache metrics, datasource construction state, and
interactive queue/pull statistics. Heavy tile-size calculations can be
enabled with `includeTileSizeDistribution=true`.

The `memory` object reconciles process-level memory with explicitly owned
mapget state:

- `process` reports RSS/peak RSS and platform-specific process or cgroup
  controls where available;
- `mapget` breaks down retained metadata, catalog, scheduler, active-filter,
  and telemetry capacity;
- `active-filters` attributes source models, output models, relation targets,
  evaluation temporaries, and orchestration state to individual filter jobs;
- `datasources` contains each datasource's optional cooperative retained-state
  estimate;
- `cache` and `transport` account loaded string pools, serialized tile blobs,
  SQLite-owned state, and queued REST/interactive response buffers;
- `unattributed-resident-bytes` is the remaining RSS after known current
  ownership is subtracted.

Container values are capacity-based lower bounds rather than allocator-exact
measurements. The unattributed remainder intentionally includes allocator
fragmentation, thread stacks, shared libraries, opaque third-party internals,
and datasource implementations which do not provide an estimate.

## `/config`

<!-- --8<-- [start:config-endpoints] -->

`GET /config` returns:

- `schema`: the datasource configuration JSON Schema;
- `model`: the current editable datasource portion of YAML;
- `readOnly`;
- `datasourceConfigUnavailable` and its stable reason;
- read-only public sections registered by the embedding application.

Sensitive password/API-key fields are represented by stable masked tokens.
Unavailable reasons include `getConfigDisabled`, `configPathUnset`,
`configFileMissing`, `configFileOpenFailed`, `configParseFailed`, and
`configValidationFailed`.

`POST /config` is accepted only when the server starts with
`--allow-post-config`. The JSON body must satisfy the returned schema. Mapget
preserves real secrets when their masked tokens are posted, writes the
datasource-model portion, preserves unknown/public top-level YAML sections,
and reloads the catalog.

<!-- --8<-- [end:config-endpoints] -->
