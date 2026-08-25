# Caching Guide

Mapget keeps recently requested tiles in a cache so that repeated requests can be answered without contacting the underlying datasources again. This guide explains the available cache types, how to configure them and how to inspect cache behaviour.

## Cache types

The `mapget serve` command offers three cache modes via the `--cache-type` option:

- `memory` (default) keeps tile blobs in an in‑memory FIFO cache. When the configured tile limit is reached, the oldest tiles are evicted first.
- `persistent` stores tiles and string pools in a SQLite database on disk. This survives server restarts and is suited for long‑running deployments.
- `none` disables caching completely and forwards every request directly to the datasources.

For historical reasons the value `rocksdb` is still accepted, but it is treated as an alias for `persistent` and uses the SQLite implementation internally.

## Configuring cache behaviour

Cache settings are accepted as `mapget serve` command-line options and through
the corresponding `mapget.serve` YAML keys. The most relevant options are:

- `-c, --cache-type` chooses between `memory`, `persistent` and `none`. The default is `memory`.
- `--cache-dir` sets the path to the persistent cache file when `--cache-type persistent` is used. The default is a file called `mapget-cache` in the current working directory.
- `--cache-max-tiles` limits the number of tiles kept in the cache. The default is `1024`; a value of `0` disables the limit.
- `--cache-max-bytes` limits serialized tile bytes retained by the in-memory cache. By default it is derived as `cache-max-tiles * 512 KiB`, so the default `1024` entries receive a `512 MiB` byte budget. A value of `0` disables the byte limit.
- `--clear-cache` clears an existing persistent cache file at startup before the server begins to process requests.

In memory mode, mapget keeps tile blobs in a FIFO queue and evicts oldest
entries until both the count and byte limits are satisfied. A single tile
larger than the byte budget is not cached. In persistent mode, the SQLite
backend tracks insertion order and removes the oldest entries once the
configured tile-count limit is reached; `cache-max-bytes` does not constrain
the on-disk database.

Cache hits and misses are decided per tile: if a tile for the requested map, layer and tile ID exists in the cache, the service returns it immediately; otherwise the corresponding datasource is asked to produce the tile and the result is inserted into the cache.

## Resetting one map at runtime

Administrators can opt into the guarded `POST /cache/reset` endpoint with
`--allow-cache-reset` and one or more `--cache-reset-auth-header HEADER=REGEX`
options. The endpoint accepts `{"mapId":"Example/Map"}` and clears Feature and
SourceData entries for that exact map ID across every layer and tile. It keeps
string-pool dictionaries and every other map's entries.

The reset crosses the service scheduler boundary: queued requests for the map
are aborted, and work that started before the reset cannot publish a stale tile
afterward. Consequently, the next ordinary request misses mapget's tile cache
and performs datasource conversion again.

This guarantee stops at the mapget cache boundary. A datasource may retain its
own network, decode, raw-source, or negative-result cache, and another mapget
replica is unaffected. The generic implementation scans cached tile keys and
erases matches individually, so use the operation as an infrequent
administrative action rather than a high-rate invalidation API.

## String pools and binary caching

When the cache is used with binary streaming, mapget stores not only tile blobs but also the shared string pools that describe field names. Each datasource node has its own string pool, which is cached alongside the tile data. This allows subsequent binary responses to reuse string IDs and avoid resending the full field name dictionary on every request.

Advanced clients can take advantage of this by setting the `stringPoolOffsets` field in `/tiles` requests, but it is perfectly fine to ignore this mechanism and rely on the cache alone.

## Inspecting cache statistics

The easiest way to see how the cache behaves is to call `GET /status` on the running server or query `GET /status-data` directly. The HTML status page contains:

- Global service information such as active datasources, the configured worker cap, running jobs, and per-source permit pressure.
- Cache statistics, including `cache-hits`, `cache-misses` and the number of loaded string pools.
- Capacity-oriented memory for loaded string pools and the cache backend.
- A browser-local rolling history graph for selectable work, memory, transport,
  and cache gauges. No history is retained by the mapget server.

When the in‑memory cache is used, additional fields show the current number of
cached tiles, FIFO size, retained blob/index capacity, and its sampled peak.
It also reports the configured count/byte limits and current serialized tile
bytes.
The SQLite backend reports page-cache, schema, and prepared-statement memory
from SQLite's own counters. These values provide a quick indication of whether
the chosen cache size is appropriate for the workload.
