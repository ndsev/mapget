#pragma once

#include "simfil/model/nodes.h"

#include "geometry-data.h"
#include "featuremodellayer.h"
#include "point.h"
#include "featureid.h"
#include "sourcedatareference.h"
#include "sourceinfo.h"
#include "merged-array-view.h"
#include "nlohmann/json_fwd.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

using simfil::ValueType;
using simfil::ModelNode;
using simfil::ModelNodeAddress;
using simfil::ModelConstPtr;
using simfil::StringId;

namespace mapget
{

class PartitionFeatureLayer;
class PartitionFeatureModelLayerBase;
class Feature;
class GeometryArrayView;
class BoundsInfoNode;
class BoundsPolygonCoordinatesNode;
class BoundsRingNode;

/**
 * Owning copy of the geometry type, exposed points and polygon ring offsets.
 * It can outlive the source layer. Names, source references and GLTF node/bounds
 * metadata are not copied; AABB points contain origin and size, not two positions.
 */
struct SelfContainedGeometry
{
    std::vector<Point> points_;
    std::vector<uint32_t> polygonRingStarts_;
    GeomType geomType_ = GeomType::Points;
};

/**
 * Layer-owned geometry or a view into another geometry's point range. Supports
 * points, lines, polygons, triangle meshes, bounds and GLTF node references.
 * Ordinary vertices are stored as float offsets from the layer's geometry anchor.
 */
class Geometry final : public simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>
{
public:
    friend class PartitionFeatureModelLayerBase;
    friend class PartitionFeatureLayer;
    friend class PointNode;
    friend class LinearRingNode;
    friend class PointBufferNode;
    friend class PolygonNode;
    friend class MeshNode;
    friend class BoundsInfoNode;
    friend class BoundsPolygonCoordinatesNode;
    friend class BoundsRingNode;

    /** Source region */
    model_ptr<SourceDataReferenceCollection> sourceDataReferences() const;
    void setSourceDataReferences(simfil::ModelNode::Ptr const& refs);

    /** Add a point to the Geometry. */
    void append(Point const& p);

    /** Configure an AABB geometry as [origin, size]. */
    void setAabb(Point const& origin, Point const& size);
    [[nodiscard]] Point aabbOrigin() const;
    [[nodiscard]] Point aabbSize() const;

    /**
     * Configure/query a GLTF node reference geometry.
     *
     * The node index is stored losslessly only up to 2^24 because it is encoded
     * in the shared float-based point-buffer storage. The referenced binary
     * payload is the tile's independently transferred
     * `glbAttachmentName()`.
     */
    void setGltfNodeIndex(uint32_t index);
    [[nodiscard]] uint32_t gltfNodeIndex() const;
    void setGltfNodeBounds(Point const& origin, Point const& size);
    [[nodiscard]] Point gltfNodeAabbOrigin() const;
    [[nodiscard]] Point gltfNodeAabbSize() const;

    /** Get the type of the geometry. */
    [[nodiscard]] GeomType geomType() const;

    /** Get the number of points in the geometry buffer. */
    [[nodiscard]] size_t numPoints() const;

    /** Get a point at an index. */
    [[nodiscard]] Point pointAt(size_t index) const;

    /** Return the number of rings in a polygon geometry; non-polygons always have zero rings. */
    [[nodiscard]] uint32_t numPolygonRings() const;

    /** Return the start vertex of one polygon ring in the shared point buffer. */
    [[nodiscard]] uint32_t polygonRingStart(uint32_t ringIndex) const;

    /** Store explicit polygon ring starts; ring zero must start at vertex zero. */
    void setPolygonRingStarts(std::span<uint32_t const> ringStarts);

    /** Hash exposed coordinates and polygon ring layout, or GLTF index/bounds.
     * Names, source references and ordinary geometry type are not mixed in;
     * equal hashes do not establish geometry identity or equality.
     */
    [[nodiscard]] uint64_t getHash() const;

    /** Get or set the persisted layer-local logical geometry name. */
    [[nodiscard]] std::optional<std::string_view> name() const;
    void setName(std::optional<std::string_view> geometryName);

    /** Iterate over all Points in the geometry.
     * @param callback Function which is called for each contained point.
     *  Must return true to continue iteration, false to abort iteration.
     * @return True if all points were visited, false if the callback ever returned false.
     * @example
     *   geometry->forEachPoint([](Point const& point){
     *      std::cout << point.x << "," << point.y << "," << point.z << std::endl;
     *      return true;
     *   })
     * @note The ModelType must also be templated here, because in this header
     *  the class only exists in a predeclared form.
     */
    template <typename LambdaType, class ModelType = PartitionFeatureModelLayerBase>
    bool forEachPoint(LambdaType const& callback) const;

