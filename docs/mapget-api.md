# HTTP / WebSocket API Guide

Mapget exposes a small HTTP + WebSocket API that lets clients discover datasources, stream tiles, locate features by ID and inspect or update the running configuration. Interactive tile streaming uses the `/interactive` WebSocket control channel plus `/interactive/payload` pull requests for the binary tile data. This guide describes the endpoints and their request and response formats.

## Base URL and formats

The server started by `mapget serve` listens on the configured host and port (by default on all interfaces and an automatically chosen port). All endpoints are rooted at that host and port.

Requests that send JSON use `Content-Type: application/json`. HTTP tile/search streaming supports two response encodings, selected via the `Accept` header or a top-level `responseType` override:

- `Accept: application/jsonl` returns a JSON‑Lines stream where each line is one JSON object.
- `Accept: application/binary` returns a compact binary stream optimized for high-volume traffic.

The binary format and the logical feature model are described in more detail in `mapget-model.md`.

## `/sources` – list datasources

`GET /sources` returns a backward-compatible JSON array describing the datasource catalog.

- **Method:** `GET`
- **Query parameters:** optional `blocking=false` returns immediately while a datasource reload is still in progress.
- **Request body:** none
- **Response:** `application/json` array of datasource descriptors

By default, `/sources` preserves legacy behavior and waits until the current config-backed datasource reload has completed before returning. Clients that want live startup indicators should call `/sources?blocking=false`; that form returns immediately and may include initializing entries. Ready datasources are serialized as normal `DataSourceInfo` objects plus catalog metadata. Initializing or failed config entries are serialized as placeholder `DataSourceInfo`-compatible objects with empty `layers`, `maxParallelJobs: 0`, and catalog metadata. This keeps old array-shape clients working while allowing frontends to show per-datasource startup state.

Each item includes:

- `status`: `initializing`, `ready` or `failed`.
- `statusMessage`: human-readable progress or failure text.
- `progress`: optional datasource-constructor progress percentage in the range `0..100`.
- `sourceId`: stable source identity from config `id`/`sourceId`, or generated as `config:<index>`.
- `configIndex`: order in `mapviewer.yaml`.
- `type`: datasource type from config.
- `configuredMapId` when known before construction.

Ready entries also contain map ID, available layers and basic metadata. Each layer entry includes its type, `zoomLevels`, `coverage`, staged-loading metadata (`stages`, optional `stageLabels`, `highFidelityStage`) and feature-type information. This endpoint is typically used by frontends to discover which maps and layers can be requested via `POST /tiles` or `/interactive`.

Response headers:

- `X-Mapget-Sources-Revision`: monotonic datasource-catalog revision.
- `ETag`: `"sources-<revision>"`; clients may send `If-None-Match` and receive `304 Not Modified`.
- `X-Mapget-Sources-Config-Status`: `ok` or `error`.
- `X-Mapget-Sources-Config-Message`: config parse/validation error text when present.

## `/location` – location lookup

`GET /location` searches the configured location database for place-name matches. By default, mapget builds can generate a small SQLite database from GeoNames `cities5000` data at build time and place it next to the runtime binary as `geonames-cities5000.sqlite`.

- **Method:** `GET`
- **Query parameters:**
  - `name` (required): place-name fragment. Empty or too-short names return an empty array.
  - `limit` (optional): maximum number of returned matches. Defaults to `10` and is clamped by the server-side `--location-max-limit` setting.
- **Response:** `application/json` array of location objects.
- **Unavailable database:** `503 application/json` with `{"error":"location database unavailable"}`.

Example:

```http
GET /location?name=munich&limit=10
```

```json
[
  {
    "id": "geonames:2867714",
    "name": "Munich, DE",
    "lonLat": [11.57549, 48.13743],
    "aabb": [[11.57549, 48.13743], [0, 0]],
    "source": "geonames-cities5000",
    "countryCode": "DE",
    "population": 1260391
  }
]
```

`lonLat` is `[longitude, latitude]` in WGS84 and is the authoritative jump coordinate. `aabb` is encoded as `[[west, south], [extentLon, extentLat]]`; GeoNames `cities5000` contains point coordinates only, so the bundled database returns zero-extent boxes.

