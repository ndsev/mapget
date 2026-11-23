#pragma once

#include "mapget/model/feature.h"
#include "mapget/model/pointseries.h"

#include <array>
#include <atomic>
#include <map>
#include <mutex>

namespace mapget::shape
{

class RecordIndexes;

/** Stations use the producer's measure. Fractional vertex ordinals are used only to
 * compare assignments on the SAME selected geometry; XY is never its identity. */
class Axis
{
public:
    using Span = std::pair<double, double>;
    Axis(model_ptr<Feature> host, uint32_t ordinal, std::function<bool()> cancelled);
    model_ptr<Feature> host_;
    model_ptr<Geometry> geometry_;
    uint32_t ordinal_;
    std::function<bool()> cancelled_;

    mutable std::atomic<uint64_t> inventories{0}, recordIndexes{0}, recordsIndexed{0},
        distanceTables{0};
    // Different attachments of a shared raw array use one position index while
    // retaining their own applicability ranges, target identities and raw paths.
    std::shared_ptr<RecordIndexes> indexes;
    void check() const;
    [[nodiscard]] bool selects(PointSeries const& decl, Attribute const& attachment) const;
    [[nodiscard]] bool fallbackGeometry() const;
    [[nodiscard]] std::optional<Span>
    coverage(Validity const&, PointSeries::Measure, bool declared) const;
    [[nodiscard]] double distanceAt(double index, PointSeries::Measure) const;
    [[nodiscard]] std::optional<double> vertexAt(double distance, PointSeries::Measure) const;
    [[nodiscard]] double logicalVertex(AttrPointSequence const&, uint32_t logical) const;
    [[nodiscard]] double
    logicalDistanceAt(AttrPointSequence const&, double logical, PointSeries::Measure) const;
    [[nodiscard]] double last() const
    {
        return geometry_ && geometry_->numPoints() ? geometry_->numPoints() - 1. : 0.;
    }
    [[nodiscard]] static bool near(double a, double b);

private:
    struct Lengths
    {
        std::once_flag built;
        std::vector<double> values;
    };
    mutable std::array<Lengths, 2> lengths_;
    mutable std::mutex logicalMutex_;
    mutable std::map<std::pair<uint32_t, PointSeries::Measure>, std::shared_ptr<Lengths>>
        logicalLengths_;
    mutable std::once_flag catalogued_;
    mutable uint32_t sameType_ = 0, sameTypeAndName_ = 0;
    mutable bool fallback_ = false;
    void inventoryGeometries() const;
    [[nodiscard]] std::vector<double> const& lengths(PointSeries::Measure) const;
    [[nodiscard]] std::optional<double> project(Point p) const;
};

}  // namespace mapget::shape