    /**
     * Sum surface distances between consecutive lon/lat points in metres,
     * ignoring elevation. Bounds and GLTF references return zero; callers
     * should use this as a path length only for line geometries.
     */
    [[nodiscard]] double length() const;

    /**
     * Return geometric points on a Polyline within the defined position bounds.
     * Project those bounds onto the line, then copy the intervening vertices.
     *
     * Requires at least two points and an in-segment x/y projection for each
     * bound. Among eligible segments, selection compares distance to each start
     * vertex, not distance to the segment; coordinates are interpolated linearly.
     * @param start is the beginning of the bounded range.
     * @param end is the optional end of the bounded range
     *            (otherwise, returns only a single position at start if the end is not passed).
     */
    [[nodiscard]] std::vector<Point> pointsFromPositionBound(const Point& start, const std::optional<Point>& end) const;

    /**
     * Return geometric points on the Polyline (if the geometry is a Polyline)
     * within the defined length range boundaries.
     * Distances are metres along the stored point order; reversed bounds are
     * swapped and clamped to [0, length()]. An omitted end returns one point.
     * Empty geometry returns no points; a single/zero-length path returns its
     * first point. Segment endpoints use linear coordinate interpolation.
     *
     * @param start Beginning of the bounded range, in metres.
     * @param end Optional end of the range, in metres; omission returns one point at start.
     */
    [[nodiscard]] std::vector<Point> pointsFromLengthBound(double start, std::optional<double> end) const;

    /**
     * Return percentage position point on the entire combined line geometries.
     * @param geoms vector of line geometries to determine the position on their entire length.
     * @param lengths Lengths in metres, in the same order and count as geoms.
     * @param numBits number of bits to store the percentage value.
     * @param position Encoded fraction in [0, 2^numBits - 1], not a value in [0, 100].
     */
    [[nodiscard]] static Point percentagePositionFromGeometries(
        std::vector<model_ptr<Geometry>> const& geoms,
        std::vector<double> const& lengths,
        uint32_t numBits,
        double position);

    /**
     * Turn the points and type from this geometry into a self-contained
     * struct which can be passed around.
     */
    [[nodiscard]] SelfContainedGeometry toSelfContained() const;
    [[nodiscard]] nlohmann::json toJson() const override;

    protected:
    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId&) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    using ViewData = GeometryViewData;

    using Storage = PartitionFeatureModelLayerBase::GeometryStorage;

