#include "featuremodellayer.h"

#include "featureid.h"
#include "geometry.h"
#include "pointnode.h"
#include "sourcedatareference.h"

namespace mapget
{

TileFeatureModelLayerBase::TileFeatureModelLayerBase(
    TileId tileId,
    std::string const& nodeId,
    std::string const& mapId,
    std::shared_ptr<LayerInfo> const& layerInfo,
    std::shared_ptr<simfil::StringPool> const& strings)
    : TileLayer(tileId, nodeId, mapId, layerInfo),
      simfil::ModelPool(strings)
{
}

TileFeatureModelLayerBase::TileFeatureModelLayerBase(
    std::vector<uint8_t> const& input,
    LayerInfoResolveFun const& layerInfoResolveFun,
    StringPoolResolveFun const& stringPoolGetter,
    size_t* bytesRead)
    : TileLayer(input, layerInfoResolveFun, bytesRead),
      simfil::ModelPool(stringPoolGetter(nodeId_))
{
}

model_ptr<Object> TileFeatureModelLayerBase::getIdPrefix() const
{
    return {};
}

void TileFeatureModelLayerBase::setMergedArrayExtension(
    simfil::ModelNodeAddress /*baseAddress*/,
    TileFeatureModelLayerBase const* /*extensionModel*/,
    simfil::ModelNodeAddress /*extensionAddress*/)
{
    // Search-result layers do not use overlay chains. Feature layers override this.
}

void TileFeatureModelLayerBase::clearMergedArrayExtension(simfil::ModelNodeAddress /*baseAddress*/)
{
    // Search-result layers do not use overlay chains. Feature layers override this.
}

std::optional<std::pair<TileFeatureModelLayerBase const*, simfil::ModelNodeAddress>>
TileFeatureModelLayerBase::mergedArrayExtension(simfil::ModelNodeAddress /*baseAddress*/) const
{
    return {};
}

using simfil::ModelNode;
using simfil::res::tag;

template<>
model_ptr<FeatureId> resolveInternal(tag<FeatureId>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    return model.resolveFeatureIdNode(node);
}

template<>
model_ptr<PointNode> resolveInternal(tag<PointNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    return model.resolvePointNode(node);
}

template<>
model_ptr<PointBufferNode> resolveInternal(tag<PointBufferNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    return model.resolvePointBufferNode(node);
}

template<>
model_ptr<Geometry> resolveInternal(tag<Geometry>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    return model.resolveGeometryNode(node);
}

template<>
model_ptr<GeometryCollection> resolveInternal(tag<GeometryCollection>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    return model.resolveGeometryCollectionNode(node);
}

template<>
model_ptr<GeometryArrayView> resolveInternal(tag<GeometryArrayView>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    return model.resolveGeometryArrayViewNode(node);
}

template<>
model_ptr<BoundsInfoNode> resolveInternal(tag<BoundsInfoNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    return model.resolveBoundsInfoNode(node);
}

template<>
model_ptr<BoundsPolygonCoordinatesNode> resolveInternal(
    tag<BoundsPolygonCoordinatesNode>,
    TileFeatureModelLayerBase const& model,
    ModelNode const& node)
{
    return model.resolveBoundsPolygonCoordinatesNode(node);
}

template<>
model_ptr<BoundsRingNode> resolveInternal(tag<BoundsRingNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    return model.resolveBoundsRingNode(node);
}

template<>
model_ptr<MeshNode> resolveInternal(tag<MeshNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    return model.resolveMeshNode(node);
}

template<>
model_ptr<MeshTriangleCollectionNode> resolveInternal(
    tag<MeshTriangleCollectionNode>,
    TileFeatureModelLayerBase const& model,
    ModelNode const& node)
{
    return model.resolveMeshTriangleCollectionNode(node);
}

template<>
model_ptr<LinearRingNode> resolveInternal(tag<LinearRingNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    return model.resolveLinearRingNode(node);
}

template<>
model_ptr<PolygonNode> resolveInternal(tag<PolygonNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    return model.resolvePolygonNode(node);
}

template<>
model_ptr<SourceDataReferenceCollection> resolveInternal(
    tag<SourceDataReferenceCollection>,
    TileFeatureModelLayerBase const& model,
    ModelNode const& node)
{
    return model.resolveSourceDataReferenceCollectionNode(node);
}

template<>
model_ptr<SourceDataReferenceItem> resolveInternal(
    tag<SourceDataReferenceItem>,
    TileFeatureModelLayerBase const& model,
    ModelNode const& node)
{
    return model.resolveSourceDataReferenceItemNode(node);
}

} // namespace mapget
