#include "featuremodellayer.h"

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>
#include <type_traits>

#include "featureid.h"
#include "geometry.h"
#include "mapget/log.h"
#include "pointnode.h"
#include "sourcedatareference.h"

namespace mapget
{
namespace
{

using GeometryPointBufferArena = TileFeatureModelLayerBase::GeometryStorage;

constexpr uint32_t SourceAddressArenaIndexBits = 20;
constexpr uint32_t SourceAddressArenaIndexMax = (~static_cast<uint32_t>(0)) >> (32 - SourceAddressArenaIndexBits);
constexpr uint32_t SourceAddressArenaSizeBits = 4;
constexpr uint32_t SourceAddressArenaSizeMax = (~static_cast<uint32_t>(0)) >> (32 - SourceAddressArenaSizeBits);
constexpr uint8_t UnnamedGeometry = 0;

std::tuple<size_t, size_t> modelAddressToSourceDataAddressList(uint32_t addr)
{
    auto const index = addr >> SourceAddressArenaSizeBits;
    auto const size = addr & SourceAddressArenaSizeMax;
    return {index, size};
}

uint32_t sourceDataAddressListToModelAddress(uint32_t index, uint32_t size)
{
    if (index > SourceAddressArenaIndexMax) {
        raiseFmt("Source-data reference index {} is out of range.", index);
    }
    if (size > SourceAddressArenaSizeMax) {
        raiseFmt("Source-data reference list size {} is out of range.", size);
    }
    return (index << SourceAddressArenaSizeBits) | size;
}

bool isBufferedGeometryColumn(uint8_t column)
{
    using Col = TileFeatureModelLayerBase::ColumnId;
    return column == Col::LineGeometries ||
           column == Col::PolygonGeometries ||
           column == Col::MeshGeometries ||
           column == Col::AabbGeometries ||
           column == Col::GltfNodeIndexGeometries;
}

bool isBaseGeometryColumn(uint8_t column)
{
    using Col = TileFeatureModelLayerBase::ColumnId;
    return column == Col::PointGeometries ||
           column == Col::GltfNodeIndexGeometries ||
           isBufferedGeometryColumn(column);
}

void ensureGeometrySourceRefCapacity(
    simfil::ModelColumn<simfil::ModelNodeAddress, simfil::detail::ColumnPageSize / 2>& refs,
    simfil::ArrayIndex index)
{
    if (index == simfil::InvalidArrayIndex) {
        raiseFmt("Invalid geometry buffer index {}.", index);
    }
    while (refs.size() <= static_cast<size_t>(index)) {
        refs.emplace_back(simfil::ModelNodeAddress{});
    }
}

void ensureGeometryNameCapacity(
    simfil::ModelColumn<uint8_t, simfil::detail::ColumnPageSize>& names,
    simfil::ArrayIndex index)
{
    if (index == simfil::InvalidArrayIndex) {
        raiseFmt("Invalid geometry buffer index {}.", index);
    }
    while (names.size() <= static_cast<size_t>(index)) {
        names.emplace_back(UnnamedGeometry);
    }
}

void ensurePolygonRingStartRefCapacity(
    simfil::ModelColumn<simfil::ArrayIndex, simfil::detail::ColumnPageSize / 2>& refs,
    simfil::ArrayIndex index)
{
    if (index == simfil::InvalidArrayIndex) {
        raiseFmt("Invalid geometry buffer index {}.", index);
    }
    while (refs.size() <= static_cast<size_t>(index)) {
        refs.emplace_back(simfil::InvalidArrayIndex);
    }
}

uint32_t extraGeometryDataStorageIndex(simfil::ArrayIndex geometryIndex)
{
    if (geometryIndex == simfil::InvalidArrayIndex) {
        raiseFmt("Invalid geometry buffer index {}.", geometryIndex);
    }

    // ArrayArena singleton handles share the same u32 address space as regular
    // array handles. Geometry metadata uses an even/odd remapping to keep those
    // two domains collision-free without allocating a sparse side table.
    if (GeometryPointBufferArena::is_singleton_handle(geometryIndex)) {
        return GeometryPointBufferArena::singleton_payload(geometryIndex) * 2U + 1U;
    }
    return geometryIndex * 2U;
}

simfil::ModelNodeAddress geometrySourceRefsAt(
    simfil::ModelColumn<simfil::ModelNodeAddress, simfil::detail::ColumnPageSize / 2> const& refs,
    uint32_t index)
{
    if (index < refs.size()) {
        return refs.at(index);
    }
    return {};
}

simfil::ArrayIndex polygonRingStartRefAt(
    simfil::ModelColumn<simfil::ArrayIndex, simfil::detail::ColumnPageSize / 2> const& refs,
    uint32_t index)
{
    if (index < refs.size()) {
        return refs.at(index);
    }
    return simfil::InvalidArrayIndex;
}

uint8_t geometryNameIndexAt(
    simfil::ModelColumn<uint8_t, simfil::detail::ColumnPageSize> const& names,
    uint32_t index)
{
    return index < names.size() ? names.at(index) : UnnamedGeometry;
}

} // namespace

TileFeatureModelLayerBase::TileFeatureModelLayerBase(
    TileId tileId,
    std::string const& stringPoolId,
    std::string const& mapId,
    std::shared_ptr<LayerInfo> const& layerInfo,
    std::shared_ptr<simfil::StringPool> const& strings)
    : TileLayer(tileId, stringPoolId, mapId, layerInfo),
      simfil::ModelPool(strings)
{
}

TileFeatureModelLayerBase::TileFeatureModelLayerBase(
    std::vector<uint8_t> const& input,
    LayerInfoResolveFun const& layerInfoResolveFun,
    StringPoolResolveFun const& stringPoolGetter,
    size_t* bytesRead)
    : TileLayer(input, layerInfoResolveFun, bytesRead),
      simfil::ModelPool(stringPoolGetter(stringPoolId_))
{
}

model_ptr<Object> TileFeatureModelLayerBase::getIdPrefix() const
{
    return {};
}

model_ptr<FeatureId> TileFeatureModelLayerBase::resolveFeatureIdNode(simfil::ModelNode const&) const
{
    raise("Cannot cast this node to a FeatureId.");
}

model_ptr<PointNode> TileFeatureModelLayerBase::resolvePointNode(simfil::ModelNode const&) const
{
    raise("Cannot cast this node to a Point.");
}

model_ptr<PointBufferNode> TileFeatureModelLayerBase::resolvePointBufferNode(simfil::ModelNode const&) const
{
    raise("Cannot cast this node to a PointBuffer.");
}

model_ptr<Geometry> TileFeatureModelLayerBase::resolveGeometryNode(simfil::ModelNode const&) const
{
    raise("Cannot cast this node to a Geometry.");
}

model_ptr<GeometryCollection> TileFeatureModelLayerBase::resolveGeometryCollectionNode(simfil::ModelNode const&) const
{
    raise("Cannot cast this node to a GeometryCollection.");
}

model_ptr<GeometryArrayView> TileFeatureModelLayerBase::resolveGeometryArrayViewNode(simfil::ModelNode const&) const
{
    raise("Cannot cast this node to a GeometryArrayView.");
}

model_ptr<BoundsInfoNode> TileFeatureModelLayerBase::resolveBoundsInfoNode(simfil::ModelNode const&) const
{
    raise("Cannot cast this node to BoundsInfo.");
}

model_ptr<BoundsPolygonCoordinatesNode> TileFeatureModelLayerBase::resolveBoundsPolygonCoordinatesNode(
    simfil::ModelNode const&) const
{
    raise("Cannot cast this node to BoundsPolygonCoordinates.");
}

model_ptr<BoundsRingNode> TileFeatureModelLayerBase::resolveBoundsRingNode(simfil::ModelNode const&) const
{
    raise("Cannot cast this node to BoundsRing.");
}

model_ptr<MeshNode> TileFeatureModelLayerBase::resolveMeshNode(simfil::ModelNode const&) const
{
    raise("Cannot cast this node to a Mesh.");
}

model_ptr<MeshTriangleCollectionNode> TileFeatureModelLayerBase::resolveMeshTriangleCollectionNode(
    simfil::ModelNode const&) const
{
    raise("Cannot cast this node to a MeshTriangleCollection.");
}

model_ptr<LinearRingNode> TileFeatureModelLayerBase::resolveLinearRingNode(simfil::ModelNode const&) const
{
    raise("Cannot cast this node to a LinearRing.");
}

model_ptr<PolygonNode> TileFeatureModelLayerBase::resolvePolygonNode(simfil::ModelNode const&) const
{
    raise("Cannot cast this node to a Polygon.");
}

model_ptr<SourceDataReferenceCollection> TileFeatureModelLayerBase::resolveSourceDataReferenceCollectionNode(
    simfil::ModelNode const&) const
{
    raise("Cannot cast this node to a SourceDataReferenceCollection.");
}

model_ptr<SourceDataReferenceItem> TileFeatureModelLayerBase::resolveSourceDataReferenceItemNode(
    simfil::ModelNode const&) const
{
    raise("Cannot cast this node to a SourceDataReferenceItem.");
}

TileFeatureModelLayerBase::GeometryStorage& TileFeatureModelLayerBase::vertexBufferStorage()
{
    return pointBuffers_;
}

uint64_t TileFeatureModelLayerBase::geometryVertexCount() const
{
    uint64_t result = 0;
    for (auto const& buffer : pointBuffers_) {
        result += buffer.size();
    }
    return result;
}

GeometryViewData const* TileFeatureModelLayerBase::geometryViewData(simfil::ModelNodeAddress address) const
{
    if (address.column() != ColumnId::GeometryViews || address.index() >= geomViews_.size()) {
        return nullptr;
    }
    return &geomViews_.at(address.index());
}

std::optional<std::string_view> TileFeatureModelLayerBase::geometryName(
    simfil::ModelNodeAddress address) const
{
    if (!isBaseGeometryColumn(address.column()) && address.column() != ColumnId::GeometryViews) {
        return std::nullopt;
    }
    auto const storageIndex = address.column() == ColumnId::GeometryViews
        ? address.index()
        : extraGeometryDataStorageIndex(static_cast<simfil::ArrayIndex>(address.index()));
    auto const storedIndex = geometryNameIndexAt(geomNameIndices_, storageIndex);
    if (storedIndex != UnnamedGeometry) {
        return decodeGeometryName(storedIndex);
    }
    if (address.column() == ColumnId::GeometryViews) {
        return geometryName(geomViews_.at(address.index()).baseGeometry_);
    }
    return std::nullopt;
}

void TileFeatureModelLayerBase::setGeometryName(
    simfil::ModelNodeAddress address,
    std::optional<std::string_view> name)
{
    if (!isBaseGeometryColumn(address.column()) && address.column() != ColumnId::GeometryViews) {
        raise("Geometry name can only be stored on geometry nodes.");
    }
    auto const storageIndex = address.column() == ColumnId::GeometryViews
        ? address.index()
        : extraGeometryDataStorageIndex(static_cast<simfil::ArrayIndex>(address.index()));
    ensureGeometryNameCapacity(geomNameIndices_, storageIndex);
    geomNameIndices_.at(storageIndex) = encodeGeometryName(name);
}

uint8_t TileFeatureModelLayerBase::encodeGeometryName(
    std::optional<std::string_view> name)
{
    if (!name) {
        return UnnamedGeometry;
    }
    if (name->empty()) {
        raise("Geometry names must not be empty; clear the name instead.");
    }
    auto const found = std::find(geometryNames_.begin(), geometryNames_.end(), *name);
    if (found != geometryNames_.end()) {
        return static_cast<uint8_t>(
            std::distance(geometryNames_.begin(), found) + 1);
    }
    if (geometryNames_.size() >= std::numeric_limits<uint8_t>::max()) {
        raise("A tile layer cannot contain more than 255 distinct geometry names.");
    }
    geometryNames_.emplace_back(*name);
    return static_cast<uint8_t>(geometryNames_.size());
}

std::optional<std::string_view> TileFeatureModelLayerBase::decodeGeometryName(
    uint8_t index) const
{
    if (index == UnnamedGeometry) {
        return std::nullopt;
    }
    auto const tableIndex = static_cast<size_t>(index - 1U);
    if (tableIndex >= geometryNames_.size()) {
        raiseFmt(
            "Geometry name index {} is invalid for a table with {} entries.",
            index,
            geometryNames_.size());
    }
    return geometryNames_[tableIndex];
}

void TileFeatureModelLayerBase::validateGeometryNameStorage() const
{
    std::set<std::string_view> uniqueNames;
    for (auto const& name : geometryNames_) {
        if (name.empty()) {
            raise("Geometry name table contains an empty name.");
        }
        if (!uniqueNames.insert(name).second) {
            raiseFmt("Geometry name table contains duplicate name '{}'.", name);
        }
    }
    for (size_t slot = 0; slot < geomNameIndices_.size(); ++slot) {
        auto const index = geomNameIndices_.at(slot);
        if (index != UnnamedGeometry &&
            static_cast<size_t>(index - 1U) >= geometryNames_.size())
        {
            raiseFmt(
                "Geometry metadata slot {} references invalid name index {}.",
                slot,
                index);
        }
    }
}

simfil::ModelNodeAddress TileFeatureModelLayerBase::geometrySourceDataReferences(
    simfil::ModelNodeAddress address) const
{
    if (address.column() == ColumnId::GeometryViews) {
        return geomViews_.at(address.index()).sourceDataReferences_;
    }
    if (!isBaseGeometryColumn(address.column())) {
        return {};
    }
    return geometrySourceRefsAt(
        geomSourceDataRefs_,
        extraGeometryDataStorageIndex(static_cast<simfil::ArrayIndex>(address.index())));
}

void TileFeatureModelLayerBase::setGeometrySourceDataReferences(
    simfil::ModelNodeAddress address,
    simfil::ModelNodeAddress refsAddress)
{
    if (address.column() == ColumnId::GeometryViews) {
        geomViews_.at(address.index()).sourceDataReferences_ = refsAddress;
        return;
    }
    if (!isBaseGeometryColumn(address.column())) {
        raise("Source data references can only be stored on geometry nodes.");
    }
    auto const storageIndex = extraGeometryDataStorageIndex(static_cast<simfil::ArrayIndex>(address.index()));
    ensureGeometrySourceRefCapacity(geomSourceDataRefs_, storageIndex);
    geomSourceDataRefs_.at(storageIndex) = refsAddress;
}

uint32_t TileFeatureModelLayerBase::polygonRingCount(simfil::ModelNodeAddress address) const
{
    if (address.column() != ColumnId::PolygonGeometries) {
        return 0;
    }

    auto const storageIndex = extraGeometryDataStorageIndex(static_cast<simfil::ArrayIndex>(address.index()));
    auto const ringStarts = polygonRingStartRefAt(polygonRingStartRefs_, storageIndex);
    if (ringStarts == simfil::InvalidArrayIndex) {
        return pointBuffers_.size(static_cast<simfil::ArrayIndex>(address.index())) == 0 ? 0U : 1U;
    }
    return polygonRingStarts_.size(ringStarts);
}

uint32_t TileFeatureModelLayerBase::polygonRingStart(
    simfil::ModelNodeAddress address,
    uint32_t ringIndex) const
{
    if (address.column() != ColumnId::PolygonGeometries) {
        raise("Polygon ring starts are only available for polygon geometries.");
    }
    auto const ringCount = polygonRingCount(address);
    if (ringIndex >= ringCount) {
        raiseFmt("Polygon ring index {} is out of range for {} rings.", ringIndex, ringCount);
    }
    auto const storageIndex = extraGeometryDataStorageIndex(static_cast<simfil::ArrayIndex>(address.index()));
    auto const ringStarts = polygonRingStartRefAt(polygonRingStartRefs_, storageIndex);
    if (ringStarts == simfil::InvalidArrayIndex) {
        return 0;
    }
    auto const start = polygonRingStarts_.at(ringStarts, ringIndex);
    if (!start) {
        raiseFmt("Polygon ring start {} is missing.", ringIndex);
    }
    return *start;
}

uint32_t TileFeatureModelLayerBase::polygonRingEnd(
    simfil::ModelNodeAddress address,
    uint32_t ringIndex) const
{
    if (address.column() != ColumnId::PolygonGeometries) {
        raise("Polygon ring ends are only available for polygon geometries.");
    }
    auto const ringCount = polygonRingCount(address);
    if (ringIndex >= ringCount) {
        raiseFmt("Polygon ring index {} is out of range for {} rings.", ringIndex, ringCount);
    }
    if (ringIndex + 1U < ringCount) {
        return polygonRingStart(address, ringIndex + 1U);
    }
    return pointBuffers_.size(static_cast<simfil::ArrayIndex>(address.index()));
}

void TileFeatureModelLayerBase::setPolygonRingStarts(
    simfil::ModelNodeAddress address,
    std::span<uint32_t const> ringStarts)
{
    if (address.column() != ColumnId::PolygonGeometries) {
        raise("Polygon ring starts can only be stored on polygon geometries.");
    }

    auto const pointCount = pointBuffers_.size(static_cast<simfil::ArrayIndex>(address.index()));
    if (ringStarts.empty()) {
        raise("Polygon ring starts must contain at least the outer ring start.");
    }
    if (ringStarts.front() != 0U) {
        raise("Polygon ring starts must begin with vertex zero.");
    }
    for (size_t i = 0; i < ringStarts.size(); ++i) {
        if (ringStarts[i] >= pointCount) {
            raiseFmt(
                "Polygon ring start {} is outside the {}-vertex polygon.",
                ringStarts[i],
                pointCount);
        }
        if (i > 0 && ringStarts[i] <= ringStarts[i - 1]) {
            raise("Polygon ring starts must be strictly increasing.");
        }
    }

    auto const storageIndex = extraGeometryDataStorageIndex(static_cast<simfil::ArrayIndex>(address.index()));
    ensurePolygonRingStartRefCapacity(polygonRingStartRefs_, storageIndex);
    if (ringStarts.size() <= 1) {
        polygonRingStartRefs_.at(storageIndex) = simfil::InvalidArrayIndex;
        return;
    }

    auto const ringStartArray = polygonRingStarts_.new_array(ringStarts.size(), true);
    for (auto const start : ringStarts) {
        polygonRingStarts_.emplace_back(ringStartArray, start);
    }
    polygonRingStartRefs_.at(storageIndex) = ringStartArray;
}

simfil::ModelNodeAddress TileFeatureModelLayerBase::appendFeatureId(FeatureIdData data)
{
    auto const index = static_cast<uint32_t>(featureIds_.size());
    featureIds_.emplace_back(std::move(data));
    return {ColumnId::ExternalFeatureIds, index};
}

simfil::ModelNodeAddress TileFeatureModelLayerBase::appendGeometryView(GeometryViewData data)
{
    auto const index = static_cast<uint32_t>(geomViews_.size());
    geomViews_.emplace_back(std::move(data));
    return {ColumnId::GeometryViews, index};
}

simfil::ModelNodeAddress TileFeatureModelLayerBase::appendSourceDataReferences(
    std::span<QualifiedSourceDataReference> list)
{
    auto const index = static_cast<uint32_t>(sourceDataReferences_.size());
    auto const size = static_cast<uint32_t>(list.size());
    sourceDataReferences_.insert(sourceDataReferences_.end(), list.begin(), list.end());
    return {
        ColumnId::SourceDataReferenceCollections,
        sourceDataAddressListToModelAddress(index, size)};
}

using simfil::ModelNode;
using simfil::res::tag;

template<>
model_ptr<FeatureId> resolveInternal(tag<FeatureId>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    if (node.addr().column() == TileFeatureModelLayerBase::ColumnId::ExternalFeatureIds) {
        return FeatureId(
            model.featureIds_.at(node.addr().index()),
            model.shared_from_this(),
            node.addr(),
            model.mpKey_);
    }
    return model.resolveFeatureIdNode(node);
}

template<>
model_ptr<PointNode> resolveInternal(tag<PointNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    switch (node.addr().column()) {
    case TileFeatureModelLayerBase::ColumnId::Points:
        return PointNode(node, static_cast<simfil::ArrayIndex>(node.addr().index()), model.mpKey_);
    case TileFeatureModelLayerBase::ColumnId::GeometryPointView:
        return PointNode(node, model.mpKey_);
    default:
        break;
    }
    return model.resolvePointNode(node);
}

template<>
model_ptr<PointBufferNode> resolveInternal(tag<PointBufferNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    if (auto existing = dynamic_cast<PointBufferNode const*>(&node)) {
        return PointBufferNode(model.shared_from_this(), existing->baseGeometryAddress(), model.mpKey_);
    }
    switch (node.addr().column()) {
    case TileFeatureModelLayerBase::ColumnId::PointBuffers:
        return PointBufferNode(
            model.shared_from_this(),
            simfil::ModelNodeAddress{
                TileFeatureModelLayerBase::ColumnId::PointGeometries,
                node.addr().index()},
            model.mpKey_);
    case TileFeatureModelLayerBase::ColumnId::PointBuffersView:
        return PointBufferNode(
            model.shared_from_this(),
            simfil::ModelNodeAddress{
                TileFeatureModelLayerBase::ColumnId::GeometryViews,
                node.addr().index()},
            model.mpKey_);
    default:
        raise("Cannot cast this node to a PointBuffer.");
    }
}

template<>
model_ptr<Geometry> resolveInternal(tag<Geometry>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    switch (node.addr().column()) {
    case TileFeatureModelLayerBase::ColumnId::PointGeometries:
    case TileFeatureModelLayerBase::ColumnId::LineGeometries:
    case TileFeatureModelLayerBase::ColumnId::PolygonGeometries:
    case TileFeatureModelLayerBase::ColumnId::MeshGeometries:
    case TileFeatureModelLayerBase::ColumnId::AabbGeometries:
    case TileFeatureModelLayerBase::ColumnId::GltfNodeIndexGeometries:
        return Geometry(model.shared_from_this(), node.addr(), model.mpKey_);
    case TileFeatureModelLayerBase::ColumnId::GeometryViews: {
        auto* geomData = &model.geomViews_.at(node.addr().index());
        using MutableGeomData = std::remove_const_t<std::remove_reference_t<decltype(*geomData)>>;
        return Geometry(
            const_cast<MutableGeomData*>(geomData),
            model.shared_from_this(),
            node.addr(),
            model.mpKey_);
    }
    default:
        raise("Cannot cast this node to a Geometry.");
    }
}

template<>
model_ptr<GeometryCollection> resolveInternal(tag<GeometryCollection>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureModelLayerBase::ColumnId::GeometryCollections &&
        node.addr().column() != TileFeatureModelLayerBase::ColumnId::FeatureGeometryCollectionView &&
        !isBaseGeometryColumn(node.addr().column()) &&
        node.addr().column() != TileFeatureModelLayerBase::ColumnId::GeometryViews) {
        raise("Cannot cast this node to a GeometryCollection.");
    }
    return GeometryCollection(model.shared_from_this(), node.addr(), model.mpKey_);
}

