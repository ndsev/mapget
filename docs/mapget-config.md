# Mapget Configuration

Mapget uses a YAML configuration file to describe which datasources are available and, optionally, to persist HTTP‑related settings for data sources. This document explains the structure of that file, the supported datasource types and the most important environment variables.

!!! note "Share configuration files between tools"
    The same YAML configuration format is used by the standalone `mapget` server, embedded mapget deployments, and configuration UIs consuming `/config`. This makes it practical to reuse the same configuration file across local testing setups and containerised deployments.

## Configuration files and `--config`

All `mapget` subcommands accept a `--config` option that points to a YAML file. When you start the HTTP server with:

```bash
mapget --config path/to/service.yaml serve
```

the server remembers that path, subscribes to the file and watches it for changes. The same file is also used by the `/config` endpoint for reading and writing configuration.

Two top‑level keys are relevant for mapget itself:

- `mapget` (optional) - contains command‑line options.
- `sources` (required) - lists the datasources that should be attached to the service. This section is described in detail below.

For integration with configuration UIs there is an additional top‑level key:

- `http-settings` (optional) stores HTTP‑related settings used by frontends or tooling. Mapget itself does not interpret its contents. It is exposed through `/config.model` only when the active datasource schema includes `http-settings`, typically via a deployment-specific `--config-schema` patch.

Embedded applications can also register additional public top-level sections for `GET /config`. These sections are returned as siblings of `model`, not inside `model`, and are outside mapget's datasource schema. For example, an embedding application may register a frontend settings section for its own UI defaults. `POST /config` remains scoped to datasource-model keys and preserves unknown public sections in the YAML file.

Changes to the `sources` section take effect while the server is running. Changes to options under `mapget` only apply after the server is restarted.

## The `sources` section

The `sources` key must contain a YAML list. Each entry describes a datasource and must provide a `type` field. At runtime, mapget matches the type string to a registered constructor and passes the remaining fields in the entry as configuration.

By default, the HTTP service registers the following datasource types:

- `DataSourceHost` – connect to a remote `DataSourceServer` over HTTP.
- `DataSourceProcess` – spawn a datasource process locally and connect to it.
- `GridDataSource` – generate synthetic tiles on the fly for testing and benchmarking.
- `GeoJsonFolder` – serve features from local GeoJSON files.
- `GeoJsonEndpoint` – serve features from GeoJSON files fetched over HTTP.

Additional datasource types can be registered from C++ code using `DataSourceConfigService`, but those are outside the scope of this guide.

Every datasource entry accepts these generic fields in addition to type-specific fields:

- `enabled` (optional, default `true`): when set to `false`, the entry is skipped before its type-specific constructor is called.
- `ttl` (optional): cache time-to-live override in seconds. `0` means infinite.
- `auth-header` (optional): header-to-regular-expression map as described below.

Disabled entries are counted separately in service diagnostics and are not treated as construction failures.

### Restricting access with `auth-header`

<!-- --8<-- [start:restrict-access] -->

For all datasources you can restrict visibility by adding an `auth-header` field. It must be a mapping from header names to regular expressions. A datasource will only serve data if at least one of the required headers in the incoming request matches the configured regular expression.

Example:

```yaml
sources:
  - type: DataSourceHost
    url: https://example.com/mapget
    auth-header:
      X-User-Role: privileged
```

<!-- --8<-- [end:restrict-access] -->

With this configuration the datasource is only visible to clients that send an `X-User-Role` header whose value matches the `privileged` pattern.

### Tile TTL

<!-- --8<-- [start:ttl] -->

Mapget provides a time-to-live (TTL) option, which can be set via the YAML config
or as a `--ttl` command line option. Time-to-live controls the time period for which the cached tile is valid.
Note: The `ttl` value is always indicated in seconds, and 0 means `infinite` (no cache expiry).

Individual datasource entries can override this with a `ttl` field in the corresponding YAML node, also expressed in seconds:

