# Mapget Simfil Extensions

The core simfil language is documented in `simfil/docs/simfil-language.md`. This companion chapter describes the mapget-specific additions that make it easier to query tiles, reason about geometry and integrate simfil with feature search.

## Running simfil against tiles

Every `TileFeatureLayer` embeds a `simfil::ModelPool`, so you can execute expressions directly against a tile without converting it to JSON. The most important helpers are:

- `TileFeatureLayer::evaluate(query, node)` – evaluates an expression and returns a `QueryResult` that contains values, diagnostics and optional traces.
- `TileFeatureLayer::complete(query, caretPosition, completionOptions)` – produces auto-completion candidates for interactive editors.
- `TileFeatureLayer::QueryResult::diagnostics` – exposes parser and runtime warnings that can be surfaced in UIs.

Erdblick’s feature search dialog and the worker processes behind `/search` rely on these APIs: each worker compiles a simfil expression once per tile, runs it via `evaluate`, and then reports values plus diagnostics back to the UI.

## Geometry-aware operators

<!-- --8<-- [start:geo-ops] -->
Mapget enriches simfil with dedicated geometry meta types (`Point`, `BBox`, `LineString`, `Polygon`) and implements the spatial operators `within`, `contains`, and `intersects` for them. These operators are exposed as regular binary operators in simfil:

| Example expression                        | Meaning                                                   |
|-------------------------------------------|-----------------------------------------------------------|
| `geo(feature.geometry) within bbox(...)`  | Returns `true` if the feature geometry lies within the bounding box. |
| `point(11.5, 48.1) within geo("@geojson")`| Checks whether the point is inside the provided GeoJSON geometry literal. |
| `linestring(feature.geometry[0]) intersects bbox(...)` | Tests whether the first geometry intersects a screen-space bounding box. |
| `geo(feature.geometry) contains point(x,y)` | Tests whether a polygon or mesh contains a point.        |

These operators work with 2D and 3D coordinates alike.
Because the operators are part of simfil’s normal precedence rules, you can combine them freely with other expressions, e.g. `geo() within bbox(...) && properties.**.speedLimit > 80`.

To make geometry queries concise, mapget registers a set of constructor functions:

- `geo(value?)` – parses a GeoJSON object or string literal into a geometry value that can be used with the operators above. Note, that the `value` argument is optional. If ommitted, it simply reads geometry from the current evaluation context.
- `point(x, y, z?)` – creates a point.
- `bbox(minX, minY, maxX, maxY)` – creates an axis-aligned bounding box.
- `linestring([ [x0, y0], [x1, y1], ... ])` – creates a line string from coordinate arrays.

These constructors return strongly typed simfil values, so you can pass them directly to `within`/`contains`/`intersects` operators.

<!-- --8<-- [end:geo-ops] -->

## Typed meta types and helpers

The geometry constructors feed into dedicated meta types (`PointType`, `BBoxType`, `LineStringType`, `PolygonType`) that implement simfil’s arithmetic hooks:

- Unary/binary operators on those types call into mapget’s geometry helpers, so you can write expressions like `pointA - pointB` or `bboxA intersects bboxB`.
- Each meta type exposes `unpack` support, which lets you iterate over coordinates or bounding boxes in simfil loops.

Together with the binary operators, this gives you enough expressiveness to describe common map filters (“features whose geometry is within this ROI”) without resorting to custom code.