    ViewData* geomViewData_ = nullptr;
    Storage* storage_ = nullptr;

public:
    explicit Geometry(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>(key)
    {
    }
    Geometry(ModelConstPtr pool,
             ModelNodeAddress a,
             simfil::detail::mp_key key);
    Geometry(ViewData* data, ModelConstPtr pool, ModelNodeAddress a, simfil::detail::mp_key key);

    Geometry() = delete;
};

/** Collection projection with `type` and `geometries` fields for zero/multiple
 * members. A singleton exposes that geometry's fields directly. A view over a
 * single geometry is read-only; materialize a concrete collection before append.
 */

class GeometryCollection
    : public MergedArrayView<GeometryCollection, Geometry, PartitionFeatureModelLayerBase>
{
public:
    friend class PartitionFeatureModelLayerBase;
    friend class PartitionFeatureLayer;
    friend class Feature;

    using Storage = simfil::Array::Storage;

    /** Adds a new Geometry to the collection and returns a reference. */
    model_ptr<Geometry> newGeometry(
        GeomType type,
        size_t initialCapacity=4,
        bool fixedSize=false);

    /** Append an existing Geometry to the collection. */
    void addGeometry(model_ptr<Geometry> const& geom);

    /** Get the number of contained geometries. */
    [[nodiscard]] size_t numGeometries() const;

    [[nodiscard]] nlohmann::json toJson() const override;

    /** Iterate over all Geometries in the collection.
     * @param callback Function which is called for each contained geometry.
     *  Must return true to continue iteration, false to abort iteration.
     * @return True if all geometries were visited, false if the callback ever returned false.
     * @example
     *   collection->forEachGeometry([](simfil::model_ptr<Geometry> const& geom){
     *      std::cout << geom->numPoints() << std::endl;
     *      return true;
     *   })
     * @note The ModelType must also be templated here, because in this header
     *  the class only exists in a predeclared form.
     */
    template <typename LambdaType, class ModelType = PartitionFeatureModelLayerBase>
    bool forEachGeometry(LambdaType const& callback) const
    {
        const auto localCount = this->localMergedSize();
        for (uint32_t i = 0; i < localCount; ++i) {
            auto localGeom = localGeometryAt(i);
            if (!localGeom) {
                continue;
            }
            if (!callback(modelPtr<ModelType>()->template resolve<Geometry>(*localGeom))) {
                return false;
            }
        }
        if (auto ext = mergedExtension()) {
            return ext->forEachGeometry(callback);
        }
        return true;
    }

public:
    explicit GeometryCollection(simfil::detail::mp_key key)
        : MergedArrayView<GeometryCollection, Geometry, PartitionFeatureModelLayerBase>(key)
    {
    }
    GeometryCollection(ModelConstPtr pool, ModelNodeAddress, simfil::detail::mp_key key);

    GeometryCollection() = delete;

private:
    [[nodiscard]] ExtensionPtr mergedExtension() const override;
    [[nodiscard]] uint32_t localMergedSize() const override;
    [[nodiscard]] ModelNode::Ptr localMergedAt(int64_t i) const override;
    bool localMergedIterate(IterCallback const& cb) const override;  // NOLINT (allow discard)
    [[nodiscard]] bool isFeatureScopedView() const;
    [[nodiscard]] model_ptr<Feature> featureScopedFeature() const;
    [[nodiscard]] ModelNodeAddress featureScopedGeometryAddress() const;
    [[nodiscard]] ModelNode::Ptr localGeometryAt(int64_t i) const;
    [[nodiscard]] model_ptr<GeometryArrayView> mergedGeometryArray() const;
    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId&) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    ModelNode::Ptr singleGeom() const;
};

/** Array projection of a feature's single geometry or collection, without copying vertices. */
class GeometryArrayView
    : public MergedArrayView<GeometryArrayView, Geometry, PartitionFeatureModelLayerBase>
{
public:
    explicit GeometryArrayView(simfil::detail::mp_key key)
        : MergedArrayView<GeometryArrayView, Geometry, PartitionFeatureModelLayerBase>(key)
    {
    }

    GeometryArrayView(ModelConstPtr pool, ModelNodeAddress address, simfil::detail::mp_key key)
        : MergedArrayView<GeometryArrayView, Geometry, PartitionFeatureModelLayerBase>(
              std::move(pool),
              address,
              key)
    {
    }

    GeometryArrayView(
        ModelConstPtr pool,
        ModelNodeAddress address,
        ModelNodeAddress singleGeometryAddress,
        simfil::detail::mp_key key)
        : MergedArrayView<GeometryArrayView, Geometry, PartitionFeatureModelLayerBase>(
              std::move(pool),
              address,
              key),
          singleGeometryAddress_(singleGeometryAddress)
    {
    }

    GeometryArrayView() = delete;

private:
    [[nodiscard]] ExtensionPtr mergedExtension() const override;
    [[nodiscard]] uint32_t localMergedSize() const override;
    [[nodiscard]] ModelNode::Ptr localMergedAt(int64_t i) const override;
    bool localMergedIterate(IterCallback const& cb) const override;  // NOLINT (allow discard)
    [[nodiscard]] bool isFeatureScopedView() const;
    [[nodiscard]] model_ptr<Feature> featureScopedFeature() const;
    [[nodiscard]] ModelNodeAddress featureScopedGeometryAddress() const;

    ModelNodeAddress singleGeometryAddress_;
};

/** Array view decoding anchor-relative storage into absolute Point nodes. */

class PointBufferNode final
    : public simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>
{
public:
    friend class PartitionFeatureModelLayerBase;
    friend class PartitionFeatureLayer;
    friend class Geometry;
    friend class MeshNode;

    explicit PointBufferNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>(key)
    {
    }

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId &) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    Point pointAt(int64_t) const;

    [[nodiscard]] ModelNodeAddress baseGeometryAddress() const { return baseGeomAddress_; }

    PointBufferNode() = delete;

public:
    PointBufferNode(
        ModelConstPtr pool,
        ModelNodeAddress const& baseGeometryAddress,
        simfil::detail::mp_key key);

