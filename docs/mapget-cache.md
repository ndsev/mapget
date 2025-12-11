# Caching Guide

Mapget keeps recently requested tiles in a cache so that repeated requests can be answered without contacting the underlying datasources again. This guide explains the available cache types, how to configure them and how to inspect cache behaviour.

## Cache types

The `mapget serve` command offers three cache modes via the `--cache-type` option:

- `memory` (default) keeps tile blobs in an in‑memory FIFO cache. When the configured tile limit is reached, the oldest tiles are evicted first.
- `persistent` stores tiles and string pools in a SQLite database on disk. This survives server restarts and is suited for long‑running deployments.
- `none` disables caching completely and forwards every request directly to the datasources.

For historical reasons the value `rocksdb` is still accepted, but it is treated as an alias for `persistent` and uses the SQLite implementation internally.

## Configuring cache behaviour

Cache settings are controlled entirely from the command line; they do not live in the YAML datasource config. The most relevant options for `mapget serve` are:

- `-c, --cache-type` chooses between `memory`, `persistent` and `none`. The default is `memory`.
- `--cache-dir` sets the path to the persistent cache file when `--cache-type persistent` is used. The default is a file called `mapget-cache` in the current working directory.
- `--cache-max-tiles` limits the number of tiles kept in the cache. The default is `1024`; a value of `0` disables the limit.
- `--clear-cache` clears an existing persistent cache file at startup before the server begins to process requests.

In memory mode, mapget keeps at most `cache-max-tiles` tile blobs in a FIFO queue. When a new tile is added and the limit is exceeded, the cache evicts the oldest tile and logs a debug message. In persistent mode, the SQLite backend tracks insertion order and removes the oldest entries once the configured tile limit is reached.

Cache hits and misses are decided per tile: if a tile for the requested map, layer and tile ID exists in the cache, the service returns it immediately; otherwise the corresponding datasource is asked to produce the tile and the result is inserted into the cache.

## String pools and binary caching

When the cache is used with binary streaming, mapget stores not only tile blobs but also the shared string pools that describe field names. Each datasource node has its own string pool, which is cached alongside the tile data. This allows subsequent binary responses to reuse string IDs and avoid resending the full field name dictionary on every request.

Advanced clients can take advantage of this by setting the `stringPoolOffsets` field in `/tiles` requests, but it is perfectly fine to ignore this mechanism and rely on the cache alone.

## Inspecting cache statistics

The easiest way to see how the cache behaves is to call `GET /status` on the running server. The HTML status page contains:

- Global service information such as the number of active datasources and worker threads.
- Cache statistics, including `cache-hits`, `cache-misses` and the number of loaded string pools.

When the in‑memory cache is used, additional fields show the current number of cached tiles and the size of the FIFO queue. These values provide a quick indication of whether the chosen cache size is appropriate for the workload.