```yaml
sources:
  - type: GeoJsonFolder
    folder: /maps/my-geojson-tiles
    ttl: 60  # Override default TTL, one minute for this datasource.
mapget:
  serve:
    ttl: 86400 # Default TTL: One day (60s/m * 60m/h * 12h/d)
```

**Note:** Any TTL default value is only applied if the datasource itself has not already set a TTL directly for the tile.

<!-- --8<-- [end:ttl] -->

## Built-in datasource types

### `DataSourceHost`

<!-- --8<-- [start:dshost] -->

`DataSourceHost` connects the service to an external HTTP datasource server.

Required fields:

- `type`: must be `DataSourceHost`.
- `url`: host and port of the remote datasource server, for example `localhost:9000`.

Optional fields:

- `auth-header`: header‑to‑regex mapping as described above.

Example:

```yaml
sources:
  - type: DataSourceHost
    url: localhost:9000
```

<!-- --8<-- [end:dshost] -->

### `DataSourceProcess`

<!-- --8<-- [start:dsprocess] -->

`DataSourceProcess` starts a datasource server process locally, monitors its lifetime and connects to it over HTTP. This is convenient for datasources implemented in other languages or built as separate executables.

Required fields:

- `type`: must be `DataSourceProcess`.
- `cmd`: command line used to start the datasource process.

Optional fields:

- `auth-header`: header‑to‑regex mapping as described above.

Example:

```yaml
sources:
  - type: DataSourceProcess
    cmd: cpp-sample-http-datasource
```

The process is expected to log a line indicating the port it is listening on, which mapget parses to connect the HTTP client.

<!-- --8<-- [end:dsprocess] -->

### `GridDataSource`

<!-- --8<-- [start:grid] -->

`GridDataSource` is a procedural generator for synthetic map data. It is useful for load testing, demos and automated tests where no real map data is available. Configuration is fully contained in the YAML entry and follows a flexible schema.

Required fields:

- `type`: must be `GridDataSource`.

Optional top‑level fields:

- `enabled`: boolean flag to switch the datasource on or off without removing it from the file. If set to `false`, the entry is ignored.
- `mapId`: string ID of the map. Defaults to `GridDataSource`.
- `spatialCoherence`: boolean switch controlling whether generated features take neighbouring tiles into account.
- `collisionGridSize`: numeric grid size used for spatial distribution of features.
- `layers`: list of layer configurations.

Each layer configuration can specify:

- `name`: layer name.
- `featureType`: feature type identifier.
- `geometry`: structure describing geometry type and parameters such as density and curvature.
- `attributes`: optional description of top‑level and layered attributes.
- `relations`: optional relation definitions between features in different layers.

A minimal example that creates a single synthetic layer could look like this:

```yaml
sources:
  - type: GridDataSource
    mapId: DemoGrid
    spatialCoherence: true
    collisionGridSize: 20.0
    layers:
      - name: Roads
        featureType: Road
        geometry:
          type: line
          density: 0.05
        attributes:
          top:
            - name: speedLimit
              type: int
              generator: random
              min: 30
              max: 130
```

The generator will produce deterministic but varied features for any requested tile ID. The full set of fields is defined in the `gridsource` library and can be explored by looking at example configurations or the header file.

<!-- --8<-- [end:grid] -->

### `GeoJsonFolder`

<!-- --8<-- [start:geojson] -->

`GeoJsonFolder` serves tiles from a directory containing GeoJSON files. It accepts the usual GeoJSON geometry families, including `Point`, `MultiPoint`, `LineString`, `MultiLineString`, `Polygon`, `MultiPolygon`, and `GeometryCollection`.

Required fields:

- `type`: must be `GeoJsonFolder`.
- `folder`: filesystem path to a directory containing `.geojson` files.

Optional fields:

- `mapId`: optional map ID override. If omitted, mapget derives a display name from the input directory.
- `withAttrLayers` (default: `true`): boolean flag. If `true`, nested objects in the GeoJSON `properties` are converted to mapget attribute layers; if `false`, only scalar top‑level properties are emitted and nested objects are silently dropped.
- `dataSourceInfo`: inline datasource info object, local YAML/JSON file path, or HTTP(S) URL.
- `tilePathTemplate`: relative path template used when `dataSourceInfo` is configured. Supported placeholders are `{tileId}` and `{layerId}`.

