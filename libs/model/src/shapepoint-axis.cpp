#include "shapepoint-axis.h"
#include "attrpoint.h"
#include "featurelayer.h"
#include "shapepoint-series.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace mapget::shape
{
namespace
{
double segmentLength(Point a, Point b, PointSeries::Measure measure)
{
    return measure == PointSeries::Measure::Wgs84Planar2D ?
        std::hypot(b.x - a.x, b.y - a.y) :
        a.geographicDistanceTo(b);
}
double atStation(std::vector<double> const& table, double index)
{
    if (table.empty() || !std::isfinite(index) || index < 0 || index > table.size() - 1)
        throw PointQueryError("Invalid geometry station.");
    auto i = static_cast<size_t>(index);
    return i + 1 == table.size() ?
        table.back() :
        std::lerp(table[i], table[i + 1], index - double(i));
}
}  // namespace

Axis::Axis(model_ptr<Feature> host, uint32_t ordinal, std::function<bool()> cancelled)
    : host_(std::move(host)),
      ordinal_(ordinal),
      cancelled_(std::move(cancelled)),
      indexes(std::make_shared<RecordIndexes>())
{
    if (auto collection = host_->geomOrNull()) {
        if (auto node = collection->mergedAt(ordinal)) {
            auto geometry = host_->model().resolve<Geometry>(*node);
            // Bounds and GLTF node indices are encoded in the point buffer,
            // but neither is an authored geometry vertex.
            if (geometry->geomType() != GeomType::AABB &&
                geometry->geomType() != GeomType::GltfNodeIndex)
                geometry_ = geometry;
        }
    }
}

void Axis::check() const
{
    if (cancelled_ && cancelled_())
        throw PointQueryError("Shape-point evaluation cancelled.");
}

bool Axis::owns(FeatureId const& ref) const
{
    auto id = host_->id();
    if (id->borrowedModel() == ref.borrowedModel() && id->addr() == ref.addr())
        return true;
    return ref.typeId() == id->typeId() && ref.mapId() == id->mapId() &&
        ref.keyValuePairs() == id->keyValuePairs();
}

bool Axis::near(double a, double b)
{
    return std::abs(a - b) <=
        8 * std::numeric_limits<double>::epsilon() * std::max({1., std::abs(a), std::abs(b)});
}

void Axis::inventoryGeometries() const
{
    std::call_once(
        catalogued_,
        [&]
        {
            uint32_t ordinal = 0, sameType = 0, sameName = 0;
            std::optional<uint32_t> firstLine, unnamedLine;
            auto name = geometry_->name();
            host_->geomOrNull()->forEachGeometry(
                [&](auto const& g)
                {
                    if (!(ordinal % 128))
                        check();
                    if (g->geomType() == geometry_->geomType()) {
                        ++sameType;
                        if (g->name() == name)
                            ++sameName;
                    }
                    if (g->geomType() == GeomType::Line) {
                        if (!firstLine)
                            firstLine = ordinal;
                        if (!unnamedLine && !g->name())
                            unnamedLine = ordinal;
                    }
                    ++ordinal;
                    return true;
                });
            sameType_ = sameType;
            sameTypeAndName_ = sameName;
            fallback_ = (unnamedLine ? unnamedLine : firstLine) == ordinal_;
        });
}

bool Axis::selects(PointSeries const& decl, Attribute const& attr) const
{
    if (geometry_->geomType() != decl.geometryType_ ||
        (decl.geometryName_ && geometry_->name() != decl.geometryName_))
        return false;
    if (auto selected = attr.pointSeriesGeometry())
        return *selected == ordinal_;
    inventoryGeometries();
    if ((decl.geometryName_ ? sameTypeAndName_ : sameType_) == 1)
        return true;

    // Only original host references can resolve a non-unique declaration.
    // Lengths, coordinates and the order in which matching lines appeared cannot.
    auto valid = attr.validityOrNull();
    std::optional<uint32_t> chosen;
    for (uint32_t i = 0; valid && i < valid->size(); ++i) {
        if (!(i % 128))
            check();
        auto v = attr.model().resolve<Validity>(*valid->at(i));
        if (v->tag())
            continue;  // Value geometries cannot choose the assignment geometry.
        if (auto ref = v->featureId(); ref && !owns(*ref))
            continue;
        auto p = v->attrPointIndex();
        auto r = v->attrPointIndexRange();
        auto seq = p ? p->sequence() : (r ? r->sequence() : model_ptr<AttrPointSequence>{});
        auto target = v->simpleGeometry();
        auto name = v->geometryName();
        if (!seq && !target && !name)
            continue;
        if (seq && !owns(*seq->featureId()))
            continue;
        uint32_t g = 0;
        host_->geomOrNull()->forEachGeometry(
            [&](auto const& candidate)
            {
                if (candidate->geomType() == decl.geometryType_ &&
                    (!decl.geometryName_ || candidate->name() == decl.geometryName_) &&
                    (!seq || seq->geometryIndex() == g) && (!name || candidate->name() == name) &&
                    (!target ||
                     (target->borrowedModel() == candidate->borrowedModel() &&
                      target->addr() == candidate->addr())))
                {
                    if (chosen && *chosen != g)
                        throw PointQueryError(
                            "Point series selects multiple geometries on this feature.");
                    chosen = g;
                }
                ++g;
                return true;
            });
    }
    if (!chosen)
        throw PointQueryError("Point series selects multiple geometries on this feature.");
    return *chosen == ordinal_;
}

bool Axis::fallbackGeometry() const
{
    inventoryGeometries();
    return fallback_;
}

std::vector<double> const& Axis::lengths(PointSeries::Measure measure) const
{
    auto& table = lengths_[size_t(measure)];
    std::call_once(
        table.built,
        [&]
        {
            auto count = geometry_->numPoints();
            std::vector<double> values(count);
            for (size_t i = 1; i < count; ++i) {
                if (!(i % 256))
                    check();
                auto a = geometry_->pointAt(i - 1), b = geometry_->pointAt(i);
                values[i] = values[i - 1] + segmentLength(a, b, measure);
            }
            table.values = std::move(values);
            ++distanceTables;
        });
    return table.values;
}

double Axis::distanceAt(double index, PointSeries::Measure measure) const
{
    return atStation(lengths(measure), index);
}

double
Axis::logicalDistanceAt(AttrPointSequence const& seq, double logical, PointSeries::Measure measure)
    const
{
    if (!seq.attrPointCount())
        return distanceAt(logical, measure);
    std::shared_ptr<Lengths> table;
    {
        std::lock_guard guard(logicalMutex_);
        auto& found = logicalLengths_[{seq.addr().value_, measure}];
        if (!found)
            found = std::make_shared<Lengths>();
        table = found;
    }
    std::call_once(
        table->built,
        [&]
        {
            std::vector<double> values(seq.positionCount());
            auto points = seq.attrPoints();
            uint32_t cursor = 0, original = 0;
            Point previous;
            auto inserted = points->attrPointAt(0);
            for (uint32_t i = 0; i < values.size(); ++i) {
                if (!(i % 256))
                    check();
                Point p;
                if (inserted && inserted->index() == i) {
                    p = inserted->point();
                    inserted = ++cursor < seq.attrPointCount() ?
                        points->attrPointAt(cursor) :
                        model_ptr<AttrPoint>{};
                }
                else
                    p = geometry_->pointAt(original++);
                if (i)
                    values[i] = values[i - 1] + segmentLength(previous, p, measure);
                previous = p;
            }
            table->values = std::move(values);
            ++distanceTables;
        });
    return atStation(table->values, logical);
}

std::optional<double> Axis::vertexAt(double distance, PointSeries::Measure measure) const
{
    auto const& table = lengths(measure);
    if (table.empty() || !std::isfinite(distance) || distance < 0 || distance > table.back())
        return {};
    if (distance == 0.)
        return 0.;
    if (distance == table.back())
        return last();
    auto it = std::lower_bound(table.begin(), table.end(), distance);
    size_t hi = it - table.begin();
    if (*it == distance)
        return double(hi);
    return double(hi - 1) + (distance - table[hi - 1]) / (table[hi] - table[hi - 1]);
}

double Axis::logicalVertex(AttrPointSequence const& sequence, uint32_t logical) const
{
    if (sequence.geometryIndex() != ordinal_ || !owns(*sequence.featureId()) ||
        logical >= sequence.positionCount())
        throw PointQueryError("Logical index references a different point sequence.");
    auto points = sequence.attrPoints();
    uint32_t lo = 0, hi = sequence.attrPointCount();
    while (lo < hi) {
        auto mid = lo + (hi - lo) / 2;
        if (points->attrPointAt(mid)->index() < logical)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == sequence.attrPointCount() || points->attrPointAt(lo)->index() != logical)
        return double(logical - lo);
    // Insertions have no original ordinal. Distribute a run strictly between its
    // original neighbours to preserve bounds without turning it into a vertex.
    auto next = lo, prior = lo;
    while (next + 1 < sequence.attrPointCount() &&
           points->attrPointAt(next + 1)->index() == points->attrPointAt(next)->index() + 1)
        ++next;
    while (prior &&
           points->attrPointAt(prior - 1)->index() + 1 == points->attrPointAt(prior)->index())
        --prior;
    return double(logical - lo) - 1. + double(lo - prior + 1) / double(next - prior + 2);
}

std::optional<double> Axis::project(Point p) const
{
    std::optional<double> result;
    double nearest = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i + 1 < geometry_->numPoints(); ++i) {
        if (!(i % 256))
            check();
        auto a = geometry_->pointAt(i), b = geometry_->pointAt(i + 1);
        auto dx = b.x - a.x, dy = b.y - a.y, px = p.x - a.x, py = p.y - a.y;
        auto d = dx * dx + dy * dy;
        if (!d)
            continue;
        auto t = (px * dx + py * dy) / d;
        auto score = px * px + py * py;
        if (t >= 0 && t <= 1 && score < nearest) {
            result = double(i) + t;
            nearest = score;
        }
    }
    return result;
}