template<>
model_ptr<GeometryArrayView> resolveInternal(tag<GeometryArrayView>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureModelLayerBase::ColumnId::GeometryArrayView &&
        node.addr().column() != TileFeatureModelLayerBase::ColumnId::FeatureGeometryArrayView) {
        raise("Cannot cast this node to a GeometryArrayView.");
    }
    return GeometryArrayView(model.shared_from_this(), node.addr(), model.mpKey_);
}

template<>
model_ptr<BoundsInfoNode> resolveInternal(tag<BoundsInfoNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureModelLayerBase::ColumnId::GeometryBoundsInfoView) {
        raise("Cannot cast this node to BoundsInfo.");
    }
    return BoundsInfoNode(node, model.mpKey_);
}

template<>
model_ptr<BoundsPolygonCoordinatesNode> resolveInternal(
    tag<BoundsPolygonCoordinatesNode>,
    TileFeatureModelLayerBase const& model,
    ModelNode const& node)
{
    if (node.addr().column() != TileFeatureModelLayerBase::ColumnId::GeometryBoundsPolygonCoordinatesView) {
        raise("Cannot cast this node to BoundsPolygonCoordinates.");
    }
    return BoundsPolygonCoordinatesNode(node, model.mpKey_);
}

template<>
model_ptr<BoundsRingNode> resolveInternal(tag<BoundsRingNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureModelLayerBase::ColumnId::GeometryBoundsRingView) {
        raise("Cannot cast this node to BoundsRing.");
    }
    return BoundsRingNode(node, model.mpKey_);
}