The bundled GeoNames data is licensed under Creative Commons Attribution 4.0 and is provided without warranty. Keep `geonames-readme.txt` with redistributed runtime artifacts.

## `/tiles` – stream tiles (HTTP)

`POST /tiles` streams tiles for one or more map–layer combinations.

- **Method:** `POST`
- **Request body (JSON):**
  - `requests`: array of objects, each with:
    - `mapId`: string, ID of the map to query.
    - `layerId`: string, ID of the layer within that map.
    - either `tileIds`: array of numeric tile IDs in mapget’s tiling scheme. This is an **unstaged** request shape: the service does not expand it into one backend fetch per advertised stage and returns one tile response per requested tile with no explicit stage affinity.
    - or `tileIdsByNextStage`: array of arrays where bucket `i` lists tiles whose next missing stage is `i`. This is the **staged** request shape: the service expands each tile to stage `i` and all higher stages advertised by the layer.
    - `priorityTileIds` (optional): array of numeric tile IDs from the same request that should be scheduled before regular tile IDs. This is only a scheduling hint; it does not request additional tiles and does not change staged vs. unstaged semantics.
  - `responseType` (optional): `binary`, `jsonl` or `json`. This overrides `Accept`; `json` is an alias for JSON Lines.
  - `stringPoolOffsets` (optional): dictionary from datasource node ID to last known string ID. Used by advanced clients to avoid receiving the same field names repeatedly in the binary stream.
- **Response:**
  - `application/jsonl` if `Accept: application/jsonl` is sent.
  - `application/binary` if `Accept: application/binary` is sent, using the tile stream protocol.

Tiles are streamed as they become available. In JSONL mode, each line is the JSON representation of one tile layer. In binary mode, the response is a sequence of versioned messages that can be decoded using the tile stream protocol from `mapget-model.md`.

For staged feature-layer clients, `tileIdsByNextStage` must be used even when only bucket `0` is non-empty. Collapsing such a request to plain `tileIds` changes its semantics to an unstaged request.

`priorityTileIds` is intended for interactive clients that need a few foreground tiles to finish before broad background loading. For example, a map viewer can prioritize the selected feature's tile so its high-stage inspection data arrives before unrelated viewport tiles. The server may still finish already-running jobs first.

Example staged request with one foreground tile:

```json
{
  "requests": [
    {
      "mapId": "Tropico",
      "layerId": "WayLayer",
      "tileIdsByNextStage": [
        [1234, 5678],
        [9112]
      ],
      "priorityTileIds": [9112]
    }
  ]
}
```

If `Accept-Encoding: gzip` is set, the server compresses responses where possible, which is especially useful for JSONL streams.

To cancel an in-flight HTTP stream, close the HTTP connection.

## `/search` – server-side search-as-map (HTTP)

`POST /search` runs a one-shot server-side SIMFIL search over feature tiles and streams `TileSearchResultLayer` chunks back to the client. `POST /tiles` is tile-only; REST clients must use `/search` for search.

- **Method:** `POST`
- **Request body (JSON):**
  - `query`: string SIMFIL predicate evaluated on each candidate context.
  - `scope` (optional): `"feature"` (default) evaluates once per feature. `"attribute"` evaluates once per attribute validity context. `"auto"` asks mapget to normalize the query through the layer schema and choose feature or attribute scope.
  - `rewrite` (optional): boolean. When `true`, mapget normalizes the query before evaluation. `scope: "auto"` implies `rewrite: true`.
  - `withFields` (optional): array of SIMFIL expressions evaluated for every match. Values are stored in `SearchResult.values` in the same order.
  - `featureTypes` (optional): array of feature type names. When present, mapget evaluates only matching feature roots and their attributes.
  - `responseType` (optional): `"binary"`, `"jsonl"` or `"json"`. This overrides `Accept`; `"json"` is an alias for JSON Lines.
  - `requests`: array of source-layer requests with `mapId`, `layerId`, `tileIds`, and optional `priorityTileIds`.
  - `stringPoolOffsets` (optional): same binary stream optimization as `/tiles`.
- **Response:**
  - `application/jsonl` if `Accept: application/jsonl` or `responseType: "jsonl"` is used.
  - `application/binary` if `Accept: application/binary`, `responseType: "binary"`, or no response type is specified.