std::optional<Axis::Span>
Axis::coverage(Validity const& v, PointSeries::Measure measure, bool declared) const
{
    if (auto ref = v.featureId(); ref && !owns(*ref))
        return {};
    if (auto name = v.geometryName(); name && geometry_->name() != name)
        return {};
    auto kind = v.geometryDescriptionType();
    if (kind == Validity::FeatureTransition || v.direction() == Validity::None)
        return {};
    if (kind == Validity::SimpleGeometry) {
        auto g = v.simpleGeometry();
        if (!g || g->borrowedModel() != geometry_->borrowedModel() ||
            g->addr() != geometry_->addr())
            return {};
        return Span{0, last()};
    }
    if (kind == Validity::AttrPointIndexValidity || kind == Validity::AttrPointIndexRangeValidity) {
        auto p = v.attrPointIndex();
        auto r = v.attrPointIndexRange();
        auto seq = p ? p->sequence() : r->sequence();
        if (!seq || seq->geometryIndex() != ordinal_ || !owns(*seq->featureId()))
            return {};
        return Span{
            logicalVertex(*seq, p ? p->index() : r->start()),
            logicalVertex(*seq, p ? p->index() : r->end())};
    }
    if (!declared && !v.geometryName() && !fallbackGeometry())
        return {};
    if (kind == Validity::NoGeometry)
        return Span{0, last()};
    auto offset = [&](Point p) -> std::optional<double>
    {
        switch (v.geometryOffsetType()) {
        case Validity::BufferOffset:
            return p.x >= 0 && p.x <= last() ? std::optional(p.x) : std::nullopt;
        case Validity::MetricLengthOffset:
            return vertexAt(p.x, PointSeries::Measure::GeographicMetres);
        case Validity::RelativeLengthOffset:
            if (p.x == 0)
                return 0.;
            if (p.x == 1)
                return last();
            if (p.x < 0 || p.x > 1 || !distanceAt(last(), measure))
                return {};
            return vertexAt(p.x * distanceAt(last(), measure), measure);
        case Validity::GeoPosOffset: return project(p);
        default: return {};
        }
    };
    if (auto p = v.offsetPoint()) {
        if (auto i = offset(*p))
            return Span{*i, *i};
    }
    else if (auto r = v.offsetRange()) {
        auto a = offset(r->first), b = offset(r->second);
        if (a && b)
            return std::minmax(*a, *b);
    }
    return {};
}

