#pragma once

#include "simfil/model/nodes.h"

#include "point.h"
#include "featureid.h"
#include "sourceinfo.h"

#include <cstdint>
#include <optional>

using simfil::ValueType;
using simfil::ModelNode;
using simfil::ModelNodeAddress;
using simfil::ModelConstPtr;
using simfil::StringId;

namespace mapget
{

class TileFeatureLayer;

enum class GeomType: uint8_t {
    Points,   // Point-cloud
    Line,     // Line-string
    Polygon,  // Auto-closed polygon
    Mesh      // Collection of triangles
};

/**
 * Geometry object, which stores a point collection, a line-string,
 * or a triangle mesh.
 */
class Geometry final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
public:
    template<typename> friend struct simfil::shared_model_ptr;
    friend class TileFeatureLayer;
    friend class VertexNode;
    friend class LinearRingNode;
    friend class VertexBufferNode;
    friend class PolygonNode;
    friend class MeshNode;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId&) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    /** Source region */
    void setSourceDataReference(SourceDataReference info);
    SourceDataReference sourceDataReference() const;

    /** Add a point to the Geometry. */
    void append(Point const& p);

    /** Get the type of the geometry. */
    [[nodiscard]] GeomType geomType() const;

    /** Get the number of points in the geometry buffer. */
    [[nodiscard]] size_t numPoints() const;

    /** Get a point at an index. */
    [[nodiscard]] Point pointAt(size_t index) const;

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
    template <typename LambdaType, class ModelType = TileFeatureLayer>
    bool forEachPoint(LambdaType const& callback) const;

protected:
    struct Data
    {
        Data() = default;
        Data(GeomType t, size_t capacity) : isView_(false), type_(t) {
            detail_.geom_.vertexArray_ = -(simfil::ArrayIndex)capacity;
        }
        Data(GeomType t, uint32_t offset, uint32_t size, ModelNodeAddress base) : isView_(true), type_(t) {
            detail_.view_.offset_ = offset;
            detail_.view_.size_ = size;
            detail_.view_.baseGeometry_ = base;
        }

        // Flag to indicate whether this geometry is just
        // a view into another geometry object.
        bool isView_ = false;

        // Geometry type. A view can have a different geometry type
        // than the base geometry.
        GeomType type_ = GeomType::Points;

        union GeomDetails
        {
            GeomDetails() {new(&geom_) GeomBaseDetails();}

            struct GeomBaseDetails {
                // Vertex array index, or negative requested initial
                // capacity, if no point is added yet.
                simfil::ArrayIndex vertexArray_ = -1;

                // Offset is set when vertexArray is allocated,
                // which happens when the first point is added.
                Point offset_;
            } geom_;

            struct GeomViewDetails {
                // If this geometry is a view, then it references
                // a range of vertices in another geometry.

                // Offset within the other geometry.
                uint32_t offset_ = 0;

                // Number of referenced vertices.
                uint32_t size_ = 0;

                // Address of the referenced geometry - may be a view itself.
                ModelNodeAddress baseGeometry_;
            } view_;
        } detail_;

        SourceDataReference sourceDataReference_;

        template<typename S>
        void serialize(S& s) {
            s.value1b(isView_);
            s.value1b(type_);
            if (!isView_) {
                s.value4b(detail_.geom_.vertexArray_);
                s.object(detail_.geom_.offset_);
            }
            else {
                s.value4b(detail_.view_.offset_);
                s.value4b(detail_.view_.size_);
                s.object(detail_.view_.baseGeometry_);
            }
            s.object(sourceDataReference_);
        }
    };

    using Storage = simfil::ArrayArena<glm::fvec3, simfil::detail::ColumnPageSize*2>;

    Data* geomData_ = nullptr;
    Storage* storage_ = nullptr;

    Geometry() = default;
    Geometry(Data* data, ModelConstPtr pool, ModelNodeAddress a);
};

/** GeometryCollection node has `type` and `geometries` fields. */