Search requests must use `tileIds`, not `tileIdsByNextStage`. The server always loads all advertised stages for every searched tile before evaluating the SIMFIL predicate, so client-side "next missing stage" buckets are not meaningful for search.

Example REST search request:

```json
{
  "query": "typeId == 'Road'",
  "scope": "feature",
  "withFields": ["name", "typeId", "speedLimitKmh"],
  "featureTypes": ["Road"],
  "responseType": "jsonl",
  "requests": [
    {
      "mapId": "Tropico",
      "layerId": "WayLayer",
      "tileIds": [1234, 5678]
    }
  ]
}
```

The same request with `curl`:

```bash
curl -N \
  -H "content-type: application/json" \
  -H "accept: application/jsonl" \
  -d '{
    "query": "typeId == '\''Road'\''",
    "scope": "feature",
    "withFields": ["typeId"],
    "requests": [
      {"mapId": "Tropico", "layerId": "WayLayer", "tileIds": [1234, 5678]}
    ]
  }' \
  http://127.0.0.1:8080/search
```

For `scope: "feature"`, the SIMFIL context is the feature itself. For `scope: "attribute"`, the context is an attribute object with a few overlay fields:

| Field | Meaning |
|-------|---------|
| `$name` | Attribute name. |
| `$feature` | Owning feature object. |
| `$layer` | Attribute-layer name. |
| `$validityIndex` | Zero-based validity index being evaluated. |
| `$validityCount` | Number of validity contexts for the matched attribute. |

When rewrite is enabled, mapget uses `LayerSchema::normalizeSearchQuery` before tile evaluation. The normalizer compiles the query with SIMFIL schema rewrites, inspects AST-derived `referencedSchemaPaths`, and emits guarded attribute-root predicates when the query is proven to target attribute data. It performs these normalization steps:

- Exact attribute type-code/name queries such as `WARNING_SIGN` become `$feature.typeId`/`$layer`/`$name` guards.
- Feature-root attribute paths such as `properties.layer.rules.speedLimit.limit > 40` are rewritten to the attribute-root suffix, for example `limit > 40`, under the same guards.
- Enum constants rewritten by SIMFIL to schema equality paths become guarded attribute-root comparisons, for example `attributeValue.warningSign == "SPEED_LIMIT"`.
- Recursive wildcard expressions such as `**.speedLimitKmh` remain in SIMFIL's schema compile path so wildcard pruning still happens against the concrete root schema.
- References inside feature-level aggregate calls such as `count(**.speedLimitKmh) == 0` do not drive attribute-scope inference, because the aggregate result belongs to the feature query context.
- If an inferred attribute rewrite would fan out to more than eight candidate attribute contexts, mapget keeps attribute scope but suppresses the guarded rewrite. Schema-generated enum predicates are still compacted to generic attribute-root predicates, for example `conditions.*.conditionTypeCode == "DAYS_OF_WEEK"`, while ordinary wildcard paths stay in SIMFIL's schema-aware evaluation path.

`withFields` expressions run in the same context as `query`. They are intended for labels, style keys and compact metadata. Scalar values are preserved; structured values are stringified in the result layer. Attribute-scope results use the computed validity geometry for the matched validity when one is available; otherwise they fall back to the feature display geometry.

Example attribute-scope request:

```json
{
  "query": "$name == 'SPEED_LIMIT' and valueKph > 50",
  "scope": "attribute",
  "withFields": [
    "$name",
    "$layer",
    "$feature.typeId",
    "$validityIndex",
    "$validityCount",
    "valueKph",
    "trace(valueKph, name=\"speed limits\")"
  ],
  "responseType": "jsonl",
  "requests": [
    {
      "mapId": "Tropico",
      "layerId": "WayLayer",
      "tileIds": [1234, 5678],
      "priorityTileIds": [1234]
    }
  ]
}
```

Use `withFields` for values that clients need for labels, grouping, color categories, or compact export metadata. Use `trace()` when you want the result stream to carry aggregate diagnostics about values observed while evaluating the query.

### Interactive WebSocket Search

WebSocket search remains part of the `/interactive` control channel because it supports replacing an ongoing logical search. The WebSocket request shape uses the same `query`, optional `scope`, optional `withFields`, optional `rewrite`, and optional `featureTypes` search spec as `/search`, plus interactive-only `searchId` and optional `refresh`. Reusing the same `searchId` updates the ongoing query in the client session. `searchQuery` and `searchScope` are accepted as legacy aliases for older interactive clients.

