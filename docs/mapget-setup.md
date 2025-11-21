# Mapget Setup Guide

Mapget is a small server–client stack for retrieving cached map features over HTTP or directly from C++ and Python. This guide shows how to install it, start a server, and run a first request.

## Installing mapget

The recommended way to get started is to use the published Python package. On a recent Python 3 installation, you can install mapget with:

```bash
pip install mapget
```

This gives you both the Python module and a `mapget` command-line tool which behaves the same as `python -m mapget`.

If you are working from the source tree, you can also build a native executable. In a clean checkout:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build .
```

The resulting `mapget` binary offers the same subcommands as the Python entry point.

## Starting a server

Mapget exposes its functionality through the `serve` subcommand, which binds an HTTP server and wires it to one or more datasources.

- `mapget serve` starts a server.
- `mapget fetch` connects to a server and prints returned tiles as JSON.

Run `--help` to see available options:

```bash
mapget --help
mapget serve --help
mapget fetch --help
```

A minimal example that serves data from a local datasource executable might look like this:

```bash
mapget serve \
  --port 8080 \
  --datasource-exe cpp-sample-http-datasource
```

In most setups you will instead point mapget at a YAML configuration file which describes your datasources. The `--config` option is shared between all subcommands and accepts the path to such a file:

```bash
mapget --config examples/config/sample-service.yaml serve
```

When `--config` is used, the HTTP service subscribes to the referenced YAML file. Any changes to its `sources` section are picked up while the server is running. Details of that file format are described in `mapget-config.md`.

## Using the fetch client

The `fetch` subcommand is a simple HTTP client for `/tiles`. It requests one map layer for a set of tiles and prints each tile as a JSON feature collection.

Example:

```bash
mapget fetch \
  --server localhost:8080 \
  --map Tropico \
  --layer WayLayer \
  --tile 1 --tile 2 --tile 3
```

Internally this translates to a POST request against `/tiles` with a JSON body containing `mapId`, `layerId` and a list of `tileIds`. The HTTP protocol and response formats are documented in `mapget-api.md`.

## Logging and diagnostics

For local experimentation it is often useful to increase log verbosity. You can either pass a command-line option or use environment variables:

- `MAPGET_LOG_LEVEL` controls the log level (`trace`, `debug`, `info`, `warn`, `err`, `critical`).
- `MAPGET_LOG_FILE` optionally directs logs into a file instead of standard error.
- `MAPGET_LOG_FILE_MAXSIZE` limits the size of a log file before it is rotated.

These settings apply to both the Python entry point and the native executable and are especially handy when debugging datasources or cache behaviour.

