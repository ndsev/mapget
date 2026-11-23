# mapget

*mapget* is a server-client solution for cached map feature data retrieval.

**Main Capabilities:**

* Coordinating requests for map data to various map data source processes.
* Integrated map data cache with SQLite-based persistent storage or simple in-memory cache.
* Simple data-source API with bindings for C++, Python and JS.
* Compact GeoJSON feature storage model - [25 to 50% smaller than BSON/msgpack](https://github.com/ndsev/mapget/blob/main/docs/size-comparison/table.md).
* Integrated deep [feature filter language](https://github.com/klebert-engineering/simfil) based on (a subset of) *JSONata*
* PIP-installable server and client component.

## Installation

On 64-bit CPython 3.10 through 3.14, `python -m pip install mapget` installs
both the Python bindings and the `mapget` command. See the
[setup guide](https://github.com/ndsev/mapget/blob/main/docs/mapget-setup.md#installing-mapget)
for supported platforms and how to try a release candidate.

## Documentation

- **[User guide](https://github.com/ndsev/mapget/blob/main/docs/mapget-user-guide.md)** – covers installation, configuration, REST usage, caching, data model basics and troubleshooting.
- **[Developer guide](https://github.com/ndsev/mapget/blob/main/docs/mapget-dev-guide.md)** – explains the architecture, libraries, datasource interfaces, diagrams and build instructions.