Example control-channel message:

```json
{
  "requests": [
    {
      "mapId": "Tropico",
      "layerId": "WayLayer",
      "tileIds": [1234, 5678],
      "priorityTileIds": [1234]
    }
  ],
  "searchId": "speed-limit-search",
  "query": "$name == 'SPEED_LIMIT' and valueKph > 50",
  "scope": "attribute",
  "withFields": ["$name", "$feature.typeId", "valueKph"],
  "featureTypes": ["Road"],
  "refresh": true
}
```

Clients should treat `searchId` as the routing key for one interactive search session. Sending another message with the same `searchId` replaces the previous logical search on that connection; stale queued frames for the older request may be dropped before delivery. Status frames and `TileSearchResultLayer` frames carry search metadata so clients can ignore frames that no longer match their active request key.

### Search result JSONL shape

In JSONL mode, search result chunks are emitted as `SearchResultCollection` objects. Search progress status objects may appear between result chunks.

```json
{
  "type": "SearchResultCollection",
  "mapgetTileId": 1234,
  "mapId": "Tropico",
  "mapgetLayerId": "WayLayer",
  "resultFields": ["limit", "$feature.typeId", "$layer"],
  "diagnostics": [
    {
      "message": "No matches for field: someField",
      "location": {"offset": 0, "size": 9}
    }
  ],
  "info": {
    "searchScope": "attribute",
    "resultCount": 1,
    "sourceNodeId": "WayDataSource",
    "sourceMapId": "Tropico",
    "sourceLayerId": "WayLayer",
    "sourceTileId": 1234
  },
  "results": [
    {
      "type": "SearchResult",
      "featureId": "Road.42",
      "geometry": {"type": "GeometryCollection", "geometries": []},
      "values": [80, "Road", "details"],
      "attributeIndex": 0,
      "match": {
        "attributeIndex": 0,
        "validityIndex": 0,
        "validityCount": 1
      }
    }
  ]
}
```

Interactive WebSocket search result layers additionally carry `info.searchId`, `info.searchRequestKey`, and optional `info.refresh` so clients can route replacement-search updates.

Important result fields:

| Field | Description |
|-------|-------------|
| `resultFields` | Copy of the requested `withFields` expressions. Indices align with every result's `values` array. |
| `results[].featureId` | Dot-separated feature ID string for the matched source feature. |
| `diagnostics` | Optional parsed SIMFIL diagnostics from `query` evaluation, serialized with the result layer and exported in JSONL mode. |
| `traces` | Optional typed SIMFIL `trace()` aggregates collected while evaluating the search and field expressions. |
| `results[].geometry` | Copied display geometry used for map styling/highlighting. For attribute-scope validity matches, this is the computed validity geometry when available. |
| `results[].values` | Evaluated `withFields` values in order. Binary/object/list values are represented as `blob`/`object`/`list` placeholder strings. |
| `results[].match` | Present for attribute-scope matches and identifies the matched attribute/validity context. |
| `info.sourceStageMask` | Present when staged source payloads were assembled before search evaluation. |

### Search status objects

Search status frames/JSONL lines have `type: "mapget.search.status"` and describe backend progress:

```json
{
  "type": "mapget.search.status",
  "state": "TileSearched",
  "tilesQueued": 8,
  "tilesLoaded": 4,
  "tilesSearched": 3,
  "matches": 12,
  "chunksEmitted": 3
}
```

Observed `state` values include `Open`, `TileLoaded`, `TileSearched`, `Success`, `Aborted` and `Failed`. Failed statuses also include an `error` string. WebSocket status objects also include `searchId`, `requestKey`, and optional `refresh`.

## `/interactive` – interactive control channel (WebSocket)

`GET /interactive` supports WebSocket upgrades. This endpoint is the control channel for interactive clients. It carries request updates and lightweight status/control frames; binary tile data is pulled separately via `/interactive/payload`. `GET /tiles` is accepted as a legacy WebSocket alias for deployments with older reverse-proxy rules; `POST /tiles` remains the stateless REST tile endpoint.

