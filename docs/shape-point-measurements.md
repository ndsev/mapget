# Measurements on original geometry vertices

Use a point series when a stored attribute contains measurements at positions on
a feature's geometry. Declare how to read the existing records; **do not copy
them onto every vertex or generate one validity per record in the converter**.
Features without a series can still be filtered, picked and inspected at their
original vertices.

## Declaring a series

Suppose a Lane's CURVATURE attribute has an array named `records`. Each record
has an integer `point` (an original vertex index) and a `code`. The following
declaration belongs to the **use** of CURVATURE in one attribute layer, not to
the shared physical Attribute type. A different use can measure a different
geometry. First declare the typed records and bind the attribute through the
normal LayerSchema API, then register the decoder and series before finalizing:

```cpp
// Example table with two known codes. A real producer supplies its complete table.
mapget::PointSeries::Codec curvature;
curvature.tableOnly_ = true;
curvature.codes_ = {{.raw_ = 7, .value_ = -0.15296},
                    {.raw_ = 511, .value_ = 0.0}};
layer->addPointCodec("curvature-code", std::move(curvature));

mapget::PointSeries s;
s.featureType_ = "Lane";
s.attributeLayer_ = "LaneAdasLayerWithGeo";
s.attribute_ = "CURVATURE";
s.geometryName_ = "ADAS";
s.records_ = {"records"};
s.position_.kind_ = mapget::PointSeries::Position::Kind::VertexIndex;
s.position_.field_ = {"point"};
s.fields_ = {{"curvature", {"code"}, "1/m", "curvature-code"}};
layer->addPointSeries(std::move(s));
```

Paths contain literal field names, not expressions or string-pool IDs. A raw value
may be at a different path from its contextual field name. MapSchema validates
the source paths, numeric operands, output types and codec references before
publication. Layer JSON interchange uses `x-mapget.pointMeasurements.series`
and `codecs`.

The compact artifact contains the producer types, point declarations and the types
of decoded results and their explanations. It does **not** contain an occurrence
object and list for every attribute on every feature. On first use,
`layer->shapePointQueryType(featureName)` derives the named and enumerable
point fields from that feature's actual attribute bindings and declarations.
This also works on features with ordinary attributes and no sampled fields.
Native filtering and WASM completion use the same builder. Keep the returned
SIMFIL type: its numeric ID belongs to its own pool, not to the source MapSchema.
The native process (or one WASM module) keeps the two most recently used host
views warm **across all schemas**, not two per datasource. Active readers can
hold additional views across eviction. Dropping a source also drops its warm
ownership. Compiled point expressions can be shared by concurrent evaluations,
but the expression cache alone does not keep a point view alive.
Schema statistics count its live view types and allocated capacity
separately from the published artifact. Subset results use published schema
types, never a view-local type ID.

For scale, a native run over the 20 feature types in a 12-layer NDS.Live
schema measured roughly 0.6–18 ms to construct a cold host view, and
3.7–4.6 MB of allocated capacity per view. Those are machine- and
schema-dependent measurements, not latency guarantees or process RSS.

New point-series declarations use V10, which carries the producer-defined marker tag.
Existing V7 schemas have no declared samples. V8 and V9 schemas without a legacy
marker destination can still be read; a schema that used that destination must be
regenerated. Mapget cannot infer whether an old marker represents an ADAS measurement,
a speed anchor or another kind of location. Update native and WASM consumers together.

## Coverage is not sampling

**First** decide whether this occurrence covers this original vertex. Stored
applicability is authoritative: a named geometry, offset range, logical point
sequence, direction or host reference cannot be replaced by coordinate equality.
A range between two vertices can legitimately cover **zero** vertices.
Overlapping occurrences remain distinct; multiple matching validities of one
occurrence do not duplicate its value. Setting `omittedValidityCovers_` is a
producer statement about an **absent** validity field. An explicit empty list
never means complete coverage.

**Then** read the requested field from the local series:

| Position or storage | Meaning |
| --- | --- |
| Dense | One element per original vertex; a length mismatch is an error. |
| VertexIndex / LogicalIndex | Key by original vertex or by a sequence interleaving vertices and inserted attribute points. Insertions are not extra original vertices. |
| Fraction / Distance | Read the declared numerator and width, or scale distances into metres. A fraction is `p/(2^bits-1)`; widths may be per record or per enclosing occurrence. |
| VertexMeasure | A producer-retained, finite, strictly monotone M array, one entry per original vertex. M is not inferred from elevation or the XY chord. |
| Interval | Supply start/end paths, the axis and endpoint inclusivity. Return all covering records. |
| TaggedValidity | Pair each record with the next stored point validity bearing the declaration's tag. Other tags and assignment ranges are skipped. |
| Parallel columns | Resolve the position and value columns on `records_`. Pair by raw row ordinal, validate lengths and retain repeated positions. |

