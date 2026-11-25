#include "shapepoint-series.h"
#include "attrpoint.h"
#include "featurelayer.h"
#include "mapschema.h"

#include <fmt/core.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace mapget::shape
{
using Node = simfil::ModelNode::Ptr;
std::optional<double> SeriesReader::number(Node const& n)
{
    if (!n)
        return {};
    auto value = n->value();
    if (auto p = std::get_if<int64_t>(&value))
        return static_cast<double>(*p);
    if (auto p = std::get_if<double>(&value); p && std::isfinite(*p))
        return *p;
    return {};
}
std::optional<int64_t> SeriesReader::integer(Node const& n)
{
    if (!n)
        return {};
    auto value = n->value();
    if (auto p = std::get_if<int64_t>(&value))
        return *p;
    return {};
}
RecordIndexes::Alternative SeriesReader::alternativeKey(Node const& n)
{
    if (!n)
        throw PointQueryError("Missing point-series alternative key.");
    if (n->type() == ValueType::String) {
        auto val = n->value();
        if (auto text = std::get_if<std::string>(&val))
            return *text;
        return std::string(std::get<std::string_view>(val));
    }
    if (auto value = integer(n))
        return *value;
    throw PointQueryError("Point-series alternative must be a scalar key.");
}

int RecordIndexes::Fraction::compare(Fraction const& other) const
{
    auto a = numerator, b = denominator, c = other.numerator, d = other.denominator;
    if (b == d)
        return a < c ? -1 : a > c;
    if (b <= UINT32_MAX && d <= UINT32_MAX)
        return a * d < c * b ? -1 : a * d > c * b;
    // Continued fractions avoid overflow without a platform-specific 128-bit integer.
    int sign = 1;
    for (;;) {
        if (a / b != c / d)
            return a / b < c / d ? -sign : sign;
        a %= b;
        c %= d;
        if (!a || !c)
            return !a && !c ? 0 : (!a ? -sign : sign);
        std::swap(a, b);
        std::swap(c, d);
        sign = -sign;
    }
}

int RecordIndexes::Fraction::compare(double at) const
{
    // Compare the real p/q to an exact binary64 vertex coordinate without first
    // rounding p and q. Important at an endpoint: 1 - 1/(2^63-1) is not 1.
    if (at <= 0.)
        return at == 0. && numerator == 0 ? 0 : 1;
    if (at >= 1.)
        return at == 1. && numerator == denominator ? 0 : -1;
    if (numerator == denominator)
        return 1;
    uint64_t remainder = numerator;
    do {
        remainder *= 2;  // source widths <= 63, so this cannot overflow
        at *= 2.;
        bool sourceBit = remainder >= denominator, vertexBit = at >= 1.;
        if (sourceBit != vertexBit)
            return sourceBit ? 1 : -1;
        if (sourceBit) {
            remainder -= denominator;
            at -= 1.;
        }
    } while (remainder && at);
    return remainder ? 1 : at ? -1 : 0;
}

SeriesReader::SeriesReader(
    std::shared_ptr<Axis> axis,
    model_ptr<Attribute> occurrence,
    PointSeries const& decl,
    LayerSchema::Ptr schema)
    : axis_(std::move(axis)),
      occurrence_(std::move(occurrence)),
      decl_(decl),
      schema_(std::move(schema))
{
    auto bind = [&](PointSeries::Path const& path)
    {
        auto& ids = paths_[&path];
        for (auto const& key : path)
            ids.push_back(occurrence_->model().strings()->get(key));
    };
    for (auto path :
         {&decl.records_,
          &decl.branches_,
          &decl.hostTarget_,
          &decl.branchTarget_,
          &decl.branchRecords_,
          &decl.alternative_,
          &decl.position_.field_,
          &decl.position_.vertexMeasures_,
          &decl.position_.end_,
          &decl.position_.bits_,
          &decl.position_.bitsFromAttribute_})
        bind(*path);
    for (auto const& f : decl.fields_)
        bind(f.path_);
}

Node SeriesReader::read(Node n, PointSeries::Path const& path) const
{
    for (auto id : paths_.at(&path)) {
        if (!n)
            return {};
        n = id ? n->get(id) : Node{};
    }
    return n;
}
void SeriesReader::select() const
{
    std::call_once(
        selected_,
        [&]
        {
            Index selection;
            selection.records = read(occurrence_, decl_.records_);
            for (auto const& p : decl_.records_)
                selection.path.emplace_back(p);
            if (!decl_.branches_.empty()) {
                auto branches = read(occurrence_, decl_.branches_);
                auto host = read(axis_->host_, decl_.hostTarget_);
                if (branches && branches->type() != ValueType::Array)
                    throw PointQueryError("Point-series branches are not an array.");
                auto pinned = occurrence_->pointSeriesBranch();
                if (pinned && (!branches || *pinned >= branches->size()))
                    throw PointQueryError("Stored point-series branch is out of bounds.");
                auto end = pinned ? *pinned + 1 : (branches ? branches->size() : 0);
                for (uint32_t i = pinned.value_or(0); i < end; ++i) {
                    if (!(i % 256))
                        axis_->check();
                    auto branch = branches->at(i), key = read(branch, decl_.branchTarget_);
                    if (!key || !host || key->type() != host->type() ||
                        alternativeKey(key) != alternativeKey(host))
                        continue;
                    if (selection.referenced)
                        throw PointQueryError("Multiple point-series branches match the host.");
                    selection.referenced = true;
                    selection.records = read(branch, decl_.branchRecords_);
                    selection.path.assign(decl_.branches_.begin(), decl_.branches_.end());
                    selection.path.emplace_back(i);
                    for (auto const& p : decl_.branchRecords_)
                        selection.path.emplace_back(p);
                }
                if (pinned && !selection.referenced)
                    throw PointQueryError(
                        "Stored point-series branch does not identify this host.");
            }
            auto& idx = selection;
            if (idx.records && idx.records->type() != ValueType::Null) {
                std::optional<uint32_t> count;
                if (!decl_.parallel_) {
                    if (idx.records->type() != ValueType::Array)
                        throw PointQueryError("Point-series records are not an array.");
                    count = idx.records->size();
                }
                else {
                    auto column = [&](PointSeries::Path const& path)
                    {
                        if (path.empty())
                            return;
                        auto col = read(idx.records, path);
                        if (!col || col->type() != ValueType::Array ||
                            (count && *count != col->size()))
                            throw PointQueryError(
                                "Point-series parallel arrays are missing or have unequal "
                                "lengths.");
                        count = col->size();
                    };
                    column(decl_.position_.field_);
                    column(decl_.position_.bits_);
                    column(decl_.position_.end_);
                    column(decl_.alternative_);
                    for (auto const& field : decl_.fields_)
                        column(field.path_);
                }
                idx.count = count.value_or(0);
                if (decl_.position_.kind_ == Kind::Dense &&
                    idx.count != axis_->geometry_->numPoints())
                    throw PointQueryError(
                        "Dense point series does not match geometry vertex count.");
                if (decl_.position_.kind_ == Kind::TaggedValidity) {
                    auto stored = occurrence_->validityOrNull();
                    for (uint32_t i = 0; stored && i < stored->size(); ++i) {
                        if (!(i % 128))
                            axis_->check();
                        auto validity = occurrence_->model().resolve<Validity>(*stored->at(i));
                        if (validity->tag() == decl_.markerTag_) {
                            idx.storedMarkerOrdinals.push_back(i);
                            if (validity->geometryDescriptionType() ==
                                Validity::AttrPointIndexValidity)
                                idx.hasLogicalMarkers = true;
                        }
                    }
                    if (idx.count != idx.storedMarkerOrdinals.size())
                        throw PointQueryError(fmt::format(
                            "Point series has {} records but {} stored validities tagged '{}'.",
                            idx.count,
                            idx.storedMarkerOrdinals.size(),
                            decl_.markerTag_));
                }
            }
            index_ = std::move(selection);
        });
}
Node SeriesReader::member(Index const& index, uint32_t i, PointSeries::Path const& path) const
{
    if (!index.records || i >= index.count)
        return {};
    if (decl_.parallel_) {
        auto col = read(index.records, path);
        return col ? col->at(i) : Node{};
    }
    return read(index.records->at(i), path);
}

double SeriesReader::station(
    Index const& index,
    uint32_t i,
    PointSeries::Path const& path,
    RecordIndexes::Fraction* fraction) const
{
    auto const& pos = decl_.position_;
    auto kind = pos.kind_ == Kind::Interval ? pos.intervalAxis_ : pos.kind_;
    if (kind == Kind::Dense)
        return i;
    auto raw = member(index, i, path);
    if (kind == Kind::VertexMeasure) {
        if (auto value = number(raw))
            return *value;
        throw PointQueryError("Invalid source geometry-measure station.");
    }
    if (kind == Kind::Distance) {
        auto v = number(raw);
        if (v && *v >= 0 && std::isfinite(*v * pos.distanceScale_))
            return *v * pos.distanceScale_;
        throw PointQueryError("Invalid metric station in point series.");
    }
    auto v = integer(raw);
    if (!v || *v < 0)
        throw PointQueryError("Invalid index/numerator in point series.");
    if (kind == Kind::VertexIndex) {
        if (static_cast<uint64_t>(*v) >= axis_->geometry_->numPoints())
            throw PointQueryError("Point-series vertex index out of bounds.");
        return static_cast<double>(*v);
    }
    if (kind == Kind::LogicalIndex) {
        if (static_cast<uint64_t>(*v) >= sequence()->positionCount())
            throw PointQueryError("Point-series logical index out of bounds.");
        return static_cast<double>(*v);
    }
    auto bits = integer(
        pos.bitsFromAttribute_.empty() ?
            member(index, i, pos.bits_) :
            read(occurrence_, pos.bitsFromAttribute_));
    if (!bits || *bits < 1 || *bits > 63)
        throw PointQueryError("Fractional point position has an invalid width.");
    auto den = (uint64_t{1} << *bits) - 1;
    if (static_cast<uint64_t>(*v) > den)
        throw PointQueryError("Point fraction exceeds its encoded width.");
    if (fraction)
        *fraction = {uint64_t(*v), den};
    return static_cast<double>(*v) / static_cast<double>(den);
}

SeriesReader::Index const& SeriesReader::index() const
{
    select();
    std::call_once(
        indexed_,
        [&]
        {
            auto& idx = index_;
            if (!idx.records || idx.records->type() == ValueType::Null)
                return;  // inactive choice arm
            if (decl_.position_.kind_ == Kind::Dense && decl_.alternative_.empty())
                return;
            auto width = decl_.position_.bitsFromAttribute_.empty() ?
                0 :
                integer(read(occurrence_, decl_.position_.bitsFromAttribute_)).value_or(-1);
            auto logical = decl_.position_.kind_ == Kind::LogicalIndex ||
                (decl_.position_.kind_ == Kind::Interval &&
                 decl_.position_.intervalAxis_ == Kind::LogicalIndex);
            auto tagged = decl_.position_.kind_ == Kind::TaggedValidity;
            // A shared raw array does not imply shared logical sequences or stored anchors.
            RecordIndexes::NodeKey owner{};
            if (logical || tagged)
                owner = {occurrence_->borrowedModel(), occurrence_->addr().value_};
            auto table = axis_->indexes->get(
                {{idx.records->borrowedModel(), idx.records->addr().value_}, &decl_, width, owner});
            std::call_once(
                table->built,
                [&]
                {
                    decltype(table->alternatives) alternatives;
                    auto append =
                        [&](uint32_t i, double start, double end, RecordIndexes::Fraction fraction)
                    {
                        auto branch = decl_.alternative_.empty() ?
                            RecordIndexes::Alternative{} :
                            alternativeKey(member(idx, i, decl_.alternative_));
                        alternatives[std::move(branch)]
                            .ordered.push_back({start, end, i, fraction});
                    };
                    if (tagged) {
                        forEachStoredStation(
                            idx,
                            [&](uint32_t i, double vertex) { append(i, vertex, vertex, {}); });
                    }
                    else {
                        for (uint32_t i = 0; i < idx.count; ++i) {
                            if (!(i % 128))
                                axis_->check();
                            RecordIndexes::Fraction fraction, endFraction;
                            auto start = station(idx, i, decl_.position_.field_, &fraction);
                            auto end = decl_.position_.kind_ == Kind::Interval ?
                                station(idx, i, decl_.position_.end_, &endFraction) :
                                start;
                            if (endFraction.denominator ?
                                    fraction.compare(endFraction) > 0 :
                                    end < start)
                                throw PointQueryError("Point interval has reversed bounds.");
                            append(i, start, end, fraction);
                        }
                    }
                    uint32_t work = 0;
                    for (auto& [key, continuation] : alternatives) {
                        auto& records = continuation.ordered;
                        auto before = [&](Record const& a, Record const& b)
                        {
                            if (!(++work % 1024))
                                axis_->check();
                            return a.fraction.denominator ?
                                a.fraction.compare(b.fraction) < 0 :
                                a.station < b.station;
                        };
                        if (!std::ranges::is_sorted(records, before)) {
                            if (decl_.sampling_ == PointSeries::Sampling::Linear &&
                                !std::is_sorted(records.rbegin(), records.rend(), before))
                                throw PointQueryError(
                                    "Linear point records are not monotone within their "
                                    "continuation.");
                            std::stable_sort(records.begin(), records.end(), before);
                        }
                        if (decl_.position_.kind_ == Kind::Interval) {
                            auto end = -std::numeric_limits<double>::infinity();
                            continuation.maxEnd.reserve(records.size());
                            for (auto const& r : records) {
                                if (!(++work % 1024))
                                    axis_->check();
                                end = std::max(end, r.end);
                                continuation.maxEnd.push_back(end);
                            }
                        }
                    }
                    table->alternatives = std::move(alternatives);
                    ++axis_->recordIndexes;
                    axis_->recordsIndexed += idx.count;
                });
            idx.table = std::move(table);
        });
    return index_;
}

model_ptr<AttrPointSequence> const& SeriesReader::sequence() const
{
    std::call_once(
        sequenceBound_,
        [&]
        {
            auto valid = occurrence_->validityOrNull();
            model_ptr<AttrPointSequence> selected;
            for (uint32_t j = 0; valid && j < valid->size(); ++j) {
                if (!(j % 128))
                    axis_->check();
                auto validity = occurrence_->model().resolve<Validity>(*valid->at(j));
                auto p = validity->attrPointIndex();
                auto r = validity->attrPointIndexRange();
                auto seq = p ? p->sequence() : (r ? r->sequence() : model_ptr<AttrPointSequence>{});
                if (!seq || seq->geometryIndex() != axis_->ordinal_ ||
                    !axis_->owns(*seq->featureId()))
                    continue;
                if (selected && selected->addr() != seq->addr())
                    throw PointQueryError(
                        "More than one logical sequence matches this attachment.");
                selected = seq;
            }
            if (!selected)
                throw PointQueryError(
                    "Logical point sample has no matching attribute-point sequence.");
            sequence_ = std::move(selected);
        });
    return sequence_;
}

std::vector<double> const& SeriesReader::measures() const
{
    std::call_once(
        measured_,
        [&]
        {
            auto axis = read(occurrence_, decl_.position_.vertexMeasures_);
            if (!axis || axis->type() != ValueType::Array ||
                axis->size() != axis_->geometry_->numPoints())
                throw PointQueryError("Vertex measure array must match the original geometry.");
            std::vector<double> values;
            values.reserve(axis->size());
            for (uint32_t i = 0; i < axis->size(); ++i) {
                if (!(i % 256))
                    axis_->check();
                auto value = number(axis->at(i));
                if (!value)
                    throw PointQueryError("Invalid source geometry measure.");
                if (!values.empty() &&
                    (*value == values.back() ||
                     (i > 1 && ((*value > values.back()) != (values[1] > values[0])))))
                    throw PointQueryError("Geometry measures must be strictly monotone.");
                values.push_back(*value);
            }
            measuredAxis_ = std::move(values);
        });
    return measuredAxis_;
}

double SeriesReader::logicalIndex(AttrPointSequence const& seq, uint32_t original)
{
    auto inserted = seq.attrPoints();
    // Insertion i precedes original vertex (logicalIndex[i] - i).
    uint32_t lo = 0, hi = seq.attrPointCount();
    while (lo < hi) {
        auto mid = lo + (hi - lo) / 2;
        if (inserted->attrPointAt(mid)->index() - mid <= original)
            lo = mid + 1;
        else
            hi = mid;
    }
    return double(original) + lo;
}

double SeriesReader::axisValue(double vertex, Kind kind) const
{
    if (kind == Kind::LogicalIndex) {
        auto const& seq = *sequence();
        auto i = static_cast<uint32_t>(vertex);
        auto a = logicalIndex(seq, i);
        return vertex == i ? a : std::lerp(a, logicalIndex(seq, i + 1), vertex - i);
    }
    if (kind == Kind::VertexMeasure) {
        auto const& values = measures();
        auto i = static_cast<size_t>(vertex);
        return i + 1 >= values.size() ?
            values.at(i) :
            std::lerp(values[i], values[i + 1], vertex - double(i));
    }
    if (kind == Kind::Fraction) {
        auto total = axis_->distanceAt(axis_->last(), decl_.measure_);
        if (total == 0)
            return std::numeric_limits<double>::quiet_NaN();
        return axis_->distanceAt(vertex, decl_.measure_) / total;
    }
    if (kind == Kind::Distance)
        return axis_->distanceAt(vertex, PointSeries::Measure::GeographicMetres);
    return vertex;
}

}  // namespace mapget::shape
