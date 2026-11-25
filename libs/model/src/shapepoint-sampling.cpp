#include "attrpoint.h"
#include "featurelayer.h"
#include "mapschema.h"
#include "pointseries-numeric.h"
#include "shapepoint-series.h"

#include <algorithm>
#include <cmath>

namespace mapget::shape
{
using Node = simfil::ModelNode::Ptr;
simfil::SchemaType
SeriesReader::valueType(Index const& idx, uint32_t ordinal, PointSeries::Field const& field) const
{
    // A scalar usually inherits its type from the parent; detached raw leaves
    // therefore cannot answer the unit/sentinel question by themselves.
    auto type = occurrence_->schemaType();
    auto descend = [](simfil::SchemaType parent, PointSeries::Path const& path)
    {
        for (auto const& key : path) {
            if (!parent)
                break;
            parent = parent.field(key).type();
        }
        return parent;
    };
    if (!type) {
        auto layers =
            schema_->mapSchema()->schemaType(schema_->attributeLayerMapSchema(decl_.featureType_));
        type = layers.field(decl_.attributeLayer_).type().field(decl_.attribute_).type();
    }
    if (idx.referenced) {
        type = descend(descend(type, decl_.branches_).itemType(), decl_.branchRecords_);
    }
    else
        type = descend(type, decl_.records_);
    if (auto explicitContainer = idx.records->schemaType())
        type = explicitContainer;
    if (decl_.parallel_)
        return descend(type, field.path_).itemType();
    auto record = idx.records->at(ordinal);
    auto element = record ? record->schemaType() : simfil::SchemaType{};
    return descend(element ? element : type.itemType(), field.path_);
}

SeriesReader::Sample SeriesReader::decode(
    uint32_t ordinal,
    PointSeries::Field const& field,
    RecordIndexes::Alternative const& alt) const
{
    Sample s;
    s.alternative = alt;
    s.unit = field.unit_;
    s.contributors.emplace_back(ordinal, 1.);
    select();
    if (index_.referenced && std::holds_alternative<std::monostate>(s.alternative))
        s.alternative = alternativeKey(read(axis_->host_, decl_.hostTarget_));
    auto raw = member(index_, ordinal, field.path_);
    if (!raw || raw->type() == ValueType::Null || raw->type() == ValueType::Undef)
        return s;
    // Even an explicit portable codec can inherit the unit of its typed source.
    auto type = valueType(index_, ordinal, field);
    auto domain = type ? type.numericDomain() : std::nullopt;
    if (domain && s.unit.empty())
        s.unit = domain->unit;
    if (field.codec_.empty()) {
        auto physical = number(raw);
        auto encoded = integer(raw);
        point_schema::Bound code = encoded ?
            point_schema::Bound(*encoded) :
            point_schema::Bound(physical.value_or(0.));
        // A named ordinary constant is still a measurement. A sentinel is not.
        if (type && physical) {
            type.forEachNamedValue(
                [&](auto const& named)
                {
                    if (named.role != simfil::SchemaType::NamedValue::Role::Special)
                        return;
                    auto matches = [&](auto const& namedCode)
                    {
                        using T = std::decay_t<decltype(namedCode)>;
                        if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
                            return !point_schema::less(code, namedCode) &&
                                !point_schema::less(namedCode, code);
                        return false;
                    };
                    if (named.rawValue ?
                            std::visit(matches, *named.rawValue) :
                            std::visit(matches, named.value))
                        s.status = named.name;
                });
            if (!s.status.empty())
                return s;
        }
        if (domain && physical) {
            auto ordinary = domain->ordinaryRanges;
            if (!ordinary.empty() &&
                std::none_of(
                    ordinary.begin(),
                    ordinary.end(),
                    [&](auto const& range) {
                        return !point_schema::less(code, range.min) &&
                            !point_schema::less(range.max, code);
                    }))
            {
                s.status = "outside ordinary range";
                return s;
            }
            if (domain->scale != 1. || domain->offset != 0.) {
                auto decoded = *physical * domain->scale + domain->offset;
                if (!std::isfinite(decoded))
                    throw PointQueryError("Non-finite physical measurement.");
                s.value = Context::scalar(decoded);
                return s;
            }
        }
        s.value = raw;
        return s;
    }
    auto codec = schema_->pointCodec(field.codec_);
    if (!codec)
        throw PointQueryError("Unknown point-series codec " + field.codec_);
    if (auto code = integer(raw)) {
        // Schema finalization sorts and validates exception keys.
        auto it =
            std::ranges::lower_bound(codec->codes_, *code, {}, &PointSeries::Codec::Code::raw_);
        if (it != codec->codes_.end() && it->raw_ == *code) {
            if (it->value_)
                s.value = Context::scalar(*it->value_);
            s.status = it->status_;
            s.lower = it->lower_;
            s.upper = it->upper_;
            return s;
        }
    }
    if (codec->tableOnly_) {
        s.status = "unmapped";
        return s;
    }
    if (auto v = number(raw)) {
        auto decoded = *v * codec->scale_ + codec->offset_;
        if (!std::isfinite(decoded))
            throw PointQueryError("Point-series codec produced a non-finite value.");
        s.value = Context::scalar(decoded);
    }
    else
        throw PointQueryError("Affine point codec requires a numeric source value.");
    return s;
}

std::array<double, 3> SeriesReader::taggedDistances(
    Index const& idx,
    Record const& left,
    uint32_t vertex,
    Record const& right) const
{
    auto stored = occurrence_->validityOrNull();
    auto logical = [&](Record const& r)
    {
        auto node = stored->at(idx.storedMarkerOrdinals[r.ordinal]);
        return occurrence_->model().resolve<Validity>(*node)->attrPointIndex();
    };
    auto a = logical(left), b = logical(right);
    auto measure = decl_.measure_;
    if (!a && !b)
        return {
            axis_->distanceAt(left.station, measure),
            axis_->distanceAt(vertex, measure),
            axis_->distanceAt(right.station, measure)};
    auto seq = a ? a->sequence() : b->sequence();
    if (a && b && seq->addr() != b->sequence()->addr())
        throw PointQueryError("Tagged interpolation anchors refer to different logical sequences.");
    // logicalVertex orders insertions between original vertices; that ordering is
    // not a physical distance. Measure on the sequence containing the authored point.
    auto original = [&](double station)
    {
        auto i = static_cast<uint32_t>(station);
        auto d = axis_->logicalDistanceAt(*seq, logicalIndex(*seq, i), measure);
        return station == i ?
            d :
            std::lerp(
                d,
                axis_->logicalDistanceAt(*seq, logicalIndex(*seq, i + 1), measure),
                station - i);
    };
    return {
        a ? axis_->logicalDistanceAt(*seq, a->index(), measure) : original(left.station),
        original(vertex),
        b ? axis_->logicalDistanceAt(*seq, b->index(), measure) : original(right.station)};
}

std::vector<SeriesReader::Sample> SeriesReader::samples(
    uint32_t vertex,
    PointSeries::Field const& field,
    std::span<Axis::Span const> spans) const
{
    auto const& idx = index();
    if (!idx.count)
        return {};
    auto const& pos = decl_.position_;
    auto kind = pos.kind_ == Kind::Interval ? pos.intervalAxis_ : pos.kind_;
    auto at = axisValue(vertex, kind);
    if (!std::isfinite(at))
        return {};
    if (pos.fromAssignmentStart_) {
        if (spans.size() != 1)
            throw PointQueryError("A local origin requires one unambiguous assignment.");
        auto a = axisValue(spans[0].first, kind), b = axisValue(spans[0].second, kind);
        at = pos.reverse_ ? b - at : at - a;
        if (kind == Kind::Fraction) {
            if (a == b)
                return {};
            at /= b - a;
        }
    }
    else if (pos.reverse_)
        at = axisValue(0, kind) + axisValue(axis_->last(), kind) - at;
    if (kind == Kind::Dense && !idx.table) {
        if (at < 0 || at >= idx.count || std::trunc(at) != at)
            return {};
        return {decode(static_cast<uint32_t>(at), field, {})};
    }
    std::vector<Sample> result;
    uint32_t visits = 0;
    for (auto const& [alt, continuation] : idx.table->alternatives) {
        if (!(visits++ % 128))
            axis_->check();
        auto const& group = continuation.ordered;
        if (pos.kind_ == Kind::Interval) {
            auto first = std::lower_bound(
                continuation.maxEnd.begin(),
                continuation.maxEnd.end(),
                at,
                [](double end, double p) { return end < p && !Axis::near(end, p); });
            for (size_t i = first - continuation.maxEnd.begin(); i < group.size(); ++i) {
                if (!(visits++ % 128))
                    axis_->check();
                auto const& r = group[i];
                if (r.station > at && !Axis::near(r.station, at))
                    break;
                auto side = [&](double bound, PointSeries::Path const& path)
                {
                    if (!Axis::near(at, bound))
                        return bound < at ? -1 : 1;
                    if (r.fraction.denominator && (at == 0. || at == 1.)) {
                        RecordIndexes::Fraction raw;
                        (void)station(idx, r.ordinal, path, &raw);
                        return raw.compare(at);
                    }
                    return 0;
                };
                auto lo = side(r.station, pos.field_), hi = side(r.end, pos.end_);
                if ((lo < 0 || (lo == 0 && pos.includeStart_)) &&
                    (hi > 0 || (hi == 0 && pos.includeEnd_)))
                    result.push_back(decode(r.ordinal, field, alt));
            }
            continue;
        }
        auto compare = [&](Record const& r)
        {
            if (r.fraction.denominator && Axis::near(r.station, at))
                return r.fraction.compare(at);
            return r.station < at ? -1 : r.station > at;
        };
        auto sameAnchor = [](Record const& a, Record const& b)
        {
            return a.fraction.denominator ?
                a.fraction.compare(b.fraction) == 0 :
                a.station == b.station;
        };
        auto it = std::partition_point(
            group.begin(),
            group.end(),
            [&](Record const& r) { return compare(r) < 0; });
        auto anchor = group.end();
        if (it != group.end() && compare(*it) == 0)
            anchor = it;
        else if (kind == Kind::Distance || (kind == Kind::Fraction && at > 0. && at < 1.)) {
            // Computed line lengths may round. Snap to ONE nearby encoded station
            // only; never turn two different fractions into a repeated anchor.
            bool before = it != group.begin() && Axis::near((it - 1)->station, at);
            bool after = it != group.end() && Axis::near(it->station, at);
            if (before != after)
                anchor = before ? it - 1 : it;
        }
        if (anchor != group.end()) {
            auto first = anchor;
            while (first != group.begin() && sameAnchor(*(first - 1), *anchor))
                --first;
            for (auto r = first; r != group.end() && sameAnchor(*r, *anchor); ++r) {
                if (!(visits++ % 128))
                    axis_->check();
                result.push_back(decode(r->ordinal, field, alt));
            }
            continue;
        }
        if (decl_.sampling_ != PointSeries::Sampling::Linear || it == group.begin() ||
            it == group.end())
            continue;
        auto left = it - 1;
        if ((left != group.begin() && sameAnchor(*(left - 1), *left)) ||
            (it + 1 != group.end() && sameAnchor(*(it + 1), *it)))
            throw PointQueryError("Repeated interpolation anchor requires a continuation key.");
        auto a = decode(left->ordinal, field, alt), b = decode(it->ordinal, field, alt);
        auto va = number(a.value), vb = number(b.value);
        if (!va || !vb || !a.status.empty() || !b.status.empty())
            continue;
        bool vertexKey = kind == Kind::VertexIndex || kind == Kind::LogicalIndex ||
            kind == Kind::TaggedValidity;
        auto distance = [&](double station)
        {
            if (!vertexKey)
                return station;
            // Index keys select the endpoints, not the interpolation weight: authored
            // vertices need not be evenly spaced on the road.
            if (pos.fromAssignmentStart_) {
                station = pos.reverse_ ?
                    axisValue(spans.front().second, kind) - station :
                    axisValue(spans.front().first, kind) + station;
            }
            else if (pos.reverse_)
                station = axisValue(0, kind) + axisValue(axis_->last(), kind) - station;
            return kind == Kind::LogicalIndex ?
                axis_->logicalDistanceAt(*sequence(), station, decl_.measure_) :
                axis_->distanceAt(station, decl_.measure_);
        };
        auto [da, atDistance, db] = kind == Kind::TaggedValidity && idx.hasLogicalMarkers ?
            taggedDistances(idx, *left, vertex, *it) :
            std::array{distance(left->station), distance(at), distance(it->station)};
        if (da == db) {
            if (vertexKey)
                continue;  // coincident anchors
            throw PointQueryError(
                "Interpolation anchors are closer than the geometry-measure precision.");
        }
        auto t = (atDistance - da) / (db - da);
        a.value = Context::scalar(std::lerp(*va, *vb, t));
        a.contributors = {{left->ordinal, 1 - t}, {it->ordinal, t}};
        result.push_back(std::move(a));
    }
    // Iteration over continuation keys must not reorder competing source records.
    if (result.size() > 1)
        std::ranges::stable_sort(
            result,
            {},
            [](auto const& s) { return s.contributors.front().first; });
    return result;
}

Node SeriesReader::explain(Sample const& sample, PointSeries::Field const& field) const
{
    auto strings = axis_->host_->model().strings();
    Context::Fields fields;
    auto add = [&](std::string_view name, Node value)
    {
        fields.emplace_back(strings->get(name), [value] { return value; });
    };
    add("value", sample.value ? sample.value : Context::scalar(std::monostate{}));
    if (!sample.unit.empty())
        add("unit", Context::scalar(sample.unit));
    if (!sample.status.empty())
        add("status", Context::scalar(sample.status));
    if (sample.lower)
        add("lower", Context::scalar(*sample.lower));
    if (sample.upper)
        add("upper", Context::scalar(*sample.upper));
    std::visit(
        [&](auto const& key)
        {
            if constexpr (!std::is_same_v<std::decay_t<decltype(key)>, std::monostate>)
                add("alternative", Context::scalar(key));
        },
        sample.alternative);
    std::vector<Node> contributors;
    for (auto [i, weight] : sample.contributors) {
        auto path = index_.path;
        if (!decl_.parallel_)
            path.emplace_back(i);
        for (auto const& p : field.path_)
            path.emplace_back(p);
        if (decl_.parallel_)
            path.emplace_back(i);
        std::vector<Node> parts;
        for (auto const& p : path) {
            if (auto key = std::get_if<std::string>(&p))
                parts.push_back(Context::scalar(*key));
            else
                parts.push_back(Context::scalar(int64_t(std::get<uint32_t>(p))));
        }
        auto raw = member(index_, i, field.path_);
        contributors.push_back(Context::object(
            strings,
            {{strings->get("recordIndex"),
              [i]
              {
                  return Context::scalar(int64_t(i));
              }},
             {strings->get("weight"),
              [weight]
              {
                  return Context::scalar(weight);
              }},
             {strings->get("raw"),
              [raw]
              {
                  return raw;
              }},
             {strings->get("path"),
              [pathNode = Context::array(strings, std::move(parts))]
              {
                  return pathNode;
              }}}));
    }
    add("contributors", Context::array(strings, std::move(contributors)));
    return Context::object(strings, std::move(fields));
}

uint32_t SeriesReader::markerCount() const
{
    if (decl_.position_.kind_ == Kind::TaggedValidity)
        return 0;
    select();
    return index_.count;
}

std::span<uint32_t const> SeriesReader::storedMarkerOrdinals() const
{
    if (decl_.position_.kind_ != Kind::TaggedValidity)
        return {};
    select();
    return index_.storedMarkerOrdinals;
}

std::optional<double> SeriesReader::markerStation(uint32_t i) const
{
    if (decl_.position_.kind_ == Kind::TaggedValidity)
        return {};  // The stored validity is the marker; there is no offset to synthesize.
    select();
    if (i >= index_.count)
        return {};
    auto kind = decl_.position_.kind_;
    auto station = this->station(index_, i, decl_.position_.field_);
    if (decl_.position_.fromAssignmentStart_ || decl_.position_.reverse_) {
        Axis::Span origin{0, axis_->last()};
        if (decl_.position_.fromAssignmentStart_) {
            auto validities = occurrence_->validityOrNull();
            std::optional<Axis::Span> match;
            bool hasAssignment = validities && !validities->size();
            for (uint32_t j = 0; validities && j < validities->size(); ++j) {
                auto v = occurrence_->model().resolve<Validity>(*validities->at(j));
                if (v->tag())
                    continue;
                hasAssignment = true;
                auto span =
                    axis_->coverage(*v, decl_.assignmentMeasure_.value_or(decl_.measure_), true);
                if (!span)
                    continue;
                if (match && *match != *span)
                    throw PointQueryError("Marker has more than one assignment origin.");
                match = span;
            }
            if (match)
                origin = *match;
            else if (hasAssignment || !decl_.omittedValidityCovers_)
                return {};
        }
        auto a = axisValue(origin.first, kind), b = axisValue(origin.second, kind);
        if (kind == Kind::Fraction && decl_.position_.fromAssignmentStart_)
            station *= b - a;
        if (decl_.position_.fromAssignmentStart_)
            station = decl_.position_.reverse_ ? b - station : a + station;
        else
            station = a + b - station;
    }
    return station;
}
std::optional<Point> SeriesReader::logicalMarkerPosition(uint32_t i) const
{
    if (decl_.position_.kind_ != Kind::LogicalIndex)
        return {};
    auto station = markerStation(i);
    if (!station || *station < 0 || *station >= sequence()->positionCount() ||
        std::trunc(*station) != *station)
        throw PointQueryError("A logical marker must identify a stored sequence position.");
    return sequence()->pointAt(static_cast<uint32_t>(*station));
}
std::optional<std::pair<Validity::GeometryOffsetType, double>>
SeriesReader::marker(uint32_t i) const
{
    auto at = markerStation(i);
    if (!at)
        return {};
    double station = *at;
    auto kind = decl_.position_.kind_;
    if (kind == Kind::LogicalIndex) {
        if (station < 0 || station >= sequence()->positionCount() || std::trunc(station) != station)
            throw PointQueryError("Logical marker index out of bounds.");
        return std::pair{
            Validity::BufferOffset,
            axis_->logicalVertex(*sequence(), static_cast<uint32_t>(station))};
    }
    if (kind == Kind::VertexMeasure) {
        auto const& axis = measures();
        if (axis.empty())
            return {};
        bool ascending = axis.front() <= axis.back();
        auto it = std::lower_bound(
            axis.begin(),
            axis.end(),
            station,
            [ascending](double a, double b) { return ascending ? a < b : a > b; });
        if (it == axis.end())
            return {};
        auto i = static_cast<size_t>(it - axis.begin());
        if (*it == station)
            return std::pair{Validity::BufferOffset, double(i)};
        if (!i)
            return {};
        auto vertex = double(i - 1) + (station - axis[i - 1]) / (axis[i] - axis[i - 1]);
        auto length = axis_->distanceAt(axis_->last(), decl_.measure_);
        if (!length)
            return {};
        return std::pair{
            Validity::RelativeLengthOffset,
            axis_->distanceAt(vertex, decl_.measure_) / length};
    }
    if (kind == Kind::Fraction)
        return std::pair{Validity::RelativeLengthOffset, station};
    if (kind == Kind::Distance)
        return std::pair{Validity::MetricLengthOffset, station};
    return std::pair{Validity::BufferOffset, station};
}
Node SeriesReader::markerSample(uint32_t i, PointSeries::Field const& f) const
{
    select();
    if (i >= index_.count)
        return {};
    auto branch = decl_.alternative_.empty() ?
        RecordIndexes::Alternative{} :
        alternativeKey(member(index_, i, decl_.alternative_));
    return explain(decode(i, f, branch), f);
}
}  // namespace mapget::shape
