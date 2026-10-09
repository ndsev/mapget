# Offline Place Lookup

Mapget adapts [plazs](https://github.com/Klebert-Engineering/plazs), an independent
read-only WOF gazetteer, to `/location` and native MCP. This is place-name lookup,
not feature-reference `/locate` or a full address geocoder.

`LocationLookup` remains the shared, injectable contract. `SqliteLocationLookup`
owns a plazs reader and translates numeric WOF IDs to `wof:<id>`, appends the
country code to display labels, and converts west/south/east/north bounds to
mapget's origin-plus-extent representation. Search, storage, geometry decoding,
ingestion and format tests live in plazs, not mapget.

## Bundle And Serve

Server builds bundle plazs's checksum-pinned 42.26 MB prepared global profile by
default. They download only that artifact, never the raw WOF source inventory.
Use `MAPGET_LOCATION_DATABASE_FILE` for a custom/offline artifact, or
`MAPGET_BUNDLE_LOCATION_DB=OFF` to omit bundled data entirely.

```sh
cmake -S . -B build -DMAPGET_LOCATION_DATABASE_FILE=/data/places.sqlite
cmake --build build --parallel 8
mapget serve --location-db /data/places.sqlite
curl 'http://localhost:8080/location?name=Germany&limit=5'
curl 'http://localhost:8080/location?id=wof:85633111'
```

The prepared artifact is copied next to the binary/Python module as
`mapget-places.sqlite`; the wheel and both MapViewer editions include it.
`--location-db` overrides the runtime path. There is no GeoNames fallback.
A standalone mapget build can select a local plazs source checkout with
`MAPGET_PLAZS_SOURCE_DIR`; otherwise CPM fetches the pinned public dependency
(Git LFS must be installed to materialize its prepared data and test fixture).
The integrated MapViewer build registers its `deps/plazs` submodule first and
reuses the parent's SQLite, ndsmath and zserio targets. The dataset helper uses
the checked-out LFS artifact when present, or downloads the identical verified
release asset if a source archive/skip-smudge checkout contains only a pointer.
Model-only/WASM builds do not acquire plazs or its schema compiler.

<!-- mcp:
title: "Offline place lookup contract"
keywords: ["place lookup", "town", "city", "country", "extent", "location", "geocoding"]
-->
## Contract

REST name search and MCP `mapget_lookup_place` return metadata without loading
geometry. REST exact-ID lookup and MCP `mapget_get_place_geometry` include any
retained Polygon/MultiPolygon, without a separate geometry flag. Their search,
identity and polygon semantics are identical. See the
[response contract](mapget-api.md#get-location).

The plazs decoder bounds input size, allocations and vertices. MCP additionally
enforces per-call response limits; oversized results retain metadata with
`complete=false`, never partial rings. Packed storage does not imply small JSON.
Consumers must display the supplied WOF attribution and license link.

The default profile retains 64,459 places and 47,957 boundaries from an October
2025 WOF snapshot, including countries/regions and localities with known population
of at least 5,000. It uses approximately one-metre NDS-grid coordinates after
0.001-degree simplification. These are place navigation/selection areas,
not cadastral boundaries. Collapsed holes/islands may be removed; metadata remains
available if an entire boundary collapses. No geometry is fabricated from a bbox.
See [plazs data preparation and format](https://github.com/Klebert-Engineering/plazs/blob/main/docs/data.md)
for precision, provenance, source credits, artifact recipes and storage limits.

The planned `placeId` request selector (server-side expansion for tiles, filters
and extraction) is separate work. This adapter does not implement polygon clipping.

## Verify

Mapget `[Location]` tests cover identity/label/extent adaptation and optional DB
availability. `test-location-wof` uses plazs's synthetic prepared artifact against
an isolated real REST/MCP server, requiring only Python's standard library. The
bundled wheel smoke test verifies lookup from an installed wheel. Importer and
binary-format tests belong to plazs; no Shapely/zserio Python dependency is added
to ordinary mapget tests or its runtime package.

```sh
build/bin/test.mapget '[Location],[mcp-native]'
ctest --test-dir build -R '^(test-location-wof|test-native-mcp)$' --output-on-failure
```
