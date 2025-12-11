# Mapget Configuration

Mapget uses a YAML configuration file to describe which datasources are available and, optionally, to persist HTTP‑related settings for data sources. This document explains the structure of that file, the supported datasource types and the most important environment variables.

!!! note "Share configuration files between tools"
    The same YAML configuration format is used by the standalone `mapget` server, embedded deployments such as the MapViewer, and configuration UIs consuming `/config`. This makes it practical to reuse the same configuration file across local testing setups and containerised deployments.

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

- `http-settings` (optional) stores HTTP‑related settings used by frontends or tooling. Mapget itself does not interpret its contents, but exposes it via `/config` and ensures that sensitive fields such as `password` and `api-key` are masked in responses.

Changes to the `sources` section take effect while the server is running. Changes to options under `mapget` only apply after the server is restarted.

## The `sources` section

The `sources` key must contain a YAML list. Each entry describes a datasource and must provide a `type` field. At runtime, mapget matches the type string to a registered constructor and passes the remaining fields in the entry as configuration.

By default, the HTTP service registers the following datasource types:

- `DataSourceHost` – connect to a remote `DataSourceServer` over HTTP.
- `DataSourceProcess` – spawn a datasource process locally and connect to it.
- `GridDataSource` – generate synthetic tiles on the fly for testing and benchmarking.
- `GeoJsonFolder` – serve features from a directory containing `.geojson` files named by tile ID.

Additional datasource types can be registered from C++ code using `DataSourceConfigService`, but those are outside the scope of this guide.

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

`GeoJsonFolder` serves tiles from a directory containing GeoJSON files. Each file represents one tile and must be named with the tile’s numeric ID in the mapget tiling scheme, for example `123456.geojson`.

Required fields:

- `type`: must be `GeoJsonFolder`.
- `folder`: filesystem path to a directory containing `.geojson` files.

Optional fields:

- `withAttrLayers`: boolean flag. If `true`, nested objects in the GeoJSON `properties` are interpreted as attribute layers; if `false`, only scalar top‑level properties are emitted.

Example:

```yaml
sources:
  - type: GeoJsonFolder
    folder: /data/tiles
    withAttrLayers: true
```

The datasource scans the directory, infers coverage from the file names and converts each GeoJSON feature into mapget’s internal feature model when the corresponding tile is requested.

<!-- --8<-- [end:geojson] -->

## HTTP settings for tools and UIs

The optional `http-settings` top‑level key is reserved for HTTP‑related configuration used by tools and user interfaces. It is typically a list of objects that may contain fields such as `scope`, `api-key` or `password`.

Mapget itself treats this section as opaque data: it is read and written via the `/config` endpoint but not interpreted when serving tiles. When returning the configuration, mapget replaces the values of any `api-key` or `password` fields with masked tokens. When a modified configuration is posted back, these tokens are resolved to the original secret values before the YAML file is updated.

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
    no-get-config: false        # --no-get-config (set to true to disable GET /config)
    memory-trim-binary-interval: 100  # --memory-trim-binary-interval
    memory-trim-json-interval: 0      # --memory-trim-json-interval

http-settings: ...
sources: ...
```

Adjust or omit fields as needed; unspecified options fall back to the same defaults as the CLI flags (for example, in-memory cache, port 0, GET `/config` enabled, POST `/config` disabled).

<!-- --8<-- [end:yamlconf] -->