std::span<uint32_t const> RecordIndexes::verticesAt(Axis const& axis, Point p)
{
    auto finite = [](Point q)
    {
        return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
    };
    if (!finite(p))
        return {};
    std::call_once(
        verticesBuilt_,
        [&]
        {
            decltype(verticesByPosition_) positions;
            auto vertices = model_ptr<PointBufferNode>::make(
                axis.geometry_->owningModel(),
                axis.geometry_->addr());
            for (uint32_t i = 0; i < vertices->size(); ++i) {
                if (!(i % 256))
                    axis.check();
                auto q = vertices->pointAt(i);
                if (finite(q))
                    positions[{q.x, q.y, q.z}].push_back(i);
            }
            verticesByPosition_ = std::move(positions);
        });
    auto it = verticesByPosition_.find({p.x, p.y, p.z});
    if (it == verticesByPosition_.end())
        return {};
    return it->second;
}

void SeriesReader::forEachStoredStation(
    Index const& idx,
    std::function<void(uint32_t, double)> const& emit) const
{
    auto const& geometry = axis_->geometry_;
    if (idx.storedMarkerOrdinals.empty() || !geometry || !geometry->numPoints())
        return;

    // Native line offsets resolve the first named line or the default line, not
    // necessarily the pinned series geometry. A standalone point is not a line offset.
    auto name = geometry->name();
    std::optional<uint32_t> firstNamedLine;
    std::set<RecordIndexes::NodeKey> otherPointGeometries;
    uint32_t ordinal = 0;
    axis_->host_->geomOrNull()->forEachGeometry(
        [&](auto const& g)
        {
            if (!(ordinal % 128))
                axis_->check();
            if (!firstNamedLine && g->geomType() == GeomType::Line && g->name() == name)
                firstNamedLine = ordinal;
            if (ordinal != axis_->ordinal_ && g->geomType() == GeomType::Points)
                otherPointGeometries.emplace(g->borrowedModel(), g->addr().value_);
            ++ordinal;
            return true;
        });
    auto vertices = model_ptr<PointBufferNode>::make(geometry->owningModel(), geometry->addr());
    auto stored = occurrence_->validityOrNull();
    for (uint32_t i = 0; i < idx.count; ++i) {
        if (!(i % 128))
            axis_->check();
        auto v = occurrence_->model().resolve<Validity>(*stored->at(idx.storedMarkerOrdinals[i]));
        auto referencedName = v->geometryName();
        if ((referencedName && referencedName != name) || v->direction() == Validity::None)
            continue;
        if (auto ref = v->featureId(); ref && !axis_->owns(*ref))
            continue;
        if (auto g = v->simpleGeometry()) {
            if (g->geomType() != GeomType::Points || g->numPoints() != 1)
                continue;
            if (g->borrowedModel() == geometry->borrowedModel() && g->addr() == geometry->addr()) {
                emit(i, 0.);
                continue;
            }
            if (g->addr().column() == PartitionFeatureLayer::ColumnId::GeometryViews) {
                // A vertex slice has identity even if another vertex has identical XYZ.
                auto slice = model_ptr<PointBufferNode>::make(g->owningModel(), g->addr());
                if (g->borrowedModel() == geometry->borrowedModel() &&
                    slice->baseGeometryAddress() == vertices->baseGeometryAddress())
                {
                    auto vertex = std::get<int64_t>(slice->at(0)->runtimeData()) -
                        std::get<int64_t>(vertices->at(0)->runtimeData());
                    if (vertex >= 0 && vertex < vertices->size())
                        emit(i, double(vertex));
                }
            }
            else if (!otherPointGeometries.contains({g->borrowedModel(), g->addr().value_})) {
                // A detached absolute point (e.g. a house) does not request projection.
                for (auto vertex : axis_->indexes->verticesAt(*axis_, g->pointAt(0)))
                    emit(i, vertex);
            }
            continue;
        }
        if (v->geometryDescriptionType() == Validity::AttrPointIndexValidity) {
            if (auto span = axis_->coverage(*v, decl_.measure_, true))
                emit(i, span->first);
            continue;
        }
        auto p = v->offsetPoint();
        if (!p || geometry->geomType() != GeomType::Line ||
            !(referencedName ? firstNamedLine == axis_->ordinal_ : axis_->fallbackGeometry()))
            continue;
        switch (v->geometryOffsetType()) {
        case Validity::BufferOffset:
            if (p->x >= 0 && p->x < vertices->size())
                emit(i, std::trunc(p->x));  // Native buffer offsets address an original vertex.
            break;
        case Validity::MetricLengthOffset:
        case Validity::RelativeLengthOffset: {
            // Stored validity lengths are geographic metres, even when interpolation
            // between the resulting anchors uses the producer's planar measure.
            constexpr auto metres = PointSeries::Measure::GeographicMetres;
            auto length = axis_->distanceAt(axis_->last(), metres);
            auto distance = v->geometryOffsetType() == Validity::RelativeLengthOffset ?
                std::clamp(p->x, 0., 1.) * length :
                std::clamp(p->x, 0., length);
            if (auto vertex = axis_->vertexAt(distance, metres))
                emit(i, *vertex);
            break;
        }
        case Validity::GeoPosOffset:
            // Unlike SimpleGeometry, a geographic line offset explicitly requests projection.
            if (auto span = axis_->coverage(*v, decl_.measure_, false))
                emit(i, span->first);
            break;
        default: break;
        }
    }
}
}  // namespace mapget::shape