- **Connect:** `ws://<host>:<port>/interactive`
  - Legacy alias: `ws://<host>:<port>/tiles`
- **Client → Server:** send one *text* message containing tile `requests`, optional `stringPoolOffsets`, and optional interactive search fields (`searchId`, `query`, `scope`, `withFields`, `rewrite`, `featureTypes`, `refresh`; `searchQuery`/`searchScope` remain legacy aliases).
  - `stringPoolOffsets` is optional; the server remembers the latest offsets per WebSocket connection. Clients may re-send it to reset/resync offsets.
- **Server → Client:** sends *binary* WebSocket messages carrying VTLV control frames.
  - `RequestContext` frames contain a UTF-8 JSON payload with `requestId`, `clientId` and `sourcesRevision`. The `clientId` is then used for `/interactive/payload`.
  - `Status` frames contain UTF-8 JSON describing per-request `RequestStatus` transitions, search progress updates, and human-readable messages. The final regular tile status frame has `"allDone": true`.
  - `SourceCatalogChange` frames contain `{"type":"mapget.sources.changed","revision":<number>,"reason":<string>}`. Status-message and progress changes also include a `source` object with `sourceId`, `configIndex`, `type`, `status`, `statusMessage`, `progress`, `addOn` and optional `configuredMapId`, allowing clients to update loading UI without refetching `/sources`. Generic reload/add/remove changes omit `source` and tell clients to refetch `/sources`.
  - `LoadStateChange` exists in the protocol but is currently not emitted by the HTTP service.

For search requests, `/interactive/payload` returns normal stream frames plus `TileSearchResultLayer` frames. Clients should decode the binary message type and handle search-result layers separately from source `TileFeatureLayer` / `TileSourceDataLayer` frames.

Interactive sessions use bounded outgoing queues. If a client stops polling `/interactive/payload` while replacing searches quickly, the service favours current request frames and may discard stale search-result frames for superseded requests. This keeps interactive search responsive instead of forcing the client to drain obsolete result layers.

Each entry in a status frame's `requests` array contains `index`, `mapId`, `layerId`, numeric `status`, and `statusText`. For `NoDataSource` statuses, servers may also include `noDataSourceReason`:

- `emptySources`
- `allSourcesDisabled`
- `datasourceInitializationFailed`
- `missingMapOrLayer`
- `noConfig`

To cancel, either send a new request message on the same connection (which replaces the current one) or close the WebSocket connection.

## `/interactive/payload` – pull binary tile frames

`GET /interactive/payload` (also accepts `POST`) returns the next available binary tile frame batch for an active `/interactive` session, including sessions opened through the legacy `/tiles` WebSocket alias.

- **Method:** `GET` or `POST`
- **Query parameters:**
  - `clientId` (required): numeric client id received via the websocket `RequestContext` frame.
  - `waitMs` (optional): long-poll timeout in milliseconds. Defaults to 25000 and is clamped to 30000.
  - `maxBytes` (optional): batch size budget. If greater than zero, the response may concatenate multiple VTLV frames up to that byte budget (capped at 64 MiB; the budget is counted before optional gzip compression).
  - `compress` (optional): set to `1` to enable gzip compression when the client also sends `Accept-Encoding: gzip`.
- **Response:**
  - `200 application/octet-stream` with one or more concatenated `TileLayerStream` VTLV frames.
  - `204 No Content` if the long-poll timed out before any frame became available.
  - `410 Gone` if the interactive session no longer exists.

### Why JSONL instead of JSON?

JSON Lines is better suited to streaming large responses than a single JSON array. Clients can start processing the first tiles immediately, do not need to buffer the complete response in memory, and can naturally consume the stream with incremental parsers.

### JSONL response format

Each line in the JSONL response is a GeoJSON-like FeatureCollection with additional metadata:

