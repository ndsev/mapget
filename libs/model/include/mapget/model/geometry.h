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
#include <utility>

using simfil::ValueType;
using simfil::ModelNode;
using simfil::ModelNodeAddress;
using simfil::ModelConstPtr;
using simfil::StringId;

namespace mapget
{

class TileFeatureLayer;
class TileFeatureModelLayerBase;
class Feature;
class GeometryArrayView;
class BoundsInfoNode;
class BoundsPolygonCoordinatesNode;
class BoundsRingNode;

/**
 * Small interface container type which may be used
 * to pass around geometry data.
 */
struct SelfContainedGeometry
{
    std::vector<Point> points_;
    GeomType geomType_ = GeomType::Points;
};

/**
 * Geometry object, which stores a point collection, a line-string,
 * or a triangle mesh.
 */
class Geometry final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>
{
public:
    friend class TileFeatureModelLayerBase;
    friend class TileFeatureLayer;
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
     * payload is the tile's `glbAttachment()`.
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

    /** Get the human-readable stage name if this geometry is above high fidelity. */
    [[nodiscard]] std::optional<std::string_view> name() const;

    /** Get a hash of the geometry. **/
    [[nodiscard]] uint64_t getHash() const;

    /** Get the persisted logical stage of this geometry, if any. */
    [[nodiscard]] std::optional<uint32_t> stage() const;

    /** Persist an explicit geometry stage override used by JSON import/export. */
    void setStage(std::optional<uint32_t> geometryStage);

    /** Iterate over all Points in the geometry.
     * @param callback Function which is called for each contained point.
     *  Must return true to continue iteration, false to abort iteration.
     * @return True if all points were visited, false if the callback ever returned false.
     * @example
     *   collection->forEachPoint([](Point&& point){
     *      std::cout << point.x() << "," << point.y() << "," << point.z() << std::endl;
     *      return true;
     *   })
     * @note The ModelType must also be templated here, because in this header
     *  the class only exists in a predeclared form.
     */
    template <typename LambdaType, class ModelType = TileFeatureModelLayerBase>
    bool forEachPoint(LambdaType const& callback) const;

    /**
     * Get total length of the geometry in metres assuming it's a Polyline.
     */
    [[nodiscard]] double length() const;

    /**
     * Return geometric points on the Polyline (if the geometry is a Polyline)
     * within the defined position range boundaries.
     * @param start is the beginning of the bounded range.
     * @param end is the optional end of the bounded range
     *            (otherwise, returns only a single position at start if the end is not passed).
     */
    [[nodiscard]] std::vector<Point> pointsFromPositionBound(const Point& start, const std::optional<Point>& end) const;

    /**
     * Return geometric points on the Polyline (if the geometry is a Polyline)
     * within the defined length range boundaries.
     * @param start is the beginning of the bounded range.
     * @param end is the optional end of the bounded range
     *            (otherwise, returns only a single position at start if the end is not passed).
     */
    [[nodiscard]] std::vector<Point> pointsFromLengthBound(double start, std::optional<double> end) const;

    /**
     * Return percentage position point on the entire combined line geometries.
     * @param geoms vector of line geometries to determine the position on their entire length.
     * @param lengths vector of each geomtery length.
     * @param numBits number of bits to store the percentage value.
     * @param position percentage position on the geometries.
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

    using Storage = TileFeatureModelLayerBase::GeometryStorage;

    ViewData* geomViewData_ = nullptr;
    Storage* storage_ = nullptr;

public:
    explicit Geometry(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(key) {}
    Geometry(ModelConstPtr pool,
             ModelNodeAddress a,
             simfil::detail::mp_key key);
    Geometry(ViewData* data,
             ModelConstPtr pool,
             ModelNodeAddress a,
             simfil::detail::mp_key key);
    Geometry() = delete;
};

/** GeometryCollection node has `type` and `geometries` fields. */

class GeometryCollection : public MergedArrayView<GeometryCollection, Geometry, TileFeatureModelLayerBase>
{
public:
    friend class TileFeatureModelLayerBase;
    friend class TileFeatureLayer;
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

    /**
     * Resolve the geometry stage that should be used when no explicit stage
     * override is given. This defaults to the layer's configured
     * `highFidelityStage_`.
     */
    [[nodiscard]] std::optional<uint32_t> preferredGeometryStage(
        std::optional<uint32_t> stageOverride = std::nullopt) const;

