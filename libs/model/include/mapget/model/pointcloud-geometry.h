#pragma once

#include "geometry.h"
#include "pointnode.h"
#include "pointcloud-sampler.h"

namespace mapget
{
/** Count-based visualization sampling retains original coordinates and cluster provenance. */
struct PointCloudGeometrySample
{
    struct Record {
        Point point;
        uint32_t geometryAddress;
    };
    uint64_t sourceCount = 0;
    std::vector<Record> records;

    /** Count matching point geometries without scanning or copying their vertices. */
    static uint64_t count(model_ptr<GeometryCollection> const& source,
                          std::optional<std::string> const& geometryName = {})
    {
        uint64_t result = 0;
        if (source) source->forEachGeometry([&](auto const& geometry) {
            if (geometry->geomType() == GeomType::Points &&
                (!geometryName || geometry->name() == *geometryName)) {
                if (geometry->numPoints() > std::numeric_limits<uint64_t>::max() - result)
                    throw std::length_error("Point-cloud source count overflow");
                result += geometry->numPoints();
            }
            return true;
        });
        return result;
    }

    /** Stream selected original records across all clusters without an intermediate point vector. */
    template <typename Callback>
    static void forEach(model_ptr<GeometryCollection> const& source, PointCloudIndexSelection const& selection,
                        Callback&& callback, std::optional<std::string> const& geometryName = {})
    {
        uint64_t sourceIndex = 0, ordinal = 0;
        if (!source) return;
        uint64_t next = selection.size() ? selection.index(0) : std::numeric_limits<uint64_t>::max();
        source->forEachGeometry([&](auto const& geometry) {
            if (geometry->geomType() == GeomType::Points &&
                (!geometryName || geometry->name() == *geometryName)) {
                geometry->forEachPoint([&](Point const& point) {
                    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
                        std::abs(point.x) > 180. || std::abs(point.y) > 90.)
                        throw std::invalid_argument("Invalid point-cloud WGS84 coordinate");
                    if (sourceIndex++ == next) {
                        callback(Record{point, geometry->addr().value_});
                        next = ++ordinal < selection.size() ? selection.index(ordinal) :
                            std::numeric_limits<uint64_t>::max();
                    }
                    return true;
                });
            }
            return true;
        });
    }

    /** Retain an exact-sized sample for backend subset materialization. */
    static PointCloudGeometrySample build(model_ptr<GeometryCollection> const& source, uint32_t maxPoints,
                                          std::optional<std::string> const& geometryName = {})
    {
        PointCloudGeometrySample result;
        result.sourceCount = count(source, geometryName);
        PointCloudIndexSelection selection(result.sourceCount, maxPoints);
        result.records.reserve(selection.size());
        forEach(source, selection, [&](Record const& record) { result.records.push_back(record); }, geometryName);
        return result;
    }
};
}
