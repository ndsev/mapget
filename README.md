# mapget

*mapget* is a server-client solution for cached map feature data retrieval.

**Main Capabilities:**

* Coordinating requests for map data to various map data source processes.
* Integrated map data cache with SQLite-based persistent storage or simple in-memory cache.
* Simple data-source API with bindings for C++, Python and JS.
* Compact GeoJSON feature storage model - [25 to 50% smaller than BSON/msgpack](docs/size-comparison/table.md).
* Integrated deep [feature filter language](https://github.com/klebert-engineering/simfil) based on (a subset of) *JSONata*
* PIP-installable server and client component.

## Documentation

- **User guide** (`docs/mapget-user-guide.md`) – covers installation, configuration, REST usage, caching, data model basics and troubleshooting.
- **Developer guide** (`docs/mapget-dev-guide.md`) – explains the architecture, libraries, datasource interfaces, diagrams and build instructions.