Example:

```yaml
sources:
  - type: GeoJsonFolder
    folder: /data/tiles
    mapId: Road Test Data
    withAttrLayers: true
```

Example with explicit layer metadata and template-based file lookup:

```yaml
sources:
  - type: GeoJsonFolder
    folder: /data/tiles
    dataSourceInfo: /data/tiles/info.yaml
    tilePathTemplate: "{layerId}/{tileId}.geojson"
```

If `dataSourceInfo` is omitted, mapget falls back to legacy discovery:

- first it looks for a legacy `manifest.json`
- if no manifest exists, it scans the folder for files named `<tileId>.geojson`

In that fallback mode, metadata is synthesized as a single `GeoJsonAny` layer and conversion is best-effort.

#### Manifest Mode (Legacy)

If no explicit `dataSourceInfo` is configured and a `manifest.json` file exists in the input directory, it is used to map filenames to tile IDs and layers. This allows arbitrary filenames and multi‑layer support.

**Manifest Structure:**

```json
{
  "version": 1,
  "metadata": {
    "name": "My Dataset",
    "description": "Optional description of the dataset",
    "source": "OpenStreetMap",
    "created": "2024-01-15",
    "author": "Your Name",
    "license": "CC-BY-4.0"
  },
  "index": {
    "defaultLayer": "GeoJsonAny",
    "files": {
      "roads.geojson": { "tileId": 121212121212, "layer": "Road" },
      "lanes.geojson": { "tileId": 121212121212, "layer": "Lane" },
      "other.geojson": { "tileId": 343434343434 },
      "simple.geojson": 565656565656
    }
  }
}
```

**Manifest Fields:**

| Field | Required | Description |
|-------|----------|-------------|
| `version` | No | Manifest format version (default: `1`). |
| `metadata` | No | Optional metadata about the dataset. All sub‑fields (`name`, `description`, `source`, `created`, `author`, `license`) are optional strings. |
| `index` | No | File‑to‑tile mapping configuration. |
| `index.defaultLayer` | No | Default layer name for files without explicit layer (default: `"GeoJsonAny"`). |
| `index.files` | No | Object mapping filenames to tile information. |

**File Entry Formats:**

Each entry in `index.files` maps a filename to tile information. Two formats are supported:

1. **Full format** (object): `{ "tileId": <number>, "layer": "<string>" }`
   - `tileId` (required): The mapget tile ID as a 64‑bit unsigned integer.
   - `layer` (optional): Layer name. If omitted, uses `defaultLayer`.

2. **Short format** (number): Just the tile ID as a number. Uses `defaultLayer` for the layer name.

This allows multiple GeoJSON files to contribute features to the same tile in different layers, enabling separation of feature types (e.g., roads, lanes, buildings) while sharing the same tile coordinate.

#### Legacy Mode (Deprecated)

!!! warning "Deprecation Notice"
    Legacy mode is deprecated and will be removed in a future release. Please migrate to manifest mode by adding a `manifest.json` file to your data directory. When legacy mode is used, a warning is logged to help identify directories that need migration.

If no `manifest.json` exists, the datasource falls back to scanning for files named `<packed-tile-id>.geojson` (e.g., `123456.geojson`). All files are served from a single `GeoJsonAny` layer.

<!-- --8<-- [end:geojson] -->

### `GeoJsonEndpoint`

`GeoJsonEndpoint` serves GeoJSON tiles from an HTTP(S) endpoint. It uses the same GeoJSON conversion logic as `GeoJsonFolder`, but fetches each tile body over the network.

Required fields:

- `type`: must be `GeoJsonEndpoint`.
- `baseUrl`: base HTTP(S) URL used to fetch GeoJSON tiles.

Optional fields:

