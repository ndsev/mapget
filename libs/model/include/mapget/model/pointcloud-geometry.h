#pragma once

#include "geometry.h"
#include "pointnode.h"
#include "pointcloud-sampler.h"

namespace mapget
{
/** A bounded representative list in a fixed ECEF grid, retaining original WGS84 records. */
struct PointCloudGeometrySample
{
    struct Record {
        PointCloudGrid::Position position;
        Point point;
        uint32_t geometryAddress;
    };
    PointCloudGrid::Position lower{};
    PointCloudGrid::Position upper{};
    uint64_t sourceCount = 0;
    size_t quota = 0;
    std::vector<Record> records;

    /** Cartesian metres avoid longitude seams, latitude scaling, and polar singularities. */
    static PointCloudGrid::Position ecef(Point const& point)
    {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
            std::abs(point.x) > 180. || std::abs(point.y) > 90.)
            throw std::invalid_argument("Invalid point-cloud WGS84 coordinate");
        constexpr double radians = 3.14159265358979323846 / 180.;
        constexpr double e2 = 0.0066943799901413165;
        double const latitude = point.y * radians;
        double const longitude = point.x * radians;
        double const sinLat = std::sin(latitude), cosLat = std::cos(latitude);
        double const radius = 6378137. / std::sqrt(1. - e2 * sinLat * sinLat);
        return {(radius + point.z) * cosLat * std::cos(longitude),
                (radius + point.z) * cosLat * std::sin(longitude),
                (radius * (1. - e2) + point.z) * sinLat};
    }

    /** Two allocation-free source traversals: bounds/counts, then bounded selection. */
    static PointCloudGeometrySample build(
        model_ptr<GeometryCollection> const& source, size_t maxPoints,
        std::optional<std::string> const& geometryName = {})
    {
        PointCloudGeometrySample result;
        result.quota = maxPoints;
        auto visit = [&](auto&& callback) {
            if (!source) return;
            source->forEachGeometry([&](auto const& geometry) {
                if (geometry->geomType() == GeomType::Points &&
                    (!geometryName || geometry->name() == *geometryName)) {
                    geometry->forEachPoint([&](Point const& point) {
                        callback(point, geometry->addr().value_);
                        return true;
                    });
                }
                return true;
            });
        };
        visit([&](Point const& point, uint32_t) {
            auto const xyz = ecef(point);
            if (!result.sourceCount++) result.lower = result.upper = xyz;
            else for (size_t axis = 0; axis < 3; ++axis) {
                result.lower[axis] = std::min(result.lower[axis], xyz[axis]);
                result.upper[axis] = std::max(result.upper[axis], xyz[axis]);
            }
        });
        PointCloudSampler<Record> sampler(
            PointCloudGrid(result.lower, result.upper, std::max<size_t>(1, maxPoints)), maxPoints);
        visit([&](Point const& point, uint32_t address) { sampler.add({ecef(point), point, address}); });
        result.records = sampler.takeRecords();
        return result;
    }

    /** Aligned coarsening reuses representatives; no original geometry traversal. */
    [[nodiscard]] std::vector<Record> coarsen(size_t maxPoints, double spacingMeters) const
    {
        maxPoints = std::min(maxPoints, quota);
        PointCloudGrid grid(lower, upper, std::max<size_t>(1, maxPoints), spacingMeters);
        // A sparse base sample can fit the coarse quota even when its source did not.
        // Complete sources which fit the quota keep duplicates, just like initial selection.
        PointCloudSampler<Record> sampler(std::move(grid), maxPoints, sourceCount <= maxPoints);
        for (auto const& record : records) sampler.add(record);
        return sampler.takeRecords();
    }
};
}
