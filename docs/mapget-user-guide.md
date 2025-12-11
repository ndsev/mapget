# Mapget User Guide

Mapget is a small server–client stack for cached map feature retrieval. It can be run as a standalone HTTP service, embedded into C++ applications or used from Python. This user guide links to the individual chapters that describe how to install mapget, how to call its REST API and how the underlying data model is structured.

Most readers will only need the setup and API chapters. The model and configuration sections are useful when you design datasources, integrate mapget into a larger system or debug performance and caching behaviour.

## Chapters

The guide is split into several focused documents:

- [**Setup Guide**](mapget-setup.md) explains how to install mapget via `pip`, how to build the native executable from source, and how to start a server or use the built‑in `fetch` client for quick experiments.
- [**Configuration Guide**](mapget-config.md) documents the YAML configuration file used with `--config`, the supported datasource types (`DataSourceHost`, `DataSourceProcess`, `GridDataSource`, `GeoJsonFolder) and the optional `http-settings` section used by tools and UIs.
- [**REST API Guide**](mapget-api.md) describes the HTTP endpoints exposed by `mapget serve`, including `/sources`, `/tiles`, `/abort`, `/status`, `/locate` and `/config`, along with their request and response formats and example calls.
- [**Caching Guide**](mapget-cache.md) covers the available cache modes (`memory`, `persistent`, `none`), explains how to configure cache size and location, and shows how to inspect cache statistics via the status endpoint.
- [**Simfil Language Extensions**](mapget-simfil-extensions.md) introduces the feature model, tiling scheme, geometry and validity concepts, and the binary tile stream format. This chapter is especially relevant if you are writing datasources or low‑level clients.
- [**Layered Data Model**](mapget-model.md) introduces the feature model, tiling scheme, geometry and validity concepts, and the binary tile stream format. This chapter is especially relevant if you are writing datasources or low‑level clients.

You can read the chapters independently. If you are new to mapget, a good starting sequence is the setup guide, the REST API guide and then the caching guide.
