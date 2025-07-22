# mapget

*mapget* is a server-client solution for cached map feature data retrieval.

**Main Capabilities:**

* Coordinating requests for map data to various map data source processes.
* Integrated map data cache with SQLite-based persistent storage or simple in-memory cache.
* Simple data-source API with bindings for C++, Python and JS.
* Compact GeoJSON feature storage model - [25 to 50% smaller than BSON/msgpack](docs/size-comparison/table.md).
* Integrated deep [feature filter language](https://github.com/klebert-engineering/simfil) based on (a subset of) *JSONata*
* PIP-installable server and client component.

## Python Package and CLI

The `mapget` package is deployed to PyPI for any Python version between 3.8 and 3.11. Simply running `pip install mapget` is enough to get you started:

* **`python -m mapget serve`** will run a server,
* **`python -m mapget fetch`** allows you to talk to a remote server,
* you can also use the Python package to write a data source, as documented [here](#implementing-a-data-source).

If you build `mapget` from source as described below, you obtain an executable that can be used analogously to the Python package with `mapget serve` or `mapget fetch`. 

## Configuration

The command line parameters for `mapget` and its subcommands can be viewed with:

```bash
mapget --help
mapget fetch --help
mapget serve --help
```

(or `python -m mapget --help` for the Python package).

The `mapget` executable can parse a `YAML`-based config file. The path to the config file can be provided to `mapget` via command line by specifying the `--config` parameter.
The config file may have two top-level keys which are used by mapget: The `sources` key and the `mapget` key.

### The `mapget` YAML key

Under the `mapget` YAML key, any config options which can also be set via the command line may be set.
Note, that changes in this section are not applied while mapget is running, you will need to restart it.
Sample configuration files can be found under `examples/config`:

- [sample-first-datasource.yaml](examples/config/sample-first-datasource.yaml) and [sample-second-datasource.yaml](examples/config/sample-second-datasource.yaml) will configure mapget to run a simple datasource with sample data. Note: the two formats in config files for subcommand parameters can be used interchangeably.
- [sample-service.yaml](examples/config/sample-service.yaml) to execute the `mapget serve` command. The instance will fetch and serve data from sources started with `sample-*-datasource.yaml` configs above.

### The `sources` YAML key

Under the `sources` YAML key, you can configure datasources which are going to be served.
Note, that changes from the sources section are going to be applied immediately once the config
file is saved. This means, you can add and/or remove sources while mapget is running.
This section has the following format: The `sources` key must have a list. Each entry in the list
represents a datasource. The entry must have a `type` key, which denotes the specific datasource
constructor to call. You may register additional datasource types using the
`DatasourceConfigService` from `mapget/service/config.h`. By default, the following datasource types are supported:

| Data Source Type        | Required Configurations | Optional Configurations |
|-------------------------|-------------------------|-------------------------|
| `DataSourceHost`        | `url`                   | `auth-header`           |
| `DataSourceProcess`     | `cmd`                   | `auth-header`           |

For example, the following would be a valid configuration:

```yaml
sources:
  - type: DataSourceProcess
    cmd: cpp-sample-http-datasource
```

**Note:** You can restrict the visibility of a datasource by using the **`auth-header`**
config field, which holds a dictionary of required header-value-regex options. For example,
the following datasource would be restricted to users, which pass an `X-User-Role: privileged` header:

```yaml
sources:
  - type: DataSourceHost
    url: ...
    auth-header:
      X-User-Role: privileged
```

### Cache

`mapget` supports persistent tile caching using SQLite-based backends, non-persistent
in-memory caching, and a 'none' option to disable caching entirely. The CLI options to configure caching behavior are:

| Option                   | Description                                                                                          | Default Value   |
|--------------------------|------------------------------------------------------------------------------------------------------|-----------------|
| `-c,--cache-type`        | Choose between "none", "memory", or "persistent" (SQLite-based).                                    | memory          |
| `--cache-dir`            | Path to store persistent cache (SQLite database file).                                              | mapget-cache    |
| `--cache-max-tiles`      | Number of tiles to store. Tiles are purged from cache in FIFO order. Set to 0 for unlimited storage. | 1024            |
| `--clear-cache`          | Clear existing cache entries at startup.                                                             | false           |

## Map Data Sources

At the heart of *mapget* are data sources, which provide map feature data for
a specified tile area on the globe and a specified map layer. The data source
must provide information as to

1. Which map it can serve (e.g. China/Michigan/Bavaria...). In the [component overview](#component-overview), this is reflected in the `DataSourceInfo` class.
2. Which layers it can serve (e.g. Lanes/POIs/...). In the [component overview](#component-overview), this is reflected in the `LayerInfo` class.
3. Which feature types are contained in a layer (e.g. Lane Boundaries/Lane Centerlines),
   and how they are uniquely identified. In the [component overview](#component-overview), this is reflected in the `FeatureTypeInfo` class.

Feel free to check out the [sample_datasource_info.json](examples/cpp/http-datasource/sample_datasource_info.json). As the *mapget* Service is asked for a tile, e.g. using the `GET /tiles` REST API,
it first queries its cache for the relevant data. On a cache miss, it proceeds
to forward the request to one of its connected data sources for the specific requested
map.

## Map Features

The atomic units of geographic data served by *mapget* are *Features*. The content of
a *mapget* feature aligns with that of a feature in *GeoJSON*: A feature consists of
a unique ID, some attributes, and some geometry. *mapget* also allows features to have
a list of child feature IDs. **Note:** Feature geometry in *mapget* may always be 3D.

### JSON Representation

While the feature uses compact column-based storage model internally, the logical view
of a *Feature* in *mapget* is based on GeoJSON:

```yaml
{
  type: "Feature",  // Mandatory for GeoJSON compliance
  id: "<type-id>.<part-value-0>...<part-value-n>",
  typeId: "<type-id>",
  "<part-name-n>": "<part-value-n>",
  "geometry": { /* GeoJSON geometry object with additional `name` field. */ },
  "properties": {
    "layers": {
      "<attr-layer-name>": {
        "<attr-name>": {
          /* attr-fields ... */,
          "validity": [{
            direction: "<Optional validity direction along the feature>",
            geometryName: { /* Optional reference to a specific feature geometry by name. */ },
            geometry: { /* Optional nested GeoJSON geometry object. */ },
            start: { /* Optional WGS84 point or scalar depending on type, indicating range. */ },
            end: { /* Optional WGS84 point or scalar depending on type, indicating range. */ },
            point: { /* Optional WGS84 point or scalar depending on type, indicating single position. */ },
            offsetType: "GeoPosOffset|BufferOffset|RelativeLengthOffset|MetricLengthOffset",
            featureId: id of the directly referenced feature if any, used to reference geometry of another feature
          }]
        }
      },
      // Additional attribute layers
    },
    "<non-layer-attr-name>": "<non-layer-attr-value>",
    // Additional non-layered attributes
  },
  "relations": [
    {
      "name": "<relation-name>",
      "target": "<target-feature-id>",
      "targetValidity": [{ /* optional target validity-geom-description. see above for fields. */ }],
      "sourceValidity": [{ /* optional source validity-geom-description. see above for fields. */ }]
    }
    // Additional relations
  ]
}
```

This structure provides a clear, hierarchical representation of a *Feature*, where:

- **`typeId`**: Indicates the feature type.
- **`id`**: A composite identifier based on the `typeId` and additional `part-values`.
- **`geometry`**: Encodes the spatial data of the feature in a format compliant with GeoJSON, but extended to support 3D geometries.
- **`properties`**: Contains both basic and layered feature attributes.
- **`relations`**: Defines relationships between this feature and others, including the spatial validity of these relationships.

### Feature ID Schemes

Each *Feature* in *mapget* is uniquely identified by an ID that is composed of
a `typeId` and one or more `part-values`. This ID scheme ensures that each feature
can be distinctly referenced across the map.

- **`typeId`**: Represents the category or class of the feature (e.g., "road", "building").
- **`part-values`**: A series of values that, together with the `typeId`, uniquely identify the feature. These might include elements like a database ID, a road section, or other unique identifiers.

Example:

```yaml
"id": "Road.1234.7"
```

In this example, `"Road"` is the `typeId`, and `"1234.7"` are the `part-values` that
make this feature's ID unique. The part values must be based on a valid feature id composition.
Valid compositions are provided for each feature type in the respective `FeatureTypeInfo`.
Multiple possible schemes may be provided - this is to facilitate indirect references.
The feature itself must always use the first (primary) feature ID composition. But feature references
may use a secondary scheme, which might for example reference the feature via its position rather than
a unique integer. Such a secondary ID scheme may be resolved to the primary using the [locate](#about-locate)
endpoint.

### Geometry Types

*mapget* supports simple GeoJSON geometry types, with a vertex component extension for 3D data.
Each feature may zero, one or multiple of these via its `GeometryCollection`:

- **Points**: A single coordinate point. Mapped to GeoJson ``
- **Line**: A series of connected points forming a line.
- **Polygon**: A closed shape formed by a series of connected points. In *mapget*, polygons must not have holes, and they are automatically closed.
- **Mesh**: An array of triangles. This can be viewed as a GeoJson `MultiPolygon`.

### Source Data References

SourceDataReferences provide a mechanism for linking a *Feature* (or an aspect of it, like an
attribute or relation) back to its original source data, e.g. to a blob.

- **`SourceDataReferenceCollection`**: This collection holds all references to the source data for a particular feature.
- **`SourceDataReferenceItem`**: Each item in the collection represents a single reference, which includes metadata about the `SourceDataLayer` and a qualifier.

These references are useful for applications that need to maintain a link between the processed geographic data and its original, unprocessed form.

## Map Tiles

For performance reasons, *mapget* features are always served in a set covering
a whole tile. Each tile is identified by a zoom level `z` and two grid
coordinates `x` and `y`. *mapget* uses a binary tiling scheme for
the earths surface: The zoom level `z` controls the number of subdivisions for
the WGS84 longitudinal `[-180,180]` axis (columns) and latitudinal `[-90,90]` axis (rows).
The tile `x` coordinate indicates the column, and the `y` coordinate indicates the row.
On level zero, there are two columns and one row. In general, the number of rows is `2^z`,
and the number of columns is `2^(z+1)`.

The content of a tile is (leniently) coupled to the geographic extent of its tile id,
but also to the map layer it belongs to. When a data source creates a tile, it associates
the created tile with the name of the map - e.g. *"Europe-HD"*, and a map data layer,
e.g. *"Roads"* or *Lanes*.

## Component Overview

The following diagram provides an overview over the libraries, their contents, and their dependencies:

![components](docs/components.png)

`mapget` consists of four main libraries:

* The `mapget-model` library is the core library which contains the feature-model abstractions.
* The `mapget-service` library contains the main `Service`, `ICache` and `IDataSource` abstractions. Using this library, it is possible to use mapget in-process without any HTTP dependencies or RPC calls.
* The `mapget-http-service` library binds a mapget service to an HTTP server interface, as described [here](#retrieval-interface).
* The `mapget-http-datasource` library provides a `RemoteDataSource` which can connect to a `DataSourceServer`. This allows running a data source in an external process, which may be written using any programming language.

## Developer Setup

*mapget* has the following prerequisites:
- C++17 toolchain
- CMake 3.14+
- Python3
- Ninja build system (not required, but recommended)
- gcovr, if you wish to run coverage tests:
  ```
  pip install gcovr
  ```
- Python wheel package, if you wish to build the mapget wheel:
  ```
  pip install wheel
  ```

Build `mapget` with the following command:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake  --build .
```

If you wish to skip building _mapget_ wheel, deactivate the `MAPGET_WITH_WHEEL` CMake 
option in the second command:
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DMAPGET_WITH_WHEEL=OFF -G Ninja
```

### CMake Build Options

_mapget_ build can be configured using the following variables:

| Variable Name | Details   |
| ------------- | --------- |
| `MAPGET_WITH_WHEEL` | Enable mapget Python wheel (output to WHEEL_DEPLOY_DIRECTORY). |
| `MAPGET_WITH_SERVICE` | Enable mapget-service library. Requires threads. |
| `MAPGET_WITH_HTTPLIB` | Enable mapget-http-datasource and mapget-http-service libraries. |
| `MAPGET_ENABLE_TESTING` | Enable testing. |
| `MAPGET_BUILD_EXAMPLES` | Build examples. |

### Environment Settings

The logging behavior of _mapget_ can be customized with the following environment variables:

| Variable Name | Details                            | Value                                               |
| ------------- |------------------------------------|-----------------------------------------------------|
| `MAPGET_LOG_LEVEL` | Set the spdlog output level.       | "trace", "debug", "info", "warn", "err", "critical" |
| `MAPGET_LOG_FILE` | Optional file path to write the log. | string                                              |
| `MAPGET_LOG_FILE_MAXSIZE` | Max size for the logfile in bytes. | string with unsigned integer                        |


## Implementing a Data Source

### [`examples/cpp/local-datasource`](examples/cpp/local-datasource/main.cpp)

This example shows, how you can use the basic non-networked `mapget::Service`
in conjunction with a custom data source class which implements the `mapget::DataSource` interface.

### [`examples/cpp/http-datasource`](examples/cpp/http-datasource/main.cpp)

This example shows how you can write a minimal networked data source service.

### [`examples/python/datasource.py`](examples/python/datasource.py)

This example shows, how you can write a data source service in Python.
You can simply `pip install mapget` to get access to the mapget Python API.

## REST API

The `mapget` library provides simple C++ and HTTP/REST interfaces, which may be
used to satisfy the following use-cases:

* Obtain streamed map feature tile data for given constraints.
* Locate a feature by its ID within any of the connected sources.
* Describe the available map data sources.
* View a simple HTML server status page (only for REST API).
* Instruct the cache to populate itself within given constraints from the connected sources.

The HTTP interface implemented in `mapget::HttpService` is a view on the C++ interface,
which is implemented in `mapget::Service`. Detailed endpoint descriptions:

| Endpoint   | Method | Description                                                                                                       | Input                                                                                                                                               | Output                                                                                                                                                                                                                                                            |
|------------|--------|-------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `/sources` | GET    | Describe the connected Data Sources                                                                               | None                                                                                                                                                | `application/json`: List of DataSourceInfo objects.                                                                                                                                                                                                               |
| `/tiles`   | POST   | Get streamed features, according to hard constraints. Accepts encoding types `text/jsonl` or `application/binary` | List of objects containing `mapId`, `layerId`, `tileIds`, and optional `stringPoolOffsets` and `clientId`.                                          | `text/jsonl` or `application/binary`                                                                                                                                                                                                                              |
| `/abort`   | POST   | Abort a currently running `/tiles` request by its `clientId`.                                                     | `clientId`                                                                                                                                          | `text/plain`                                                                                                                                                                                                                                                      |
| `/status`  | GET    | Server status page                                                                                                | None                                                                                                                                                | `text/html`                                                                                                                                                                                                                                                       |
| `/locate`  | POST   | Obtain a list of tile-layer combinations providing a feature that satisfies given ID field constraints.           | `application/json`: List of external references, where each is a Request object with `mapId`, `typeId` and `featureId` (list of external ID parts). | `application/json`: List of lists of Resolution objects, where each corresponds to the Request object index. Each Resolution object includes `tileId`, `typeId`, and `featureId`.                                                                                 |
| `/config`  | GET    | Access the config yaml-file content. Disabled iff `--no-get-config` is passed to mapget.                          | None                                                                                                                                                | `application/json`: Contains the `sources` and `http-settings` from the config-yaml as a JSON representation. The returned JSON object has a `model`, `schema` and `readOnly` key. The schema is controlled through the `--config-schema` command line parameter. |
| `/config`  | POST   | Write the config yaml-file content. Enabled iff `--allow-post-config` is passed to mapget.                        | `application/json`                                                                                                                                  | `text/plain` (if an error occurs)                                                                                                                                                                                                                                 |

### Curl Call Example

For example, the following curl call could be used to stream GeoJSON feature objects
from the `MyMap` data source defined previously:

```bash
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
```

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
     HttpClient client("localhost", service.port());

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

### About `locate`

The `/locate` endpoint allows clients to obtain a list of tile-layer combinations that provide a feature satisfying given ID field constraints. This is crucial for applications needing to find specific data points within the massive datasets typically associated with map services. The endpoint uses a POST method due to the complexity and length of the queries, which involve resolving external references to data.

**Details:**

- **Input:** The input is a list of requests, each corresponding to an external reference that needs to be resolved. Each request object includes:
    - `typeId`: Specifies the type of feature to locate.
    - `featureId`: An array representing the external ID parts, where each part consists of a field name and value. This array is used to identify the feature uniquely. The used id scheme may be a secondary scheme.

- **Output:** The output is a nested list structure where each outer list corresponds to an input request object. Each of these lists contains resolution objects that provide details about where the requested feature can be found within the map data. Each resolution object includes:
    - `tileId`: The key of the map tile containing the feature.
    - `typeId`: The type of feature found, which should match the `typeId` specified in the request.
    - `featureId`: An array of ID parts similar to the input but typically using the primary feature ID scheme of the data source.

This design allows clients to batch queries for multiple features in a single request, improving efficiency and reducing the number of required HTTP requests. It also supports the use of different ID schemes, accommodating scenarios where the request and response might use different identifiers for the same data due to varying external reference standards.

Note, that a locate resolution must be provided by a datasource for the specified map, which implements the `onLocateRequest` callback.

### erdblick-mapget-datasource communication pattern

TODO: expand and polish this section stub.

1. Client (`erdblick` etc.) sends a composite list of requests to `mapget`. Requests are batched because browsers limit the number of concurrent requests to one domain, but we want to stream potentially hundreds of tiles.

2. `mapget` checks if all requested map+layer combinations can be fulfilled with data sources
   - yes: create tile requests, stream responses back to client,
   - no: return 400 Bad Request (client needs to refresh its info on map availability).

3. A data source drops offline / `mapget` request fails during processing?
   - `cpp-httplib` cleanup callback returns timeout response (probably status code 408).

## Making mapget Releases

The `mapget` Python package is automatically deployed to PyPI through GitHub Actions:

### Release Process

1. **Update Version**: Set `MAPGET_VERSION` in `CMakeLists.txt` to the new version (e.g., `2025.3.1`)

2. **Create GitHub Release**: Create a GitHub release with tag `v2025.3.1` (matching the CMakeLists.txt version)

The GitHub Actions workflow will automatically:
- Validate that the tag matches the CMakeLists.txt version
- Build wheels for all supported platforms and Python versions
- Upload the release to PyPI

### Development Snapshots

Every commit to `main` automatically deploys a development snapshot to PyPI with a version like `2025.3.0.dev3`. No manual intervention required.
