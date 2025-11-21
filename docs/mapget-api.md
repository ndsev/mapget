# REST API Guide

Mapget exposes a small HTTP API that lets clients discover datasources, stream tiles, abort long‑running requests, locate features by ID and inspect or update the running configuration. This guide describes the endpoints and their request and response formats.

## Base URL and formats

The server started by `mapget serve` listens on the configured host and port (by default on all interfaces and an automatically chosen port). All endpoints are rooted at that host and port.

Requests that send JSON use `Content-Type: application/json`. Tile streaming supports two response encodings, selected via the `Accept` header:

- `Accept: application/jsonl` returns a JSON‑Lines stream where each line is one JSON object.
- `Accept: application/binary` returns a compact binary stream optimized for high-volume traffic.

The binary format and the logical feature model are described in more detail in `mapget-model.md`.

## `/sources` – list datasources

`GET /sources` returns a JSON array describing all datasources currently attached to the service.

- **Method:** `GET`
- **Request body:** none
- **Response:** `application/json` array of datasource descriptors

Each item contains map ID, available layers and basic metadata. This endpoint is typically used by frontends to discover which maps and layers can be requested via `/tiles`.

## `/tiles` – stream tiles

`POST /tiles` streams tiles for one or more map–layer combinations. It is the main data retrieval endpoint used by clients such as erdblick.

- **Method:** `POST`
- **Request body (JSON):**
  - `requests`: array of objects, each with:
    - `mapId`: string, ID of the map to query.
    - `layerId`: string, ID of the layer within that map.
    - `tileIds`: array of numeric tile IDs in mapget’s tiling scheme.
  - `stringPoolOffsets` (optional): dictionary from datasource node ID to last known string ID. Used by advanced clients to avoid receiving the same field names repeatedly in the binary stream.
  - `clientId` (optional): arbitrary string identifying this client connection for abort handling.
- **Response:**
  - `application/jsonl` if `Accept: application/jsonl` is sent.
  - `application/binary` if `Accept: application/binary` is sent, using the tile stream protocol.

Tiles are streamed as they become available. In JSONL mode, each line is the JSON representation of one tile layer. In binary mode, the response is a sequence of versioned messages that can be decoded using the tile stream protocol from `mapget-model.md`.

If `Accept-Encoding: gzip` is set, the server compresses responses where possible, which is especially useful for JSONL streams.

### Why JSONL instead of JSON?

JSON Lines is better suited to streaming large responses than a single JSON array. Clients can start processing the first tiles immediately, do not need to buffer the complete response in memory, and can naturally consume the stream with incremental parsers.

## `/abort` – cancel tile streaming

`POST /abort` cancels a running `/tiles` request that was started with a matching `clientId`. It is useful when the viewport changes and the previous stream should be abandoned.

- **Method:** `POST`
- **Request body (JSON):** `{ "clientId": "<same-id-used-for-tiles>" }`
- **Response:** `text/plain` confirmation; a 400 status code if `clientId` is missing.

Internally the service marks the matching tile requests as aborted and stops scheduling further work for them.

## `/status` – service and cache statistics

`GET /status` returns a simple HTML page with diagnostic information.

- **Method:** `GET`
- **Request body:** none
- **Response:** `text/html`

The page shows the number of active datasources and worker threads, the size of the active request queue and cache statistics such as hit/miss counters. This endpoint is primarily used during development and debugging.

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

The `/config` endpoint family exposes the YAML configuration used by `mapget` for datasource wiring and HTTP settings. It is optional and can be enabled or disabled from the command line.

### `GET /config`

- **Method:** `GET`
- **Request body:** none
- **Response:** `application/json` object with the keys:
  - `model`: JSON representation of the current YAML config, limited to the `sources` and `http-settings` top‑level keys.
  - `schema`: JSON Schema used to validate incoming configurations.
  - `readOnly`: boolean flag indicating whether `POST /config` is enabled.

This call is disabled if the server is started with `--no-get-config`. When enabled, it provides a safe way for tools and UIs to read the active configuration without exposing secrets in plain text: any `password` or `api-key` fields are replaced with stable masked tokens.

### `POST /config`

- **Method:** `POST`
- **Request body:** `application/json` matching the schema returned by `GET /config`.
  - Must contain both `sources` and `http-settings` keys at the top level.
- **Response:**
  - `text/plain` success message when the configuration was validated, written to disk and successfully applied.
  - `text/plain` error description and a 4xx/5xx status code if validation or application failed.

This call is only accepted if the server is started with `--allow-post-config`. When a valid configuration is posted, mapget rewrites the underlying YAML file, preserving real secret values where masked tokens were supplied, and then reloads the datasource configuration. Clients should be prepared for temporary 5xx errors if reloading fails.

