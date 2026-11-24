#include "coverage.h"
#include "mapget/log.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace mapget
{
namespace
{
/** Validate geographic metadata before trusting any negative membership result. */
bool validBounds(CoverageBounds const& b)
{
    return std::isfinite(b.west_) && std::isfinite(b.south_) && std::isfinite(b.east_) &&
        std::isfinite(b.north_) && b.west_ >= -180 && b.west_ <= 180 &&
        b.east_ >= -180 && b.east_ <= 180 && b.south_ >= -90 && b.north_ <= 90 &&
        b.south_ <= b.north_;
}

/** Split a packed-grid interval at its geographic wrap seam. */
std::vector<std::pair<uint32_t, uint32_t>> splitInterval(uint32_t first, uint32_t last, uint32_t seam)
{
    if (seam && first < seam && last >= seam) return {{first, seam - 1}, {seam, last}};
    return {{first, last}};
}

/** Project a compact packed rectangle without confusing Greenwich with the dateline. */
void appendGridRectangle(std::vector<CoverageBounds>& output, uint32_t x0, uint32_t y0,
    uint32_t x1, uint32_t y1, int level)
{
    for (auto [left, right] : splitInterval(x0, x1, uint32_t{1} << level)) {
        for (auto [bottom, top] : splitInterval(y0, y1, level ? uint32_t{1} << (level - 1) : 0)) {
            auto a = CoverageQuery::tileBounds(TileId::fromTileXY(left, bottom, level));
            auto b = CoverageQuery::tileBounds(TileId::fromTileXY(right, top, level));
            output.push_back({a.west_, a.south_, b.east_, b.north_});
        }
    }
}

/** Move every ring into its enclosing box's longitude interval. */
ndsmath::PlanarRing unwrapRing(std::vector<std::array<double, 2>> const& input, CoverageBounds b)
{
    ndsmath::PlanarRing result;
    result.reserve(input.size());
    for (auto const& p : input) {
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || p[0] < -180 || p[0] > 180 ||
            p[1] < b.south_ || p[1] > b.north_)
            throw std::invalid_argument("Spatial extent ring contains an invalid position.");
        auto x = p[0] < b.west_ ? p[0] + 360 : p[0];
        if (x < b.west_ || x > b.east_)
            throw std::invalid_argument("Spatial extent ring lies outside its bounding box.");
        result.emplace_back(x, p[1]);
    }
    return result;
}
}

CoverageBounds CoverageTileRange::bounds() const
{
    auto size = std::ldexp(180.0, -level_);
    return {-180 + minX_ * size, -90 + minY_ * size,
        -180 + (maxX_ + 1) * size, -90 + (maxY_ + 1) * size};
}

TileId CoverageTileRange::tile() const
{
    auto b = bounds();
    return TileId::fromWgs84((b.west_ + b.east_) * .5, (b.south_ + b.north_) * .5, level_);
}

CoverageBounds CoverageQuery::tileBounds(TileId tile)
{
    auto sw = tile.southWestWgs84(), ne = tile.northEastWgs84();
    // At level zero the single packed Y cell wraps the entire latitude domain.
    return {sw.first, tile.level() ? sw.second : -90,
        ne.first, tile.level() ? ne.second : 90};
}

