#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <glm/vec3.hpp>

#include "simfil/model/arena.h"
#include "simfil/model/model.h"
#include "simfil/model/nodes.h"
#include "simfil/model/string-pool.h"

#include "geometry-data.h"
#include "info.h"
#include "layer.h"
#include "point.h"
#include "sourceinfo.h"
#include "tl/expected.hpp"

namespace simfil::detail
{
template <>
struct is_model_column_external_type<glm::vec3> : std::true_type
{};
}

namespace mapget
{

class FeatureId;
class Geometry;
class GeometryCollection;
class SourceDataReferenceCollection;
class SourceDataReferenceItem;
struct QualifiedSourceDataReference;
class PointNode;
class PointBufferNode;
class GeometryArrayView;
class BoundsInfoNode;
class BoundsPolygonCoordinatesNode;
class BoundsRingNode;
class PolygonNode;
class MeshNode;
class MeshTriangleCollectionNode;
class LinearRingNode;
template<class, class, class> class MergedArrayView;

template<typename T>
using model_ptr = simfil::model_ptr<T>;

using Object = simfil::Object;
using Array = simfil::Array;

/**
 * Common ModelPool base for tile layers that store map feature identifiers and geometry.
 *
 * The base deliberately owns no concrete columns. Instead, it defines the shared
 * column id domain and the factory/storage protocol used by reusable FeatureId
 * and geometry nodes. Concrete layers decide which roots and result-specific
 * columns they add on top.
 */
class TileFeatureModelLayerBase : public TileLayer, public simfil::ModelPool
{
    friend class FeatureId;
    friend class Geometry;
    friend class GeometryCollection;
    friend class PointNode;
    friend class PointBufferNode;
    friend class PolygonNode;
    friend class MeshNode;
    friend class MeshTriangleCollectionNode;
    friend class LinearRingNode;
    friend class SourceDataReferenceCollection;
    friend class SourceDataReferenceItem;
    template<class, class, class> friend class MergedArrayView;
    template<typename Target>
    friend model_ptr<Target> resolveInternal(
        simfil::res::tag<Target>,
        TileFeatureModelLayerBase const&,
        simfil::ModelNode const&);

public:
    using ModelPool::resolve;
    using GeometryStorage = simfil::ArrayArena<glm::vec3, simfil::detail::ColumnPageSize * 2>;

    /** Shared custom column ids used by reusable feature-id and geometry nodes. */
    struct ColumnId { enum : uint8_t {
        Features = FirstCustomColumnId,
        FeatureComplexData,
        FeatureProperties,
        FeatureIds,
        ExternalFeatureIds,
        Attributes,
        AttributeLayers,
        AttributeLayerLists,
        Relations,
        Points,
        PointBuffers,
        PointBuffersView,
        PointGeometries,
        LineGeometries,
        PolygonGeometries,
        MeshGeometries,
        AabbGeometries,
        GltfNodeIndexGeometries,
        GeometryViews,
        GeometryCollections,
        Mesh,
        MeshTriangleCollection,
        MeshTriangleLinearRing,
        Polygon,
        LinearRing,
        SourceDataReferenceCollections,
        SourceDataReferences,
        Validities,
        ValidityPoints,
        ValidityCollections,
        FeatureRelationsView,
        GeometryArrayView,
        GeometryBoundsInfoView,
        GeometryBoundsPolygonCoordinatesView,
        GeometryBoundsRingView,
        GeometryPointView,
        SimpleValidity,
        SearchResults,
        SearchResultValues,
    }; };

    /** Create a feature id in the concrete layer's shared FeatureId storage. */
    virtual model_ptr<FeatureId> newFeatureId(
        std::string_view const& typeId,
        KeyValueViewPairs const& featureIdParts,
        std::optional<std::string_view> externalMapId = std::nullopt) = 0;

    /** Create a geometry collection in the concrete layer's shared geometry storage. */
    virtual model_ptr<GeometryCollection> newGeometryCollection(
        size_t initialCapacity = 2,
        bool fixedSize = false) = 0;

    /** Create a concrete geometry in the concrete layer's shared geometry storage. */
    virtual model_ptr<Geometry> newGeometry(
        GeomType geomType,
        size_t initialCapacity = 2,
        bool fixedSize = false) = 0;

    /** Create a view into an existing geometry buffer. */
    virtual model_ptr<Geometry> newGeometryView(
        GeomType geomType,
        uint32_t offset,
        uint32_t size,
        model_ptr<Geometry> const& base) = 0;

    /** Create a source-data reference collection used by geometry nodes. */
    virtual model_ptr<SourceDataReferenceCollection> newSourceDataReferenceCollection(
        std::span<QualifiedSourceDataReference> list) = 0;

    /** Resolve a stored feature-id node from the concrete layer's columns. */
    [[nodiscard]] virtual model_ptr<FeatureId> resolveFeatureIdNode(simfil::ModelNode const& node) const = 0;

    /** Resolve a point node from the concrete layer's columns. */
    [[nodiscard]] virtual model_ptr<PointNode> resolvePointNode(simfil::ModelNode const& node) const = 0;

    /** Resolve a point-buffer node from the concrete layer's geometry storage. */
    [[nodiscard]] virtual model_ptr<PointBufferNode> resolvePointBufferNode(simfil::ModelNode const& node) const = 0;