private:
    simfil::ArrayIndex baseVertexArray_ = simfil::InvalidArrayIndex;
    ModelNodeAddress baseGeomAddress_;
    Geometry::Storage* storage_ = nullptr;
    uint32_t offset_ = 0;
    uint32_t size_ = 0;
};

/** Procedural {origin, size} object for an AABB or GLTF node's bounds. */
class BoundsInfoNode final
    : public simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>
{
public:
    explicit BoundsInfoNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>(key)
    {
    }

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId&) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    BoundsInfoNode() = delete;
    BoundsInfoNode(ModelNode const& baseNode, simfil::detail::mp_key key);

private:
    ModelNodeAddress baseGeometryAddress_;
};

/** GeoJSON polygon-coordinate wrapper for a bounds geometry's ground footprint. */
class BoundsPolygonCoordinatesNode final
    : public simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>
{
public:
    explicit BoundsPolygonCoordinatesNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>(key)
    {
    }

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId&) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    BoundsPolygonCoordinatesNode() = delete;
    BoundsPolygonCoordinatesNode(ModelNode const& baseNode, simfil::detail::mp_key key);

private:
    ModelNodeAddress baseGeometryAddress_;
};

/** Closed five-point ground-footprint ring, derived without a second vertex buffer. */
class BoundsRingNode final
    : public simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>
{
public:
    explicit BoundsRingNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>(key)
    {
    }

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId&) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    BoundsRingNode() = delete;
    BoundsRingNode(ModelNode const& baseNode, simfil::detail::mp_key key);

private:
    ModelNodeAddress baseGeometryAddress_;
};

/** GeoJSON coordinate array exposing the outer polygon ring followed by its holes. */

class PolygonNode final
    : public simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>
{
public:
    friend class PartitionFeatureModelLayerBase;
    friend class PartitionFeatureLayer;
    friend class Geometry;

    explicit PolygonNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>(key)
    {
    }

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId &) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    PolygonNode() = delete;

public:
    PolygonNode(ModelConstPtr pool, ModelNodeAddress const& a, simfil::detail::mp_key key);
};

/** GeoJSON MultiPolygon projection of a triangle mesh; triangles share stored vertices. */

class MeshNode final : public simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>
{
public:
    friend class PartitionFeatureModelLayerBase;
    friend class PartitionFeatureLayer;
    friend class Geometry;

    explicit MeshNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>(key)
    {
    }

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;

    [[nodiscard]] ModelNode::Ptr get(const StringId&) const override { return {}; }

    [[nodiscard]] StringId keyAt(int64_t) const override { return {}; }
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    MeshNode() = delete;

public:
    MeshNode(ModelConstPtr pool,
             ModelNodeAddress const& a,
             simfil::detail::mp_key key);

private:
    uint32_t size_ = 0;
};

/** One triangle's polygon coordinate wrapper in the mesh's MultiPolygon projection. */
class MeshTriangleCollectionNode
    : public simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>
{
public:
    friend class PartitionFeatureModelLayerBase;
    friend class PartitionFeatureLayer;
    friend class Geometry;

    explicit MeshTriangleCollectionNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>(key)
    {
    }

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    MeshTriangleCollectionNode() = delete;

public:
    explicit MeshTriangleCollectionNode(const ModelNode& base, simfil::detail::mp_key key);

private:
    uint32_t index_ = 0;
};

/**
 * Coordinate view that closes a stored polygon ring and normalizes its winding.
 * Outer rings are counter-clockwise; hole rings are clockwise. Closing/reversing
 * the view leaves the source vertex buffer unchanged.
 */
class LinearRingNode : public simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>
{
public:
    friend class PartitionFeatureModelLayerBase;
    friend class PartitionFeatureLayer;
    friend class Geometry;

    explicit LinearRingNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<PartitionFeatureModelLayerBase>(key)
    {
    }

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId &) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    LinearRingNode() = delete;

public:
    explicit LinearRingNode(const ModelNode& base, simfil::detail::mp_key key);
    LinearRingNode(const ModelNode& base, std::optional<size_t> length, simfil::detail::mp_key key);

private:
    model_ptr<PointBufferNode> vertexBuffer() const;

    enum class Orientation : uint8_t { CW, CCW };
    Orientation orientation_ = Orientation::CW;
    Orientation desiredOrientation_ = Orientation::CCW;
    bool closed_ = false;
    uint32_t offset_ = 0;
    uint32_t size_ = 0;
    uint32_t ringIndex_ = 0;
};

}