CoverageQuery::CoverageQuery(LayerInfo const& layer)
{
    if (layer.spatialExtent_) {
        for (auto const& extent : *layer.spatialExtent_) {
            Region region;
            region.geographic_ = true;
            try {
                auto const& v = extent.bounds_;
                CoverageBounds b{v[0], v[1], v[2], v[3]};
                if (!validBounds(b)) throw std::invalid_argument("Invalid spatial extent bounds.");
                if (b.east_ < b.west_) b.east_ += 360;
                if (extent.exterior_) {
                    region.exterior_ = unwrapRing(*extent.exterior_, b);
                    for (auto const& hole : extent.holes_)
                        region.holes_.push_back(unwrapRing(hole, b));
                    if (!ndsmath::validPolygonArea(region.exterior_, region.holes_))
                        throw std::invalid_argument("Invalid spatial extent polygon or holes.");
                    b = {region.exterior_.front().x, region.exterior_.front().y,
                        region.exterior_.front().x, region.exterior_.front().y};
                    for (auto p : region.exterior_) {
                        b.west_ = std::min(b.west_, p.x); b.east_ = std::max(b.east_, p.x);
                        b.south_ = std::min(b.south_, p.y); b.north_ = std::max(b.north_, p.y);
                    }
                }
                else if (!extent.holes_.empty())
                    throw std::invalid_argument("Spatial extent holes require an exterior.");
                region.rectangles_.push_back(b);
            }
            catch (std::exception const& error) {
                valid_ = false;
                region = {};
                log().warn("Incomplete spatial coverage for {}: {}", layer.layerId_, error.what());
            }
            regions_.push_back(std::move(region));
        }
    }
    else {
        for (auto const& coverage : layer.coverage_) {
            Region region;
            auto const a = coverage.min_, b = coverage.max_;
            if (!a.isValid() || !b.isValid() || a.level() != b.level() || a.x() > b.x() || a.y() > b.y()) {
                valid_ = false;
                regions_.push_back(std::move(region));
                continue;
            }
            auto width = uint64_t{b.x()} - a.x() + 1, height = uint64_t{b.y()} - a.y() + 1;
            if (!coverage.filled_.empty() && coverage.filled_.size() != width * height) {
                valid_ = false;
                regions_.push_back(std::move(region));
                continue;
            }
            if (coverage.filled_.empty())
                appendGridRectangle(region.rectangles_, a.x(), a.y(), b.x(), b.y(), a.level());
            else {
                // Cost is proportional to the supplied mask, never to its fine-level descendants.
                std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> active;
                for (uint32_t y = a.y(); y <= b.y() + 1; ++y) {
                    std::set<std::pair<uint32_t, uint32_t>> row;
                    if (y <= b.y()) {
                        for (uint32_t x = a.x(); x <= b.x();) {
                            if (!coverage.filled_[(y - a.y()) * width + x - a.x()]) { ++x; continue; }
                            auto first = x++;
                            while (x <= b.x() && coverage.filled_[(y - a.y()) * width + x - a.x()]) ++x;
                            row.emplace(first, x - 1);
                        }
                    }
                    for (auto it = active.begin(); it != active.end();) {
                        if (row.contains(it->first)) { ++it; continue; }
                        appendGridRectangle(region.rectangles_, it->first.first, it->second.first,
                            it->first.second, it->second.second, a.level());
                        it = active.erase(it);
                    }
                    for (auto run : row) {
                        auto [it, inserted] = active.try_emplace(run, y, y);
                        if (!inserted) it->second.second = y;
                    }
                }
            }
            regions_.push_back(std::move(region));
        }
    }
    complete_ = valid_ && layer.coverageComplete_.value_or(false) && !layer.serviceMetadata_;
}

CoverageQuery::Relation CoverageQuery::classify(CoverageBounds area, std::optional<size_t> selected) const
{
    auto result = Relation::Outside;
    auto begin = selected.value_or(0), end = selected ? begin + 1 : regions_.size();
    for (auto i = begin; i < std::min(end, regions_.size()); ++i) {
        auto const& region = regions_[i];
        for (auto const& b : region.rectangles_) {
            for (double shift : {-360., 0., 360.}) {
                auto a = area;
                a.west_ += shift; a.east_ += shift;
                auto outside = region.geographic_
                    ? a.east_ < b.west_ || a.west_ > b.east_ || a.north_ < b.south_ || a.south_ > b.north_
                    : a.east_ <= b.west_ || a.west_ >= b.east_ || a.north_ <= b.south_ || a.south_ >= b.north_;
                if (outside) continue;
                auto relation = Relation::Boundary;
                if (!region.exterior_.empty())
                    relation = ndsmath::classifyPolygonRectangle(region.exterior_, region.holes_,
                        {a.west_, a.south_}, {a.east_, a.north_});
                else if (a.west_ >= b.west_ && a.east_ <= b.east_ &&
                    a.south_ >= b.south_ && a.north_ <= b.north_)
                    relation = Relation::Inside;
                if (relation == Relation::Inside) return relation;
                if (relation == Relation::Boundary) result = relation;
            }
        }
    }
    return result;
}

std::optional<std::pair<size_t, size_t>> CoverageQuery::overlappingPolygons() const
{
    if (!valid_) return {};
    for (size_t i = 0; i < regions_.size(); ++i) {
        auto const& a = regions_[i];
        if (a.exterior_.empty()) continue;
        auto const& ab = a.rectangles_.front();
        for (size_t j = i + 1; j < regions_.size(); ++j) {
            auto const& b = regions_[j];
            if (b.exterior_.empty()) continue;
            auto const& bb = b.rectangles_.front();
            if (ab.north_ < bb.south_ || bb.north_ < ab.south_) continue;
            for (auto shift : {-360., 0., 360.}) {
                if (ab.east_ < bb.west_ + shift || bb.east_ + shift < ab.west_) continue;
                auto exterior = b.exterior_;
                auto holes = b.holes_;
                for (auto& p : exterior) p.x += shift;
                for (auto& hole : holes) for (auto& p : hole) p.x += shift;
                if (ndsmath::polygonAreasIntersect(a.exterior_, a.holes_, exterior, holes))
                    return std::pair{i, j};
            }
        }
    }
    return {};
}

bool CoverageQuery::permits(TileId tile) const
{
    return !complete_ || !tile.isValid() || classify(tileBounds(tile)) != Relation::Outside;
}

std::optional<size_t> CoverageQuery::regionForTile(TileId tile) const
{
    if (!tile.isValid()) return {};
    auto b = tileBounds(tile);
    for (size_t i = 0; i < regions_.size(); ++i)
        if (classify(b, i) != Relation::Outside) return i;
    return {};
}