template<>
model_ptr<MeshNode> resolveInternal(tag<MeshNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    return MeshNode(model.shared_from_this(), node.addr(), model.mpKey_);
}

template<>
model_ptr<MeshTriangleCollectionNode> resolveInternal(
    tag<MeshTriangleCollectionNode>,
    TileFeatureModelLayerBase const& model,
    ModelNode const& node)
{
    return MeshTriangleCollectionNode(node, model.mpKey_);
}

template<>
model_ptr<LinearRingNode> resolveInternal(tag<LinearRingNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    switch (node.addr().column()) {
    case TileFeatureModelLayerBase::ColumnId::LinearRing:
        return LinearRingNode(node, model.mpKey_);
    case TileFeatureModelLayerBase::ColumnId::MeshTriangleLinearRing:
        return LinearRingNode(node, 3, model.mpKey_);
    default:
        raise("Cannot cast this node to a LinearRing.");
    }
}

template<>
model_ptr<PolygonNode> resolveInternal(tag<PolygonNode>, TileFeatureModelLayerBase const& model, ModelNode const& node)
{
    return PolygonNode(model.shared_from_this(), node.addr(), model.mpKey_);
}

template<>
model_ptr<SourceDataReferenceCollection> resolveInternal(
    tag<SourceDataReferenceCollection>,
    TileFeatureModelLayerBase const& model,
    ModelNode const& node)
{
    if (node.addr().column() != TileFeatureModelLayerBase::ColumnId::SourceDataReferenceCollections) {
        raise("Cannot cast this node to a SourceDataReferenceCollection.");
    }
    auto [index, size] = modelAddressToSourceDataAddressList(node.addr().index());
    return SourceDataReferenceCollection(index, size, model.shared_from_this(), node.addr(), model.mpKey_);
}

template<>
model_ptr<SourceDataReferenceItem> resolveInternal(
    tag<SourceDataReferenceItem>,
    TileFeatureModelLayerBase const& model,
    ModelNode const& node)
{
    if (node.addr().column() != TileFeatureModelLayerBase::ColumnId::SourceDataReferences) {
        raise("Cannot cast this node to a SourceDataReferenceItem.");
    }
    auto const* data = &model.sourceDataReferences_.at(node.addr().index());
    return SourceDataReferenceItem(data, model.shared_from_this(), node.addr(), model.mpKey_);
}

} // namespace mapget