class GeometryCollection : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
public:
    template<typename> friend struct simfil::shared_model_ptr;
    friend class TileFeatureLayer;
    friend class Feature;

    using Storage = simfil::Array::Storage;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId&) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    /** Adds a new Geometry to the collection and returns a reference. */
    model_ptr<Geometry> newGeometry(GeomType type, size_t initialCapacity=4);

    /** Append an existing Geometry to the collection. */
    void addGeometry(model_ptr<Geometry> const& geom);

    /** Get the number of contained geometries. */
    [[nodiscard]] size_t numGeometries() const;

    /** Iterate over all Geometries in the collection.
     * @param callback Function which is called for each contained geometry.
     *  Must return true to continue iteration, false to abort iteration.
     * @return True if all geometries were visited, false if the callback ever returned false.
     * @example
     *   collection->forEachGeometry([](simfil::shared_model_ptr<Geometry> const& geom){
     *      std::cout << geom->type() << std::endl;
     *      return true;
     *   })
     * @note The ModelType must also be templated here, because in this header
     *  the class only exists in a predeclared form.
     */
    template <typename LambdaType, class ModelType = TileFeatureLayer>
    bool forEachGeometry(LambdaType const& callback) const {
        auto geomArray = modelPtr<ModelType>()->arrayMemberStorage().range((simfil::ArrayIndex)addr().index());
        return std::all_of(geomArray.begin(), geomArray.end(), [this, &callback](auto&& geomNodeAddress){
            return callback(modelPtr<ModelType>()->resolveGeometry(*ModelNode::Ptr::make(model_, geomNodeAddress)));
        });
    }

private:
    GeometryCollection() = default;
    GeometryCollection(ModelConstPtr pool, ModelNodeAddress);

    ModelNode::Ptr singleGeom() const;
};

/** VertexBuffer Node */

class VertexBufferNode final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
public:
    template<typename> friend struct simfil::shared_model_ptr;
    friend class TileFeatureLayer;
    friend class Geometry;
    friend class MeshNode;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId &) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    Point pointAt(int64_t) const;

    VertexBufferNode() = delete;

private:
    VertexBufferNode(Geometry::Data const* geomData, ModelConstPtr pool, ModelNodeAddress const& a);

    Geometry::Data const* baseGeomData_ = nullptr;
    ModelNodeAddress baseGeomAddress_;
    Geometry::Storage* storage_ = nullptr;
    uint32_t offset_ = 0;
    uint32_t size_ = 0;
};

/** Polygon Node */

class PolygonNode final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
public:
    template<typename> friend struct simfil::shared_model_ptr;
    friend class TileFeatureLayer;
    friend class Geometry;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId &) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    PolygonNode() = delete;

private:
    PolygonNode(ModelConstPtr pool, ModelNodeAddress const& a);
};

/** Mesh Node */

class MeshNode final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
public:
    template<typename> friend struct simfil::shared_model_ptr;
    friend class TileFeatureLayer;
    friend class Geometry;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId &) const override { return {}; }
    [[nodiscard]] StringId keyAt(int64_t) const override { return {}; }
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    MeshNode() = delete;

private:
    MeshNode(Geometry::Data const* geomData, ModelConstPtr pool, ModelNodeAddress const& a);

    Geometry::Data const* geomData_;
    uint32_t size_ = 0;
};

class MeshTriangleCollectionNode : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
public:
    template<typename> friend struct simfil::shared_model_ptr;
    friend class TileFeatureLayer;
    friend class Geometry;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    MeshTriangleCollectionNode() = delete;

private:
    explicit MeshTriangleCollectionNode(const ModelNode& base);

    uint32_t index_ = 0;
};

/**
 * LinearRing Node
 *
 * A linear ring represents a simple polygon that is closed and in CCW order.
 */
class LinearRingNode : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
public:
    template<typename> friend struct simfil::shared_model_ptr;
    friend class TileFeatureLayer;
    friend class Geometry;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId &) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    LinearRingNode() = delete;

private:
    explicit LinearRingNode(const ModelNode& base, std::optional<size_t> length = {});

    model_ptr<VertexBufferNode> vertexBuffer() const;

    enum class Orientation : uint8_t { CW, CCW };
    Orientation orientation_ = Orientation::CW;
    bool closed_ = false;
    uint32_t offset_ = 0;
    uint32_t size_ = 0;
};

/** Vertex Node */

class VertexNode final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
public:
    template<typename> friend struct simfil::shared_model_ptr;
    friend class TileFeatureLayer;
    friend class Geometry;
    friend class VertexBufferNode;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId &) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    VertexNode() = delete;

private:
    VertexNode(ModelNode const& baseNode, Geometry::Data const* geomData);

    Point point_;
};

template <typename LambdaType, class ModelType>
bool Geometry::forEachPoint(LambdaType const& callback) const {
    VertexBufferNode vertexBufferNode{geomData_, model_, {ModelType::ColumnId::PointBuffers, addr_.index()}};
    for (auto i = 0; i < vertexBufferNode.size(); ++i) {
        VertexNode vertex{*vertexBufferNode.at(i), vertexBufferNode.baseGeomData_};
        if (!callback(vertex.point_))
            return false;
    }
    return true;
}

}