std::vector<CoverageBounds> CoverageQuery::bounds(std::optional<size_t> selected, std::optional<int> displayLevel) const
{
    std::vector<CoverageBounds> result;
    if (displayLevel && (*displayLevel < 0 || *displayLevel > 15)) return result;
    auto begin = selected.value_or(0), end = selected ? begin + 1 : regions_.size();
    for (auto i = begin; i < std::min(end, regions_.size()); ++i) {
        for (auto area : regions_[i].rectangles_) {
            if (!displayLevel) { result.push_back(area); continue; }
            // Geographic boundaries are closed. Include both incident cells;
            // grid masks already describe half-open cell areas.
            if (regions_[i].geographic_) {
                const double epsilon = std::ldexp(180., -*displayLevel) * 1e-9;
                area.west_ -= epsilon; area.east_ += epsilon;
                area.south_ -= epsilon; area.north_ += epsilon;
            }
            for (auto const& range : viewportRanges(area, *displayLevel)) result.push_back(range.bounds());
        }
    }
    return result;
}

std::vector<CoverageTileRange> CoverageQuery::viewportRanges(CoverageBounds b, int level)
{
    if (level < 0 || level > 15 || !std::isfinite(b.west_) || !std::isfinite(b.east_) ||
        !std::isfinite(b.south_) || !std::isfinite(b.north_) || b.north_ <= b.south_) return {};
    if (b.east_ < b.west_) b.east_ += 360;
    auto width = std::min(360., b.east_ - b.west_);
    if (width <= 0 || b.north_ <= -90 || b.south_ >= 90) return {};
    b.west_ = std::fmod(std::fmod(b.west_ + 180., 360.) + 360., 360.) - 180.;
    b.east_ = b.west_ + width;
    auto size = std::ldexp(180., -level);
    auto nx = uint32_t{1} << (level + 1), ny = uint32_t{1} << level;
    auto lower = [&](double value, double origin, uint32_t count) {
        return static_cast<uint32_t>(std::clamp(std::floor((value - origin) / size), 0., double(count - 1)));
    };
    auto upper = [&](double value, double origin, uint32_t count) {
        return static_cast<uint32_t>(std::clamp(std::ceil((value - origin) / size) - 1, 0., double(count - 1)));
    };
    auto y0 = lower(b.south_, -90, ny), y1 = upper(b.north_, -90, ny);
    if (width >= 360) return {{0, y0, nx - 1, y1, level}};
    std::vector<CoverageTileRange> result{{lower(b.west_, -180, nx), y0,
        upper(std::min(180., b.east_), -180, nx), y1, level}};
    if (b.east_ > 180) result.push_back({0, y0, upper(b.east_ - 360, -180, nx), y1, level});
    // A wrapped viewport can touch the same coarse cell from both ends.
    if (result.size() == 2 && result[1].maxX_ >= result[0].minX_)
        return {{0, y0, nx - 1, y1, level}};
    return result;
}

std::pair<CoverageTileRange, CoverageTileRange> CoverageQuery::split(CoverageTileRange a)
{
    auto b = a;
    if (a.maxX_ - a.minX_ >= a.maxY_ - a.minY_ && a.minX_ < a.maxX_) {
        a.maxX_ = a.minX_ + (a.maxX_ - a.minX_) / 2;
        b.minX_ = a.maxX_ + 1;
    }
    else {
        a.maxY_ = a.minY_ + (a.maxY_ - a.minY_) / 2;
        b.minY_ = a.maxY_ + 1;
    }
    return {a, b};
}

std::vector<CoverageTileRange> CoverageQuery::rectangles(CoverageBounds viewport, int level) const
{
    auto pending = viewportRanges(viewport, level);
    std::vector<CoverageTileRange> result;
    while (!pending.empty()) {
        auto range = pending.back(); pending.pop_back();
        auto relation = classify(range.bounds());
        if (relation == Relation::Outside) continue;
        if (relation == Relation::Inside || (range.minX_ == range.maxX_ && range.minY_ == range.maxY_))
            result.push_back(range);
        else {
            auto [a, b] = split(range);
            pending.push_back(b); pending.push_back(a);
        }
    }
    return result;
}

bool SpatialExtentPolicy::limits(std::string const& mapId) const
{
    auto found = maps_.find(mapId);
    return found == maps_.end() ? defaultLimit_ : found->second;
}

SpatialExtentPolicy SpatialExtentPolicy::fromJson(nlohmann::json const& json)
{
    if (!json.is_object()) throw std::invalid_argument("spatialExtentPolicy must be an object.");
    return {json.value("defaultLimit", false), json.value("maps", std::map<std::string, bool>{})};
}

nlohmann::json SpatialExtentPolicy::toJson() const
{
    return {{"defaultLimit", defaultLimit_}, {"maps", maps_}};
}
}
