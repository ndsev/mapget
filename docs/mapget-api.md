# HTTP / WebSocket API Guide

Mapget protocol 4 exposes complete source tiles to trusted clients and
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
multi-input grouping are not supported in protocol 3.1.

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

Each output `TileSubsetLayer` carries `filterId` and `generation`. Its lifetime
is the original timestamp/positive TTL pair of the contributing local, halo,
or relation-source tile with the earliest absolute expiry. If none has a
positive TTL, the subset does not expire. It also inherits source `info()` and
contains:

- ordered channels and their concrete scope/field schemas;
- typed `FeatureEntry`, `AttributeValidityEntry`, `RelationEntry`, or
  `GroupEntry` arrays;
- source dependencies with `MapTileKey` and `sourceFeatureCount`;
- scalar `Filter/Entries/*#count` statistics and
  `Filter/Geometry/Vertices#count` for cheap pre-deserialization accounting;
- aggregated issues and SIMFIL traces;
- optional GLB attachment name.

Definition, binding, or exact-root changes advance the interactive generation.
Ordinary pending-set amendments and TTL refreshes retain it. A filtered output
is identified by `(filterId, generation, output MapTileKey)`; mapget rejects a
same-generation definition/root mutation while any overlapping output remains
active, queued, or handed off.

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

An interactive message contains `requests`, optional envelope-level
`filterId`, `generation`, `channels`, optional `bindings`, and optional
`stringPoolOffsets`. The filter definition may be on the envelope and is
inherited by each request:

```json
{
  "filterId": "view-7",
  "generation": 4,
  "channels": [{
    "channelId": "roads",
    "scope": "feature",
    "entryFilter": "typeId == 'Road'"
  }],
  "requests": [{
    "mapId": "Tropico",
    "layerId": "Display",
    "tileIds": [545554572, 545554573]
  }]
}
```

Every completed envelope replaces the connection's complete **pending-output
snapshot**. `tileIds` lists only outputs the client is still waiting to accept;
it is not the client's retained viewport inventory. Repeating a key is
idempotent while backend work is active, its frame is queued, or an unexpired
handoff record represents bytes already placed in a payload response.

Mapget preserves overlapping active work, prunes omitted outputs, and starts
only additions. Indexed `chunk` messages are staged and reconciled atomically
after the final chunk. Omitting a handed-off key releases its bookkeeping; if
the handed-off value's own timestamp plus positive TTL has expired, repeating
the key follows the ordinary cache/refresh path without an omit/re-add cycle.
Missing or zero TTL has no session-side expiry. The removed `renewals`,
`deliveryEpoch`, and `deliveryEpochs` fields are rejected by protocol 4.

Completed snapshots are consumed through a latest-wins mailbox outside the
WebSocket I/O thread. If several complete snapshots arrive faster than mapget
can apply them, an unapplied intermediate snapshot may be superseded without a
`RequestContext` or status response. The newest snapshot is always retained,
and running request expansion abandons a superseded candidate before its
atomic commit. Clients must therefore treat `requestId` as an acknowledgement
of applied state rather than expecting one response for every submitted
update.

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
Portable selectors use exactly one of `canonicalFeatureId`, `typeId` plus
`featureFilter`, or `typeId` plus `featureIdExpression`. The latter evaluates
once per loaded candidate tile with `$features` and optional scalar `bindings`,
then resolves the returned canonical IDs through the tile's primary-ID index.
It is intended for secondary identities that would otherwise require a nested
full-tile expression for every candidate feature.

## `GET /location`

`/location?name=munich&limit=10` searches the configured place-name database.
Results contain `name`, WGS84 `lonLat`, and an `aabb`. The endpoint returns
`503` when no location database is available. Native deployments and the
Python wheel bundle the default GeoNames database beside their mapget binary;
`mapget serve --location-db` can select a different SQLite database.

## `GET /status`, `GET /status-data`, and `POST /status-data/cache-report`

