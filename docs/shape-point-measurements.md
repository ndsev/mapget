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
the source paths, numeric operands, output types and codec references, freezes
them into the binary artifact and derives each feature's point query schema.
Layer JSON interchange uses `x-mapget.pointMeasurements.series` and `codecs`.
The same artifact drives native filtering and WASM inspection. V7 artifacts
remain readable as V7; they do not acquire serialized point declarations.

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
can still show both original anchors in either case. Set `markers_` to
`AppendToValidity` or `PointValidities` only when that public marker layout is
part of the datasource contract. `attr->validityOrNull()` is the **stored**
coverage; `attr->validityView(host, layerName)` is the lazy public view.

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