Exact sampling never snaps an off-vertex marker to an original index. Linear
sampling uses ordinary decoded values in one continuation, never crosses a
branch, never extrapolates and never invents a missing endpoint. For index-keyed
anchors, weights use distance along the declared path, not the number of intervening
vertices. For length fractions choose the producer's convention: planar WGS84 XY
length and geographic metres give different answers at the bend in a line.
A separate applicability measure, a local assignment origin and reverse stationing
are explicit declaration choices.

With anchors 80 and 60 at the ends of a line, a **covered** midpoint yields 70
and two contributors of weight 0.5. The same anchors over a short stored assignment
that excludes the midpoint yield no occurrence there. Attribute-scope inspection
can still show both original anchors in either case. Set `markerTag_` on the
series to publish tagged point validities after the stored entries, in raw record
order. The view and matcher read the same records; no per-vertex table is stored.
Untagged assignments alone determine coverage. A marker outside the assigned
range can be inspected but cannot make its attribute apply at that vertex.
With markers present, an explicitly empty assignment starts the public list
with `{"direction":"NONE"}`; an omitted assignment does not. One entry is
still unwrapped as an object in GeoJSON.

When the producer already stores the positions as tagged validities, select
`TaggedValidity` instead of a raw position field. Dense/indexed layouts need
no synthesized markers.

To link a nested field to a point without repeating the geometry, append the
tagged locations with `Attribute::appendTaggedValidities` and use the returned
ordinal to create a `ValidityReference`. GeoJSON enumerates just
`{"$mapgetValidity": n}`; named lookup can follow the target. The tag describes
the point, not its identity. Even coincident points with the same tag retain
distinct indices.

An occurrence attached to another feature may refer to a different record array
inside a shared payload. Declare `branches_`, `branchTarget_`, `branchRecords_`
and `hostTarget_` for this case. One matching target selects its local records;
no match uses the primary array. The attachment has its own applicability.
If several branches match, bind the selected raw ordinal with
`attr->setPointSeriesBranch(...)`. Use `setPointSeriesGeometry(...)` for an
otherwise ambiguous geometry. A restricted clone must retain both discriminators;
neither is a new per-vertex field.

## Filtering and inspection

Open one range per host and geometry. The geometry ordinal is in the published
feature, and the visible point index is relative to that geometry (including a
sliced geometry). It is not the draw-buffer vertex or the position of an accepted row.

```cpp
auto points = feature->shapePoints(geometryOrdinal);
if (auto point = points.at(pointIndex)) {
    auto originalPosition = point->position();
    auto context = point->queryContext(); // independently retainable SIMFIL model node
}
```

- `$pointIndex`, `$geometryOrdinal` and `$attributes` are enumerable.
  `$geometry` and `$feature` are named-only: walking a point must not walk its entire host.
- `$attributes.*` enumerates covering occurrences in source order.
  `$attributes.LaneAdasLayerWithGeo.CURVATURE.*` selects occurrences by name
  without creating a second enumerable list. `$attributeIndex` and the original
  indices in `$validities` identify the attachment and its applicability rows.
- `curvature` is the physical shortcut. Zero is a measurement; a missing or
  exceptional value is not zero. Competing physical values raise a field-local
  diagnostic rather than choosing the first. Select `$samples.curvature.*`
  to inspect candidates, status/range/unit, and an optional continuation key.
- Each contributor carries its raw value, weight, raw record ordinal and a
  path of string/integer components **from the stored attribute**. For the row
  above this could be `["records", 0, "code"]` even though the original point
  index is **1**. A path is not a byte address. Use the enclosing occurrence's
  real source-data link unless the producer supplies more specific provenance.

The range retains the source and initializes lookup tables on demand. Reading
`$pointIndex` does not visit attributes. Readers reuse sparse record indexes for
shared payloads and geometry distance tables for matching measures. Each context
and retained child keeps its own point identity; advancing an iterator does not
retarget it. A caller can pass a cancellation predicate to `shapePoints`. The
filter evaluator scopes `PointQueryError::Capture` per expression, including
materialization, so failures inside SIMFIL's non-throwing callbacks become
projection diagnostics. Direct native inspection still gets a `PointQueryError`
at the failing read.

For wire row layout, source snapshot checks and picking, see the
[subset model](mapget-model.md) and the [native-to-WASM fixture](mapget-dev-guide.md#original-shape-points).