- `mapId`: optional map ID override. If omitted, mapget derives a display name from the base URL.
- `withAttrLayers` (default: `true`): converts nested GeoJSON property objects to mapget attribute layers.
- `dataSourceInfo`: inline datasource info object, local YAML/JSON file path, or HTTP(S) URL.
- `tileUrlTemplate`: URL or relative path template used to fetch tiles. Supported placeholders are `{tileId}`, `{layerId}`, and `{baseUrl}`.

Example:

```yaml
sources:
  - type: GeoJsonEndpoint
    baseUrl: https://example.test/tiles
    dataSourceInfo: https://example.test/info.yaml
    tileUrlTemplate: "{layerId}/{tileId}.geojson"
```

If `dataSourceInfo` is omitted, mapget emits a strong warning and falls back to a synthesized single-layer `GeoJsonAny` datasource with empty coverage. In that fallback mode, conversion is still attempted, but service discovery remains intentionally limited.

## HTTP settings for tools and UIs

The optional `http-settings` top‑level key is reserved for HTTP‑related configuration used by tools and user interfaces. It is typically a list of objects that may contain fields such as `scope`, `api-key` or `password`.

Mapget itself treats this section as opaque data and does not interpret it when serving tiles. It is included in `/config.model` only when the active datasource schema contains an `http-settings` property, for example through a deployment-specific `--config-schema` patch. When returning the configuration, mapget replaces the values of any `api-key` or `password` fields with masked tokens. When a modified configuration is posted back, these tokens are resolved to the original secret values before the YAML file is updated.

## Environment variables

<!-- --8<-- [start:env] -->

Several environment variables control logging behaviour independently of the YAML configuration:

| Variable Name | Details                            | Value                                               |
| ------------- |------------------------------------|-----------------------------------------------------|
| `MAPGET_LOG_LEVEL` | Set the spdlog output level.       | "trace", "debug", "info", "warn", "err", "critical" |
| `MAPGET_LOG_FILE` | Optional file path to write the log. | string                                              |
| `MAPGET_LOG_FILE_MAXSIZE` | Max size for the logfile in bytes. | string with unsigned integer                        |

These settings apply to both the Python entry point (`python -m mapget`) and the native executable built from the CMake project.

<!-- --8<-- [end:env] -->

## Command-line options in YAML

<!-- --8<-- [start:yamlconf] -->

All of `mapget serve`’s command-line switches can be persisted in the same YAML file under a `mapget` key. Use the long option names without leading dashes; the server applies them on startup and then watches the rest of the file (`sources`, `http-settings`) for live changes.

```yaml
mapget:
  serve:
    port: 9000                  # --port
    cache-type: persistent      # --cache-type [memory|persistent|none]
    cache-dir: /var/lib/mapget/cache.db   # --cache-dir (used with persistent cache)
    cache-max-tiles: 20000      # --cache-max-tiles (0 disables the limit)
    clear-cache: false          # --clear-cache
    allow-post-config: true     # --allow-post-config (enables POST /config)
    no-get-config: false        # --no-get-config (hides datasource model in GET /config)
    webapp: /srv/my-ui          # --webapp, one application document root
    static-mount:               # --static-mount, additional static aliases
      - /assets:/srv/assets
    memory-trim-binary-interval: 100  # --memory-trim-binary-interval
    memory-trim-json-interval: 0      # --memory-trim-json-interval

http-settings: ...
sources: ...
```

Adjust or omit fields as needed; unspecified options fall back to the same defaults as the CLI flags (for example, in-memory cache, port 0, GET `/config` enabled, POST `/config` disabled). Static mount entries use `[<url-scope>:]<filesystem-path>` syntax and are served as plain files; mapget does not attach application-specific meaning to those files.

Datasource editor visibility is controlled by `allow-post-config` and `no-get-config`:

| `allow-post-config` | `no-get-config` | `/config` datasource model | Editor behaviour |
| --- | --- | --- | --- |
| `false` | `false` | Current model is returned. | Read-only. |
| `true` | `false` | Current model is returned. | Editable. |
| `false` | `true` | Model and schema are empty. | Disabled. |
| `true` | `true` | Model is empty, schema is returned. | Empty editor; applying overwrites datasource config. |

<!-- --8<-- [end:yamlconf] -->