`/status` is the operational HTML dashboard. Its live tabs poll
`GET /status-data`, which contains service/cache metrics, datasource
construction state, memory ownership, and interactive queue/pull statistics.
The live endpoint deliberately never parses cached feature tiles.

`POST /status-data/cache-report` explicitly generates one point-in-time cache
report. It returns `featureTree` storage measurements and a
`tileSizeDistribution`, together with `generatedAtMs`, `durationMs`, and the
cache counters captured for that report. Cache reports are serialized,
concurrent callers share an active run, and the blocking traversal executes
outside Drogon's HTTP event loop. The dashboard retains the result in the
browser until the customer regenerates or reloads it; automatic refreshes do
not repeat the analysis.

The `memory` object presents process residency, allocator state, and
explicitly owned mapget state as distinct measurement domains:

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
- `allocator-trim` reports whether periodic glibc heap trimming is supported
  and enabled, its period, attempt/success counters, and the most recent
  duration and free-arena samples;
- `reconciliation` contains diagnostic differences between allocator-live
  bytes, anonymous RSS, file/shared RSS, and known ownership estimates.

Container values are capacity-based lower bounds rather than allocator-exact
measurements. They must not be added directly to RSS rows. The reconciliation
residuals can indicate allocator fragmentation, thread stacks, opaque mappings,
or missing ownership instrumentation, but they do not identify leaks by
themselves.

## `/cache/reset`

`POST /cache/reset` clears every cached Feature and SourceData tile for one
exact map ID. The JSON request body is:

```json
{"mapId": "Example/Map"}
```

The endpoint is disabled by default. It is available only when mapget starts
with `--allow-cache-reset` and at least one
`--cache-reset-auth-header HEADER=REGEX` gate. A request must satisfy that global gate and the target
map's ordinary datasource `auth-header` gate. Header names are matched without
case sensitivity, values use full regular-expression matching, and multiple
configured alternatives are ORed.

A successful reset returns `204 No Content` after the cache and scheduler
boundary has been crossed. Invalid input returns `400`, a disabled or failed
global gate returns `403`, an unknown or caller-inaccessible ready primary map
returns `404`, and an invalidation failure returns `500`. Feature checks happen
before map lookup so an unauthorized caller cannot use the endpoint to probe
configured map IDs.

The operation is local to this mapget process. It does not clear datasource-
internal caches or notify other browser sessions.

## `/config`

<!-- --8<-- [start:config-endpoints] -->

`GET /config` returns:

- `schema`: the datasource configuration JSON Schema;
- `model`: the current editable datasource portion of YAML;
- `readOnly`;
- `datasourceConfigUnavailable` and its stable reason;
- caller-specific server capabilities such as `capabilities.cacheReset`;
- read-only public sections registered by the embedding application.

Sensitive password/API-key fields are represented by stable masked tokens.
Unavailable reasons include `getConfigDisabled`, `configPathUnset`,
`configFileMissing`, `configFileOpenFailed`, `configParseFailed`, and
`configValidationFailed`.

Configuration writes are accepted only when the server starts with
`--allow-post-config`:

- `POST /config` accepts a complete datasource model that satisfies the
  returned schema. Mapget preserves real secrets represented by masked tokens,
  writes the datasource-model portion, preserves unknown/public top-level YAML
  sections, and reloads the datasource catalog.
- `PATCH /config` accepts exactly `{"path":"/...","value":...}` plus an
  `If-Match` header containing the current full-file revision. The path is an
  opaque identifier and must exactly match a field writer registered by the
  embedding application; Mapget does not provide arbitrary JSON/YAML patching.
  A successful response returns the same path, the writer's canonical value,
  the new revision, and a matching `ETag`. Public-field writes use the same
  atomic whole-document replacement and revision safeguards without reloading
  datasources.

Because capabilities can vary with request headers, `GET /config` responses
include `Cache-Control: private, no-store`.

<!-- --8<-- [end:config-endpoints] -->