```json
{
  "type": "FeatureCollection",
  "mapgetTileId": 281479271743500,
  "mapId": "EuropeHD",
  "mapgetLayerId": "Roads",
  "timestamp": 1736850600000000,
  "ttl": 3600000,
  "error": {
    "code": 404,
    "message": "Error while contacting remote data source: not found"
  },
  "features": [...]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Always `"FeatureCollection"` |
| `mapgetTileId` | integer | The mapget tile ID (64-bit decimal) |
| `mapId` | string | Map identifier |
| `mapgetLayerId` | string | Layer identifier within the map |
| `timestamp` | integer | Tile creation time in microseconds since the Unix epoch |
| `ttl` | integer | Time-to-live in milliseconds (optional) |
| `error` | object | Error information if tile creation failed (optional) |
| `error.code` | integer | Numeric error code, e.g., HTTP status or database error (optional) |
| `error.message` | string | Human-readable error message (optional) |
| `features` | array | Array of GeoJSON Feature objects |

The `error` object is only present if an error occurred while filling the tile. When present, the `features` array may be empty or contain partial data.

### Curl Call Example

For example, the following curl call could be used to stream GeoJSON feature objects
from the `MyMap` data source defined previously:

```bash
# Standard request (uncompressed response)
curl -X POST \
    -H "Content-Type: application/json" \
    -H "Accept: application/jsonl" \
    -H "Connection: close" \
    -d '{
    "requests": [
       {
           "mapId": "Tropico",
           "layerId": "WayLayer",
           "tileIds": [1, 2, 3]
       }
    ]
}' "http://localhost:8080/tiles"

# Request with gzip compression (reduces bandwidth by ~70-95%)
curl -X POST \
    -H "Content-Type: application/json" \
    -H "Accept: application/jsonl" \
    -H "Accept-Encoding: gzip" \
    -H "Connection: close" \
    --compressed \
    -d '{
    "requests": [
       {
           "mapId": "Tropico",
           "layerId": "WayLayer",
           "tileIds": [1, 2, 3]
       }
    ]
}' "http://localhost:8080/tiles"
```

Note: The `--compressed` flag tells curl to automatically decompress the gzip response for display.

### C++ Call Example

If we use `"Accept: application/binary"` instead, we get a binary stream of
tile data which we can also parse in C++, Python or JS. Here is an example in C++, using
the `mapget::HttpClient` class:

```C++
#include "mapget/http-service/http-client.h"
#include <iostream>

using namespace mapget;