    /** Resolve a concrete geometry node from the concrete layer's geometry storage. */
    [[nodiscard]] virtual model_ptr<Geometry> resolveGeometryNode(simfil::ModelNode const& node) const = 0;

    /** Resolve a geometry collection node from array storage. */
    [[nodiscard]] virtual model_ptr<GeometryCollection> resolveGeometryCollectionNode(simfil::ModelNode const& node) const = 0;

    /** Resolve a merged geometry-array view node. */
    [[nodiscard]] virtual model_ptr<GeometryArrayView> resolveGeometryArrayViewNode(simfil::ModelNode const& node) const = 0;

    /** Resolve the GeoJSON-style bounds-info object for one geometry. */
    [[nodiscard]] virtual model_ptr<BoundsInfoNode> resolveBoundsInfoNode(simfil::ModelNode const& node) const = 0;

    /** Resolve the GeoJSON-style bounds coordinate array for one geometry. */
    [[nodiscard]] virtual model_ptr<BoundsPolygonCoordinatesNode> resolveBoundsPolygonCoordinatesNode(simfil::ModelNode const& node) const = 0;

    /** Resolve one GeoJSON bounds ring array. */
    [[nodiscard]] virtual model_ptr<BoundsRingNode> resolveBoundsRingNode(simfil::ModelNode const& node) const = 0;

    /** Resolve one mesh geometry view node. */
    [[nodiscard]] virtual model_ptr<MeshNode> resolveMeshNode(simfil::ModelNode const& node) const = 0;

    /** Resolve one mesh triangle collection node. */
    [[nodiscard]] virtual model_ptr<MeshTriangleCollectionNode> resolveMeshTriangleCollectionNode(simfil::ModelNode const& node) const = 0;

    /** Resolve one polygon linear ring node. */
    [[nodiscard]] virtual model_ptr<LinearRingNode> resolveLinearRingNode(simfil::ModelNode const& node) const = 0;

    /** Resolve one polygon geometry node. */
    [[nodiscard]] virtual model_ptr<PolygonNode> resolvePolygonNode(simfil::ModelNode const& node) const = 0;

    /** Resolve a geometry source-data-reference collection. */
    [[nodiscard]] virtual model_ptr<SourceDataReferenceCollection> resolveSourceDataReferenceCollectionNode(
        simfil::ModelNode const& node) const = 0;

    /** Resolve one geometry source-data-reference item. */
    [[nodiscard]] virtual model_ptr<SourceDataReferenceItem> resolveSourceDataReferenceItemNode(
        simfil::ModelNode const& node) const = 0;

    /** Return an optional common feature-id prefix; search-result layers normally return null. */
    [[nodiscard]] virtual model_ptr<Object> getIdPrefix() const;

    /** Access layer-wide geometry anchor used for anchor-relative vertex encoding. */
    [[nodiscard]] virtual Point geometryAnchor() const = 0;

    /** Link a merged array to an extension array in another compatible layer. */
    virtual void setMergedArrayExtension(
        simfil::ModelNodeAddress baseAddress,
        TileFeatureModelLayerBase const* extensionModel,
        simfil::ModelNodeAddress extensionAddress);

    /** Clear a previously configured merged-array extension link. */
    virtual void clearMergedArrayExtension(simfil::ModelNodeAddress baseAddress);

    /** Return the extension link for a merged array, if one has been configured. */
    [[nodiscard]] virtual std::optional<std::pair<TileFeatureModelLayerBase const*, simfil::ModelNodeAddress>>
    mergedArrayExtension(simfil::ModelNodeAddress baseAddress) const;

protected:
    TileFeatureModelLayerBase(
        TileId tileId,
        std::string const& nodeId,
        std::string const& mapId,
        std::shared_ptr<LayerInfo> const& layerInfo,
        std::shared_ptr<simfil::StringPool> const& strings);

    TileFeatureModelLayerBase(
        std::vector<uint8_t> const& input,
        LayerInfoResolveFun const& layerInfoResolveFun,
        StringPoolResolveFun const& stringPoolGetter,
        size_t* bytesRead = nullptr);

    /** Access the concrete vertex buffer arena backing geometry nodes. */
    virtual GeometryStorage& vertexBufferStorage() = 0;

    /** Return geometry-view metadata for a stored geometry view address. */
    [[nodiscard]] virtual GeometryViewData const* geometryViewData(simfil::ModelNodeAddress address) const = 0;

    /** Return the persisted geometry stage for one geometry address. */
    [[nodiscard]] virtual std::optional<uint8_t> geometryStage(simfil::ModelNodeAddress address) const = 0;

    /** Store or clear the persisted geometry stage for one geometry address. */
    virtual void setGeometryStage(simfil::ModelNodeAddress address, std::optional<uint8_t> stage) = 0;

    /** Return the source-data-reference collection address attached to one geometry. */
    [[nodiscard]] virtual simfil::ModelNodeAddress geometrySourceDataReferences(
        simfil::ModelNodeAddress address) const = 0;

    /** Attach source-data references to one geometry. */
    virtual void setGeometrySourceDataReferences(
        simfil::ModelNodeAddress address,
        simfil::ModelNodeAddress refsAddress) = 0;
};

// Primary template for ADL-based resolve hooks shared by feature and search-result layers.
template<typename Target>
simfil::model_ptr<Target> resolveInternal(
    simfil::res::tag<Target>,
    TileFeatureModelLayerBase const& model,
    simfil::ModelNode const& node);

} // namespace mapget
