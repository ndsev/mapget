# HTTP / WebSocket API Guide

Mapget exposes a small HTTP + WebSocket API that lets clients discover datasources, stream tiles, locate features by ID and inspect or update the running configuration. Interactive tile streaming now uses a WebSocket control channel plus `/tiles/next` pull requests for the binary tile data. This guide describes the endpoints and their request and response formats.

## Base URL and formats

The server started by `mapget serve` listens on the configured host and port (by default on all interfaces and an automatically chosen port). All endpoints are rooted at that host and port.

Requests that send JSON use `Content-Type: application/json`. HTTP tile streaming supports two response encodings, selected via the `Accept` header:

- `Accept: application/jsonl` returns a JSON‑Lines stream where each line is one JSON object.
- `Accept: application/binary` returns a compact binary stream optimized for high-volume traffic.

The binary format and the logical feature model are described in more detail in `mapget-model.md`.

## `/sources` – list datasources

`GET /sources` returns a JSON array describing all datasources currently attached to the service.

- **Method:** `GET`
- **Request body:** none
- **Response:** `application/json` array of datasource descriptors

Each item contains map ID, available layers and basic metadata. Each layer entry includes its type, `zoomLevels`, `coverage`, staged-loading metadata (`stages`, optional `stageLabels`, `highFidelityStage`) and feature-type information. This endpoint is typically used by frontends to discover which maps and layers can be requested via `/tiles`.

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

## `/tiles` – interactive control channel (WebSocket)

`GET /tiles` supports WebSocket upgrades. This endpoint is the control channel for interactive clients. It carries request updates and lightweight status/control frames; binary tile data is pulled separately via `/tiles/next`.

- **Connect:** `ws://<host>:<port>/tiles`
- **Client → Server:** send one *text* message containing the same JSON body as for `POST /tiles` (`requests`, optional `stringPoolOffsets`).
  - `stringPoolOffsets` is optional; the server remembers the latest offsets per WebSocket connection. Clients may re-send it to reset/resync offsets.
- **Server → Client:** sends *binary* WebSocket messages carrying VTLV control frames.
  - `RequestContext` frames contain a UTF-8 JSON payload with `requestId` and `clientId`. The `clientId` is then used for `/tiles/next`.
  - `Status` frames contain UTF-8 JSON describing per-request `RequestStatus` transitions and a human-readable message. The final status frame has `"allDone": true`.
  - `LoadStateChange` exists in the protocol but is currently not emitted by the HTTP service.

Each entry in a status frame's `requests` array contains `index`, `mapId`, `layerId`, numeric `status`, and `statusText`. For `NoDataSource` statuses, servers may also include `noDataSourceReason`:

- `emptySources`
- `allSourcesDisabled`
- `datasourceInitializationFailed`
- `missingMapOrLayer`
- `noConfig`

To cancel, either send a new request message on the same connection (which replaces the current one) or close the WebSocket connection.

## `/tiles/next` – pull binary tile frames

`GET /tiles/next` (also accepts `POST`) returns the next available binary tile frame batch for an active interactive `/tiles` session.

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

     auto receivedTileCount = 0;
     client.request(std::make_shared<LayerTilesRequest>(
         "Tropico",
         "WayLayer",
         std::vector<TileId>{{1234, 5678, 9112, 1234}},
         [&](auto&& tile) { receivedTileCount++; }
     ))->wait();

     std::cout << receivedTileCount << std::endl;
     service.stop();
}
```

Keep in mind, that you can also run a `mapget` service without any RPCs in your application. Check out [`examples/cpp/local-datasource`](examples/cpp/local-datasource/main.cpp) on how to do that.

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
- `tilesWebsocket`: control-channel metrics such as active sessions, pending queued frames for `/tiles/next`, blocked pull requests, and total forwarded bytes / frames

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
  - `model`: JSON representation of the current YAML config, limited to datasource-model top-level keys such as `sources` and `http-settings`.
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