void main(int argc, char const *argv[])
{
     // Create client with gzip compression enabled (default)
     HttpClient client("localhost", service.port());
     // Or disable compression: HttpClient client("localhost", service.port(), {}, false);

     auto tileRequest = std::make_shared<LayerTilesRequest>(
         "Tropico",
         "WayLayer",
         std::vector<TileId>{{1234, 5678, 9112, 1234}});
     auto receivedTileCount = 0;
     tileRequest->onFeatureLayer([&](auto&& tile) { receivedTileCount++; });
     client.request(tileRequest)->wait();

     std::cout << receivedTileCount << std::endl;

     FeatureLayerSearchRequest search;
     search.query_ = "typeId == 'Road'";
     search.withFields_ = {"name", "typeId"};
     auto searchRequest = std::make_shared<FeatureLayerSearchTilesRequest>(
         "Tropico",
         "WayLayer",
         std::vector<TileId>{{1234, 5678}},
         std::move(search));
     searchRequest->onSearchResult([](TileSearchResultLayer::Ptr layer) {
         std::cout << "matches in tile: " << layer->size() << std::endl;
     });
     client.search(searchRequest)->wait();

     service.stop();
}
```

Keep in mind, that you can also run a `mapget` service without any RPCs in your application. Check out [`examples/cpp/local-datasource`](../examples/cpp/local-datasource/main.cpp) on how to do that.

## `/status` – service and cache statistics

`GET /status` returns a simple HTML page with diagnostic information.

- **Method:** `GET`
- **Request body:** none
- **Response:** `text/html`

The page shows the number of active datasources and worker threads, cache statistics, websocket/pull metrics, and optional tile-size-distribution data. It refreshes by polling `/status-data`. This endpoint is primarily used during development and debugging.

## `/status-data` – machine-readable diagnostics

`GET /status-data` returns the JSON payload that powers `/status`.

- **Method:** `GET`
- **Query parameters:**
  - `includeTileSizeDistribution` (optional, default `false`): include the heavy cached-tile size histogram / distribution calculations.
  - `includeCachedFeatureTreeBytes` (optional, default `true`): include cached feature-tree byte breakdowns.
- **Response:** `application/json`

The response contains:

- `timestampMs`
- `service`: service statistics, datasource info, cache occupancy, datasource-config counts, and optional tile-size-distribution data
- `cache`: cache hit/miss counters and cache sizes
- `tilesWebsocket`: control-channel metrics such as active sessions, pending queued frames for `/interactive/payload`, blocked pull requests, and total forwarded bytes / frames

`service.datasource-config` reports datasource YAML load diagnostics:

- `configured`: number of entries under `sources`.
- `enabled`: number of entries not disabled by `enabled: false`.
- `disabled`: number of entries skipped because `enabled: false`.
- `construction-failed`: number of enabled entries whose datasource construction failed.

## `/locate` – resolve external feature IDs

`POST /locate` resolves external feature references to the tile IDs and feature IDs that contain them. This is commonly used together with feature search results or external databases that store map references.

- **Method:** `POST`
- **Request body (JSON):**
  - `requests`: array of objects, each with:
    - `mapId`: ID of the map to search in.
    - `typeId`: feature type identifier.
    - `featureId`: array of ID parts forming the external feature ID.
- **Response:** `application/json` object:
  - `responses`: array of arrays. Each inner array corresponds to one input request and contains resolution objects with:
    - `tileId`: numeric tile ID where the feature can be found.
    - `typeId`: feature type in the resolved context.
    - `featureId`: resolved feature ID string within that tile.

Datasources are free to implement more advanced resolution schemes (for example mapping secondary ID schemes to primary ones) as long as they return consistent tile and feature identifiers.

## `/config` – inspect and update configuration

The `/config` endpoint family exposes the YAML configuration used by `mapget` for datasource wiring and HTTP settings. Command-line flags control whether datasource config is exposed and whether updates are accepted.

<!-- --8<-- [start:config-endpoints] -->

### `GET /config`

- **Method:** `GET`
- **Request body:** none
- **Response:** `application/json` object with the keys:
  - `schema`: JSON Schema used to validate datasource-model configurations.
  - `model`: JSON representation of the current YAML config, limited to top-level keys in the active datasource schema. The built-in schema includes `sources`; deployments can add keys such as `http-settings` through `--config-schema`.
  - `readOnly`: boolean flag indicating whether `POST /config` is enabled.
  - `datasourceConfigUnavailable`: boolean flag indicating that datasource config could not or must not be exposed.
  - `datasourceConfigUnavailableReason`: `null` on success, otherwise a stable reason string.
  - Additional public sections registered by the embedding application, returned as top-level siblings of `model`.

When the endpoint handler is reached, `GET /config` returns HTTP `200`. `readOnly` reflects whether `POST /config` is enabled. If `--no-get-config` is set, `datasourceConfigUnavailable` is `true`, `datasourceConfigUnavailableReason` is `getConfigDisabled`, and `model` is empty. In that state, writable servers still return `schema` so clients can present an empty replacement editor; read-only servers return an empty schema.

Unavailable reason values are:

- `getConfigDisabled`
- `configPathUnset`
- `configFileMissing`
- `configFileOpenFailed`
- `configParseFailed`
- `configValidationFailed`

On a successful datasource-config response, `datasourceConfigUnavailable` is `false` and `datasourceConfigUnavailableReason` is `null`. The returned model masks sensitive fields: any `password` or `api-key` values are replaced with stable masked tokens.

Registered public sections are read-only. They are included as top-level siblings of `model` when the YAML config can be read and parsed, even if the datasource model itself is hidden through `--no-get-config`. If the YAML config cannot be read or parsed, registered public sections are still present but empty.

### `POST /config`

- **Method:** `POST`
- **Request body:** `application/json` matching the schema returned by `GET /config`.
  - Must contain the datasource-model keys required by the schema.
- **Response:**
  - `text/plain` success message when the configuration was validated, written to disk and successfully applied.
  - `text/plain` error description and a 4xx/5xx status code if validation or application failed.

This call is only accepted if the server is started with `--allow-post-config`. When a valid configuration is posted, mapget rewrites the datasource-model fields in the underlying YAML file, preserving real secret values where masked tokens were supplied, and then reloads the datasource configuration. Unknown top-level YAML sections, including registered public sections, are preserved but not edited through this endpoint. Clients should be prepared for temporary 5xx errors if reloading fails.

<!-- --8<-- [end:config-endpoints] -->
