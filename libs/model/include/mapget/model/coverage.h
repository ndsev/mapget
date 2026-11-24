#pragma once

#include "info.h"
#include <ndsmath/polygonarea.h>

namespace mapget
{
/** Geographic rectangle, optionally unwrapped past +180 longitude. */
struct CoverageBounds
{
    double west_, south_, east_, north_;
};

/** Inclusive geographic tile-grid rectangle (west-to-east, south-to-north). */
struct CoverageTileRange
{
    uint32_t minX_, minY_, maxX_, maxY_;
    int level_;

    /** Geographic bounds of all complete tiles in this range. */
    [[nodiscard]] CoverageBounds bounds() const;
    /** Return the packed identity of a one-cell range. */
    [[nodiscard]] TileId tile() const;
};

/** Immutable coverage interpretation shared by the service and browser's native adapter. */
class CoverageQuery
{
public:
    using Relation = ndsmath::PolygonAreaRelation;

    /** Validate and normalize all contributors; invalid metadata disables exclusion. */
    explicit CoverageQuery(LayerInfo const& layer);
    [[nodiscard]] bool complete() const { return complete_; }
    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] size_t regionCount() const { return regions_.size(); }
    /** Usable geometry can survive incomplete coverage; empty regions do not provide bounds. */
    [[nodiscard]] bool hasBounds() const { return hasBounds_; }
    /** First intersecting/touching polygon pair, including containment and dateline equivalence.
     * This is an optional producer check; overlaps remain valid for generic unions.
     * Bounding-box-only regions are not bounding polygons and are ignored.
     */
    [[nodiscard]] std::optional<std::pair<size_t, size_t>> overlappingPolygons() const;

    /** Admit unknown coverage and nonspatial sentinels; exclude only proven outside tiles. */
    [[nodiscard]] bool permits(TileId tile) const;
    /** Classify an area against the available geometry, independently of completeness. */
    [[nodiscard]] Relation classify(CoverageBounds area, std::optional<size_t> region = {}) const;
    /** Resolve a clicked display tile to the first original contributing region. */
    [[nodiscard]] std::optional<size_t> regionForTile(TileId tile) const;
    /** Complete occupied region bounds; independent of viewport clipping and tile capacity. */
    [[nodiscard]] std::vector<CoverageBounds> bounds(std::optional<size_t> region = {}, std::optional<int> displayLevel = {}) const;
    /** Compact, disjoint covered tile rectangles, clipped to viewport-intersecting tiles. */
    [[nodiscard]] std::vector<CoverageTileRange> rectangles(CoverageBounds viewport, int level) const;

    /** Split geographic view bounds into nonwrapping tile ranges without enumerating cells. */
    [[nodiscard]] static std::vector<CoverageTileRange> viewportRanges(CoverageBounds viewport, int level);
    /** Geographic footprint, including full latitude for the level-zero cell. */
    [[nodiscard]] static CoverageBounds tileBounds(TileId tile);
    /** Split a non-singleton tile range along its longer axis. */
    [[nodiscard]] static std::pair<CoverageTileRange, CoverageTileRange> split(CoverageTileRange range);

private:
    /** One original region, retaining polygons separately from grid cells' half-open edges. */
    struct Region
    {
        std::vector<CoverageBounds> rectangles_;
        ndsmath::PlanarRing exterior_;
        std::vector<ndsmath::PlanarRing> holes_;
        bool geographic_ = false;
    };
    std::vector<Region> regions_;
    bool valid_ = true;
    bool complete_ = false;
    bool hasBounds_ = false;
};

/** Request-scoped per-map policy; omitted policies retain legacy unrestricted behavior. */
struct SpatialExtentPolicy
{
    bool defaultLimit_ = false;
    std::map<std::string, bool> maps_;

    /** Resolve a destination map's setting, including cross-map dependencies. */
    [[nodiscard]] bool limits(std::string const& mapId) const;
    /** Parse and validate a compact policy without trusting client-supplied geometry. */
    static SpatialExtentPolicy fromJson(nlohmann::json const& json);
    /** Canonical serialization also serves as policy-dependent result identity. */
    [[nodiscard]] nlohmann::json toJson() const;
    bool operator==(SpatialExtentPolicy const&) const = default;
};
}