    /**
     * Find the first geometry of the requested type at the preferred stage.
     * When `stageOverride` is omitted, the layer's `highFidelityStage_` is used.
     */
    [[nodiscard]] model_ptr<Geometry> geometryOfTypeAtPreferredStage(
        GeomType type,
        std::optional<uint32_t> stageOverride = std::nullopt) const;

    [[nodiscard]] nlohmann::json toJson() const override;

    /** Iterate over all geometries at the preferred stage. */
    template <typename LambdaType>
    bool forEachGeometryAtPreferredStage(
        std::optional<uint32_t> stageOverride,
        LambdaType const& callback) const
    {
        auto const preferredStage = preferredGeometryStage(stageOverride);
        if (!preferredStage) {
            return true;
        }
        return forEachGeometry([&](model_ptr<Geometry> const& geom) {
            if (geom->stage().value_or(0U) != *preferredStage) {
                return true;
            }
            return callback(geom);
        });
    }

    /** Iterate over all Geometries in the collection.
     * @param callback Function which is called for each contained geometry.
     *  Must return true to continue iteration, false to abort iteration.
     * @return True if all geometries were visited, false if the callback ever returned false.
     * @example
     *   collection->forEachGeometry([](simfil::model_ptr<Geometry> const& geom){
     *      std::cout << geom->type() << std::endl;
     *      return true;
     *   })
     * @note The ModelType must also be templated here, because in this header
     *  the class only exists in a predeclared form.
     */
    template <typename LambdaType, class ModelType = TileFeatureModelLayerBase>
    bool forEachGeometry(LambdaType const& callback) const {
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
        : MergedArrayView<GeometryCollection, Geometry, TileFeatureModelLayerBase>(key) {}
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

class GeometryArrayView : public MergedArrayView<GeometryArrayView, Geometry, TileFeatureModelLayerBase>
{
public:
    explicit GeometryArrayView(simfil::detail::mp_key key)
        : MergedArrayView<GeometryArrayView, Geometry, TileFeatureModelLayerBase>(key)
    {
    }

    GeometryArrayView(
        ModelConstPtr pool,
        ModelNodeAddress address,
        simfil::detail::mp_key key)
        : MergedArrayView<GeometryArrayView, Geometry, TileFeatureModelLayerBase>(std::move(pool), address, key)
    {
    }

    GeometryArrayView(
        ModelConstPtr pool,
        ModelNodeAddress address,
        ModelNodeAddress singleGeometryAddress,
        simfil::detail::mp_key key)
        : MergedArrayView<GeometryArrayView, Geometry, TileFeatureModelLayerBase>(std::move(pool), address, key),
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

/** VertexBuffer Node */

class PointBufferNode final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>
{
public:
    friend class TileFeatureModelLayerBase;
    friend class TileFeatureLayer;
    friend class Geometry;
    friend class MeshNode;

    explicit PointBufferNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(key) {}

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

class BoundsInfoNode final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>
{
public:
    explicit BoundsInfoNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(key) {}

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

class BoundsPolygonCoordinatesNode final
    : public simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>
{
public:
    explicit BoundsPolygonCoordinatesNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(key) {}

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

class BoundsRingNode final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>
{
public:
    explicit BoundsRingNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(key) {}

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

/** Polygon Node */

class PolygonNode final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>
{
public:
    friend class TileFeatureModelLayerBase;
    friend class TileFeatureLayer;
    friend class Geometry;

    explicit PolygonNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(key) {}

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

/** Mesh Node */

class MeshNode final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>
{
public:
    friend class TileFeatureModelLayerBase;
    friend class TileFeatureLayer;
    friend class Geometry;

    explicit MeshNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(key) {}

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId &) const override { return {}; }
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

class MeshTriangleCollectionNode : public simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>
{
public:
    friend class TileFeatureModelLayerBase;
    friend class TileFeatureLayer;
    friend class Geometry;

    explicit MeshTriangleCollectionNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(key) {}

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
 * LinearRing Node
 *
 * A linear ring represents a simple polygon that is closed and in CCW order.
 */
class LinearRingNode : public simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>
{
public:
    friend class TileFeatureModelLayerBase;
    friend class TileFeatureLayer;
    friend class Geometry;

    explicit LinearRingNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(key) {}

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
    bool closed_ = false;
    uint32_t offset_ = 0;
    uint32_t size_ = 0;
};

}
