#include "geometry.h"
#include "feature.h"
#include "featurelayer.h"
#include "simfil/model/nodes.h"
#include "simfil/model/string-pool.h"
#include "sourcedatareference.h"
#include "stringpool.h"
#include "pointnode.h"
#include "hash.h"
#include "mapget/log.h"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <variant>

static const std::string_view GeometryCollectionStr("GeometryCollection");
static const std::string_view MultiPointStr("MultiPoint");
static const std::string_view LineStringStr("LineString");
static const std::string_view PolygonStr("Polygon");
static const std::string_view MultiPolygonStr("MultiPolygon");

namespace
{
/** Project a point onto a finite line segment in the x/y plane. */
std::optional<glm::dvec3>
projectPointOnLine(const glm::dvec3& point, const glm::dvec3& a, const glm::dvec3& b)
{
    // Check if A and B are the same point (zero-length line segment).
    if (a == b) {
        return std::nullopt; // Projection is undefined for a zero-length line segment.
    }

    // Calculate 2d vectors AB and AP (from A to Point).
    glm::dvec2 AB = b - a;
    glm::dvec2 AP = point - a;

    // Calculate the squared length of AB to check for numerical stability.
    double lengthSquaredAB = glm::dot(AB, AB);
    if (lengthSquaredAB == 0.0) {
        return std::nullopt; // Avoid division by zero.
    }

    // Project AP onto AB using dot product.
    double dotProduct = glm::dot(AP, AB);
    double projectionFactor = dotProduct / lengthSquaredAB;

    // Check if projection extends beyond A or B.
    if (projectionFactor < 0 || projectionFactor > 1) {
        return std::nullopt; // Projection outside the segment AB.
    }

    // Calculate the 3d projection point.
    return projectionFactor * (b - a);
}
}

namespace mapget
{

using namespace simfil;

namespace
{
/** Resolve the feature root that owns a feature-scoped procedural view. */
model_ptr<Feature> resolveFeatureByRootIndex(TileFeatureLayer const& model, uint32_t index)
{
    auto rootResult = model.root(index);
    if (!rootResult || !*rootResult) {
        return {};
    }
    return model.resolve<Feature>(**rootResult);
}

/** Check whether a model column stores an actual geometry payload. */
bool isBaseGeometryColumn(uint8_t column)
{
    using Col = TileFeatureModelLayerBase::ColumnId;
    return column == Col::PointGeometries ||
           column == Col::LineGeometries ||
           column == Col::PolygonGeometries ||
           column == Col::MeshGeometries ||
           column == Col::AabbGeometries ||
           column == Col::GltfNodeIndexGeometries;
}

/** Map storage columns back to their logical geometry type. */
GeomType geometryTypeForColumn(uint8_t column)
{
    using Col = TileFeatureModelLayerBase::ColumnId;
    switch (column) {
    case Col::PointGeometries:
        return GeomType::Points;
    case Col::LineGeometries:
        return GeomType::Line;
    case Col::PolygonGeometries:
        return GeomType::Polygon;
    case Col::MeshGeometries:
        return GeomType::Mesh;
    case Col::AabbGeometries:
        return GeomType::AABB;
    case Col::GltfNodeIndexGeometries:
        return GeomType::GltfNodeIndex;
    default:
        raiseFmt("Unexpected geometry column {}.", column);
        return GeomType::Points;
    }
}

/** Map a stored geometry stage back to the optional exported `geometryName`. */
std::optional<std::string_view> geometryNameForStage(
    TileFeatureModelLayerBase const& model,
    std::optional<uint32_t> geometryStage)
{
    if (!geometryStage || !model.layerInfo()) {
        return std::nullopt;
    }
    auto const& layerInfo = *model.layerInfo();
    if (*geometryStage <= layerInfo.highFidelityStage_) {
        // The default high-fidelity stage is represented by the absence of a
        // label so generic GeoJSON stays uncluttered.
        return std::nullopt;
    }
    if (*geometryStage >= layerInfo.stageLabels_.size()) {
        return std::nullopt;
    }
    auto const& label = layerInfo.stageLabels_.at(*geometryStage);
    if (label.empty()) {
        return std::nullopt;
    }
    return label;
}

/** Serialize one 3D coordinate triple in GeoJSON position form. */
nlohmann::json positionJson(Point const& point)
{
    return nlohmann::json::array({point.x, point.y, point.z});
}

/** Build the footprint polygon used to export AABB-like geometries. */
nlohmann::json groundFootprintPolygon(Point const& origin, Point const& size)
{
    auto const minX = origin.x;
    auto const minY = origin.y;
    auto const minZ = origin.z;
    auto const maxX = origin.x + size.x;
    auto const maxY = origin.y + size.y;

    return nlohmann::json::array({
        nlohmann::json::array({
            nlohmann::json::array({minX, minY, minZ}),
            nlohmann::json::array({maxX, minY, minZ}),
            nlohmann::json::array({maxX, maxY, minZ}),
            nlohmann::json::array({minX, maxY, minZ}),
            nlohmann::json::array({minX, minY, minZ}),
        })
    });
}

/** Resolve the origin of an AABB-like geometry regardless of storage flavor. */
Point boundsOrigin(model_ptr<Geometry> const& geometry)
{
    return geometry->geomType() == GeomType::AABB
        ? geometry->aabbOrigin()
        : geometry->gltfNodeAabbOrigin();
}

/** Resolve the size of an AABB-like geometry regardless of storage flavor. */
Point boundsSize(model_ptr<Geometry> const& geometry)
{
    return geometry->geomType() == GeomType::AABB
        ? geometry->aabbSize()
        : geometry->gltfNodeAabbSize();
}

/** Reinterpret a base geometry address in one of the helper-view columns. */
ModelNodeAddress geometryHelperAddress(
    uint8_t helperColumn,
    ModelNodeAddress baseGeometryAddress)
{
    return {helperColumn, baseGeometryAddress.index()};
}

/** Encode the base geometry address into the helper node payload. */
int64_t geometryHelperData(ModelNodeAddress baseGeometryAddress)
{
    return encodeGeometryHelperData(baseGeometryAddress);
}

/** Encode base address plus point-view flavor for helper point nodes. */
int64_t geometryPointHelperData(
    ModelNodeAddress baseGeometryAddress,
    GeometryPointViewKind pointKind)
{
    return encodeGeometryHelperData(baseGeometryAddress, static_cast<uint8_t>(pointKind));
}

/** Create the object view that exposes origin/size for bounds geometries. */
ModelNode::Ptr makeBoundsInfoView(
    TileFeatureModelLayerBase const& model,
    ModelNodeAddress baseGeometryAddress)
{
    return model.resolve(
        geometryHelperAddress(
            TileFeatureModelLayerBase::ColumnId::GeometryBoundsInfoView,
            baseGeometryAddress),
        geometryHelperData(baseGeometryAddress));
}

/** Create the polygon coordinate view used for AABB and GLTF-bounds export. */
ModelNode::Ptr makeBoundsPolygonCoordinatesView(
    TileFeatureModelLayerBase const& model,
    ModelNodeAddress baseGeometryAddress)
{
    return model.resolve(
        geometryHelperAddress(
            TileFeatureModelLayerBase::ColumnId::GeometryBoundsPolygonCoordinatesView,
            baseGeometryAddress),
        geometryHelperData(baseGeometryAddress));
}

/** Create the single ring used by the bounds polygon coordinate view. */
ModelNode::Ptr makeBoundsRingView(
    TileFeatureModelLayerBase const& model,
    ModelNodeAddress baseGeometryAddress)
{
    return model.resolve(
        geometryHelperAddress(
            TileFeatureModelLayerBase::ColumnId::GeometryBoundsRingView,
            baseGeometryAddress),
        geometryHelperData(baseGeometryAddress));
}

/** Create a procedural point view into either a bounds helper or point buffer. */
ModelNode::Ptr makeGeometryPointView(
    TileFeatureModelLayerBase const& model,
    ModelNodeAddress baseGeometryAddress,
    GeometryPointViewKind pointKind)
{
    return model.resolve(
        geometryHelperAddress(
            TileFeatureModelLayerBase::ColumnId::GeometryPointView,
            baseGeometryAddress),
        geometryPointHelperData(baseGeometryAddress, pointKind));
}

constexpr size_t GltfNodeIndexSlot = 0;
constexpr size_t GltfNodeAabbOriginSlot = 1;
constexpr size_t GltfNodeAabbSizeSlot = 2;
constexpr uint32_t MaxExactGltfNodeIndex = 1U << 24;

/** Convert a geometry address to the corresponding point-buffer storage index. */
simfil::ArrayIndex geometryBufferIndex(ModelNodeAddress geometryAddress)
{
    return static_cast<simfil::ArrayIndex>(geometryAddress.index());
}

template <typename StorageType>
/** Ensure GLTF node geometries expose the fixed [index, origin, size] slot layout. */
void ensureGltfNodeStorageInitialized(StorageType& storage, simfil::ArrayIndex arrayIndex)
{
    auto const currentSize = storage.size(arrayIndex);
    if (currentSize == 0) {
        storage.emplace_back(arrayIndex, glm::vec3{0.0F, 0.0F, 0.0F});
        storage.emplace_back(arrayIndex, glm::vec3{0.0F, 0.0F, 0.0F});
        storage.emplace_back(arrayIndex, glm::vec3{0.0F, 0.0F, 0.0F});
        return;
    }
    if (currentSize != 3) {
        raiseFmt("GLTF node geometry expects exactly three stored entries, found {}.", currentSize);
    }
}

}

/** Model node impls. for GeometryCollection */

GeometryCollection::GeometryCollection(ModelConstPtr pool_, ModelNodeAddress a, simfil::detail::mp_key key)
    : MergedArrayView<GeometryCollection, Geometry, TileFeatureModelLayerBase>(std::move(pool_), a, key)
{}

ValueType GeometryCollection::type() const {
    return ValueType::Object;
}

ModelNode::Ptr GeometryCollection::at(int64_t i) const {
    if (auto singleGeomEntry = singleGeom())
        return singleGeomEntry->at(i);
    if (i == 0) return model_ptr<ValueNode>::make(GeometryCollectionStr, model_);
    if (i == 1) return mergedGeometryArray();
    throw std::out_of_range("geom collection: Out of range.");
}

uint32_t GeometryCollection::size() const {
    if (auto singleGeomEntry = singleGeom())
        return singleGeomEntry->size();
    return 2;
}

ModelNode::Ptr GeometryCollection::get(const StringId& f) const {
    if (auto singleGeomEntry = singleGeom())
        return singleGeomEntry->get(f);
    if (f == StringPool::TypeStr) return at(0);
    if (f == StringPool::GeometriesStr) return mergedGeometryArray();
    return {};
}

StringId GeometryCollection::keyAt(int64_t i) const {
    if (auto singleGeomEntry = singleGeom())
        return singleGeomEntry->keyAt(i);
    if (i == 0) return StringPool::TypeStr;
    if (i == 1) return StringPool::GeometriesStr;
    throw std::out_of_range("geom collection: Out of range.");
}

model_ptr<Geometry> GeometryCollection::newGeometry(
    GeomType type,
    size_t initialCapacity,
    bool fixedSize)
{
    if (addr_.column() != TileFeatureModelLayerBase::ColumnId::GeometryCollections) {
        raise("Cannot append to a single-geometry view.");
    }
    auto result = model().newGeometry(type, initialCapacity, fixedSize);
    auto array = model().resolve<simfil::Array>(ModelNodeAddress{simfil::ModelPool::Arrays, addr_.index()});
    array->append(result);
    return result;
}

bool GeometryCollection::iterate(const IterCallback& cb) const
{
    if (auto singleGeomEntry = singleGeom())
        return singleGeomEntry->iterate(cb);
    if (!cb(*at(0))) return false;
    if (!cb(*at(1))) return false;
    return true;
}

ModelNode::Ptr GeometryCollection::singleGeom() const
{
    if (auto ext = mergedExtension(); ext && ext->mergedSize() > 0) {
        return {};
    }

    auto const localAddress = isFeatureScopedView()
        ? featureScopedGeometryAddress()
        : addr_;
    if (!localAddress) {
        return {};
    }
    if (isBaseGeometryColumn(localAddress.column()) ||
        localAddress.column() == TileFeatureModelLayerBase::ColumnId::GeometryViews) {
        return model().resolve(localAddress);
    }
    if (model().arrayMemberStorage().size((ArrayIndex)localAddress.index()) == 1) {
        auto array = model().resolve<simfil::Array>(ModelNodeAddress{simfil::ModelPool::Arrays, localAddress.index()});
        return array->at(0);
    }
    return {};
}

nlohmann::json GeometryCollection::toJson() const
{
    return simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>::toJson();
}

void GeometryCollection::addGeometry(const model_ptr<Geometry>& geom)
{
    if (addr_.column() != TileFeatureModelLayerBase::ColumnId::GeometryCollections) {
        raise("Cannot append to a single-geometry view.");
    }
    auto array = model().resolve<simfil::Array>(ModelNodeAddress{simfil::ModelPool::Arrays, addr_.index()});
    array->append(ModelNode::Ptr(geom));
}

size_t GeometryCollection::numGeometries() const
{
    auto result = localMergedSize();
    if (auto ext = mergedExtension()) {
        result += ext->numGeometries();
    }
    return result;
}

std::optional<uint32_t> GeometryCollection::preferredGeometryStage(
    std::optional<uint32_t> stageOverride) const
{
    if (stageOverride) {
        return stageOverride;
    }

    std::optional<uint32_t> preferredStage;
    forEachGeometry([&](model_ptr<Geometry> const& geom) {
        preferredStage = geom->model().layerInfo()->highFidelityStage_;
        return false;
    });
    return preferredStage;
}

model_ptr<Geometry> GeometryCollection::geometryOfTypeAtPreferredStage(
    GeomType type,
    std::optional<uint32_t> stageOverride) const
{
    auto const preferredStage = preferredGeometryStage(stageOverride);
    model_ptr<Geometry> result;
    if (!preferredStage) {
        return result;
    }

    forEachGeometry([&](model_ptr<Geometry> const& geom) {
        if (geom->geomType() != type) {
            return true;
        }
        auto const geometryStage = geom->stage().value_or(0U);
        if (geometryStage != *preferredStage) {
            return true;
        }
        result = geom;
        return false;
    });
    return result;
}

ModelNode::Ptr GeometryCollection::localGeometryAt(int64_t i) const
{
    if (i < 0) {
        return {};
    }
    auto const localAddress = isFeatureScopedView()
        ? featureScopedGeometryAddress()
        : addr_;
    if (!localAddress) {
        return {};
    }
    if (isBaseGeometryColumn(localAddress.column()) ||
        localAddress.column() == TileFeatureModelLayerBase::ColumnId::GeometryViews) {
        if (i == 0) {
            return model().resolve(localAddress);
        }
        return {};
    }
    auto array = model().resolve<simfil::Array>(ModelNodeAddress{simfil::ModelPool::Arrays, localAddress.index()});
    if (i >= static_cast<int64_t>(array->size())) {
        return {};
    }
    return array->at(i);
}

model_ptr<GeometryArrayView> GeometryCollection::mergedGeometryArray() const
{
    if (isFeatureScopedView()) {
        return model_ptr<GeometryArrayView>::make(
            model_,
            ModelNodeAddress{TileFeatureModelLayerBase::ColumnId::FeatureGeometryArrayView, addr_.index()});
    }

    auto result = (isBaseGeometryColumn(addr_.column()) ||
        addr_.column() == TileFeatureModelLayerBase::ColumnId::GeometryViews)
        ? model_ptr<GeometryArrayView>::make(
            model_,
            ModelNodeAddress{TileFeatureModelLayerBase::ColumnId::GeometryArrayView, addr_.index()},
            addr_)
        : model_ptr<GeometryArrayView>::make(
            model_,
            ModelNodeAddress{TileFeatureModelLayerBase::ColumnId::GeometryArrayView, addr_.index()});
    return result;
}

uint32_t GeometryCollection::localMergedSize() const
{
    auto const localAddress = isFeatureScopedView()
        ? featureScopedGeometryAddress()
        : addr_;
    if (!localAddress) {
        return 0;
    }
    if (isBaseGeometryColumn(localAddress.column()) ||
        localAddress.column() == TileFeatureModelLayerBase::ColumnId::GeometryViews) {
        return 1;
    }
    return model().arrayMemberStorage().size(static_cast<ArrayIndex>(localAddress.index()));
}

ModelNode::Ptr GeometryCollection::localMergedAt(int64_t i) const
{
    return localGeometryAt(i);
}

bool GeometryCollection::localMergedIterate(const IterCallback& cb) const
{
    const auto localCount = localMergedSize();
    for (uint32_t i = 0; i < localCount; ++i) {
        if (auto node = localMergedAt(i)) {
            if (!cb(*node)) {
                return false;
            }
        }
    }
    return true;
}

GeometryCollection::ExtensionPtr GeometryCollection::mergedExtension() const
{
    if (!isFeatureScopedView()) {
        return {};
    }
    auto const* featureLayer = dynamic_cast<TileFeatureLayer const*>(&model());
    if (!featureLayer) {
        return {};
    }
    auto overlay = featureLayer->overlay();
    if (!overlay || addr().index() >= overlay->size()) {
        return {};
    }
    return overlay->resolve<GeometryCollection>(
        ModelNodeAddress{TileFeatureModelLayerBase::ColumnId::FeatureGeometryCollectionView, addr().index()});
}

bool GeometryCollection::isFeatureScopedView() const
{
    return addr_.column() == TileFeatureModelLayerBase::ColumnId::FeatureGeometryCollectionView;
}

model_ptr<Feature> GeometryCollection::featureScopedFeature() const
{
    if (!isFeatureScopedView()) {
        return {};
    }
    auto const* featureLayer = dynamic_cast<TileFeatureLayer const*>(&model());
    if (!featureLayer) {
        return {};
    }
    return resolveFeatureByRootIndex(*featureLayer, addr().index());
}

ModelNodeAddress GeometryCollection::featureScopedGeometryAddress() const
{
    auto feature = featureScopedFeature();
    return feature ? feature->geometryNodeAddress() : ModelNodeAddress{};
}

GeometryArrayView::ExtensionPtr GeometryArrayView::mergedExtension() const
{
    if (!isFeatureScopedView()) {
        return {};
    }
    auto const* featureLayer = dynamic_cast<TileFeatureLayer const*>(&model());
    if (!featureLayer) {
        return {};
    }
    auto overlay = featureLayer->overlay();
    if (!overlay || addr().index() >= overlay->size()) {
        return {};
    }
    return overlay->resolve<GeometryArrayView>(
        ModelNodeAddress{TileFeatureModelLayerBase::ColumnId::FeatureGeometryArrayView, addr().index()});
}

bool GeometryArrayView::isFeatureScopedView() const
{
    return addr_.column() == TileFeatureModelLayerBase::ColumnId::FeatureGeometryArrayView;
}

model_ptr<Feature> GeometryArrayView::featureScopedFeature() const
{
    if (!isFeatureScopedView()) {
        return {};
    }
    auto const* featureLayer = dynamic_cast<TileFeatureLayer const*>(&model());
    if (!featureLayer) {
        return {};
    }
    return resolveFeatureByRootIndex(*featureLayer, addr().index());
}

ModelNodeAddress GeometryArrayView::featureScopedGeometryAddress() const
{
    auto feature = featureScopedFeature();
    return feature ? feature->geometryNodeAddress() : ModelNodeAddress{};
}

uint32_t GeometryArrayView::localMergedSize() const
{
    if (isFeatureScopedView()) {
        auto const localAddress = featureScopedGeometryAddress();
        if (!localAddress) {
            return 0;
        }
        if (isBaseGeometryColumn(localAddress.column()) ||
            localAddress.column() == TileFeatureModelLayerBase::ColumnId::GeometryViews) {
            return 1;
        }
        return model().arrayMemberStorage().size(static_cast<ArrayIndex>(localAddress.index()));
    }
    if (singleGeometryAddress_) {
        return 1;
    }
    return MergedArrayView<GeometryArrayView, Geometry, TileFeatureModelLayerBase>::Base::size();
}

ModelNode::Ptr GeometryArrayView::localMergedAt(int64_t i) const
{
    if (isFeatureScopedView()) {
        if (i < 0) {
            return {};
        }
        auto const localAddress = featureScopedGeometryAddress();
        if (!localAddress) {
            return {};
        }
        if (isBaseGeometryColumn(localAddress.column()) ||
            localAddress.column() == TileFeatureModelLayerBase::ColumnId::GeometryViews) {
            return i == 0 ? this->model().resolve(localAddress) : ModelNode::Ptr{};
        }
        auto array = this->model().resolve<simfil::Array>(
            ModelNodeAddress{simfil::ModelPool::Arrays, localAddress.index()});
        return i < static_cast<int64_t>(array->size()) ? array->at(i) : ModelNode::Ptr{};
    }
    if (singleGeometryAddress_) {
        if (i == 0) {
            return this->model().resolve(singleGeometryAddress_);
        }
        return {};
    }
    return MergedArrayView<GeometryArrayView, Geometry, TileFeatureModelLayerBase>::Base::at(i);
}

bool GeometryArrayView::localMergedIterate(const IterCallback& cb) const
{
    if (isFeatureScopedView()) {
        auto const localCount = localMergedSize();
        for (uint32_t i = 0; i < localCount; ++i) {
            if (auto node = localMergedAt(i)) {
                if (!cb(*node)) {
                    return false;
                }
            }
        }
        return true;
    }
    if (singleGeometryAddress_) {
        if (auto node = this->model().resolve(singleGeometryAddress_)) {
            return cb(*node);
        }
        return true;
    }
    return MergedArrayView<GeometryArrayView, Geometry, TileFeatureModelLayerBase>::Base::iterate(cb);
}

/** ModelNode impls. for Geometry */

Geometry::Geometry(
    ModelConstPtr pool_,
    ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(std::move(pool_), a, key)
{
    storage_ = &model().vertexBufferStorage();
}

Geometry::Geometry(ViewData* data, ModelConstPtr pool_, ModelNodeAddress a, simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(std::move(pool_), a, key),
      geomViewData_(data)
{
    storage_ = &model().vertexBufferStorage();
}

uint64_t Geometry::getHash() const
{
    if (geomType() == GeomType::GltfNodeIndex) {
        Hash result;
        auto const origin = gltfNodeAabbOrigin();
        auto const size = gltfNodeAabbSize();
        result.mix(gltfNodeIndex())
            .mix(origin.x).mix(origin.y).mix(origin.z)
            .mix(size.x).mix(size.y).mix(size.z);
        return result.value();
    }
    Hash result;
    if (geomType() == GeomType::Polygon && !geomViewData_) {
        auto const ringCount = numPolygonRings();
        result.mix(ringCount);
        for (uint32_t ringIndex = 0; ringIndex < ringCount; ++ringIndex) {
            result.mix(polygonRingStart(ringIndex));
        }
    }
    forEachPoint([&result](Point const& p)
    {
        result.mix(p.x).mix(p.y).mix(p.z);
        return true;
    });
    return result.value();
}

std::optional<std::string_view> Geometry::name() const
{
    return geometryNameForStage(model(), stage());
}

std::optional<uint32_t> Geometry::stage() const
{
    if (auto geometryStage = model().geometryStage(addr_)) {
        return *geometryStage;
    }
    return std::nullopt;
}

void Geometry::setStage(std::optional<uint32_t> geometryStage)
{
    if (geometryStage && *geometryStage > static_cast<uint32_t>(std::numeric_limits<uint8_t>::max())) {
        raise("Geometry::setStage: stage is out of uint8_t range.");
    }
    model().setGeometryStage(
        addr_,
        geometryStage ? std::optional<uint8_t>{static_cast<uint8_t>(*geometryStage)} : std::nullopt);
}

SelfContainedGeometry Geometry::toSelfContained() const
{
    SelfContainedGeometry result{{}, {}, geomType()};
    result.points_.reserve(numPoints());
    forEachPoint([&result](auto&& pt)
    {
        result.points_.emplace_back(pt);
        return true;
    });
    if (geomType() == GeomType::Polygon) {
        auto const ringCount = numPolygonRings();
        result.polygonRingStarts_.reserve(ringCount);
        for (uint32_t ringIndex = 0; ringIndex < ringCount; ++ringIndex) {
            result.polygonRingStarts_.push_back(polygonRingStart(ringIndex));
        }
    }
    return result;
}

nlohmann::json Geometry::toJson() const
{
    return simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>::toJson();
}

ValueType Geometry::type() const {
    return ValueType::Object;
}

ModelNode::Ptr Geometry::at(int64_t i) const {
    auto const sourceDataReferences = model().geometrySourceDataReferences(addr_);
    auto const geometryName = name();
    auto const type = geomType();
    if (sourceDataReferences) {
        if (i == 0)
            return get(StringPool::SourceDataStr);
        i -= 1;
    }
    if (geometryName) {
        if (i == 0)
            return get(StringPool::GeometryNameStr);
        i -= 1;
    }
    if (i == 0)
        return get(StringPool::TypeStr);
    if (i == 1)
        return get(StringPool::CoordinatesStr);
    // AABB and GLTF-node geometries serialize as polygons plus one auxiliary
    // metadata field so they remain consumable as GeoJSON.
    if (type == GeomType::AABB && i == 2)
        return get(StringPool::AabbStr);
    if (type == GeomType::GltfNodeIndex && i == 2)
        return get(StringPool::GltfNodeIndexStr);
    throw std::out_of_range("geom: Out of range.");
}

uint32_t Geometry::size() const {
    auto const sourceDataReferences = model().geometrySourceDataReferences(addr_);
    auto const geometryName = name();
    auto const extraFields =
        geomType() == GeomType::AABB || geomType() == GeomType::GltfNodeIndex ? 1U : 0U;
    return 2 + extraFields + (sourceDataReferences ? 1 : 0) + (geometryName ? 1 : 0);
}

ModelNode::Ptr Geometry::get(const StringId& f) const {
    auto const sourceDataReferences = model().geometrySourceDataReferences(addr_);
    auto const geometryName = name();
    auto const type = geomViewData_ ? geomViewData_->type_ : geometryTypeForColumn(addr_.column());
    if (f == StringPool::SourceDataStr && sourceDataReferences) {
        return model().resolve(sourceDataReferences);
    }
    if (f == StringPool::GeometryNameStr && geometryName) {
        return model_ptr<ValueNode>::make(std::string_view(*geometryName), model_);
    }
    if (f == StringPool::TypeStr) {
        std::string_view typeName;
        switch (type) {
        case GeomType::Points:
            typeName = MultiPointStr;
            break;
        case GeomType::Line:
            typeName = LineStringStr;
            break;
        case GeomType::Polygon:
            typeName = PolygonStr;
            break;
        case GeomType::Mesh:
            typeName = MultiPolygonStr;
            break;
        case GeomType::AABB:
            // Bounds-only geometries are exported as footprint polygons plus an
            // auxiliary `aabb` object containing the full 3D extent.
            typeName = PolygonStr;
            break;
        case GeomType::GltfNodeIndex:
            // GLTF node references also export their bounds footprint so generic
            // GeoJSON consumers can still place them spatially.
            typeName = PolygonStr;
            break;
        }
        return model_ptr<ValueNode>::make(std::string_view(typeName), model_);
    }
    if (f == StringPool::AabbStr && type == GeomType::AABB) {
        return makeBoundsInfoView(model(), addr_);
    }
    if (f == StringPool::GltfNodeIndexStr && type == GeomType::GltfNodeIndex) {
        return model_ptr<ValueNode>::make(static_cast<int64_t>(gltfNodeIndex()), model_);
    }
    if (f == StringPool::CoordinatesStr) {
        switch (type) {
        case GeomType::AABB:
            return makeBoundsPolygonCoordinatesView(model(), addr_);
        case GeomType::GltfNodeIndex:
            return makeBoundsPolygonCoordinatesView(model(), addr_);
        case GeomType::Polygon:
            if (geomViewData_) {
                // Geometry views may expose only a point subrange, so they fall
                // back to the generic point-buffer view instead of polygon rings.
                break;
            }
            return model().resolve(
                ModelNodeAddress{TileFeatureModelLayerBase::ColumnId::Polygon, addr_.index()});
        case GeomType::Mesh:
            if (geomViewData_) {
                // Same for mesh views: only base meshes can present triangle collections.
                break;
            }
            return model().resolve(
                ModelNodeAddress{TileFeatureModelLayerBase::ColumnId::Mesh, addr_.index()});
        default:
            return model_ptr<PointBufferNode>::make(model_, addr_);
        }
        return model_ptr<PointBufferNode>::make(model_, addr_);
    }
    return {};
}

StringId Geometry::keyAt(int64_t i) const {
    auto const sourceDataReferences = model().geometrySourceDataReferences(addr_);
    auto const geometryName = name();
    if (sourceDataReferences) {
        if (i == 0)
            return StringPool::SourceDataStr;
        i -= 1;
    }
    if (geometryName) {
        if (i == 0)
            return StringPool::GeometryNameStr;
        i -= 1;
    }
    if (i == 0) return StringPool::TypeStr;
    if (i == 1) return StringPool::CoordinatesStr;
    if (geomType() == GeomType::AABB && i == 2) return StringPool::AabbStr;
    if (geomType() == GeomType::GltfNodeIndex && i == 2) return StringPool::GltfNodeIndexStr;
    throw std::out_of_range("geom: Out of range.");
}

model_ptr<SourceDataReferenceCollection> Geometry::sourceDataReferences() const
{
    auto const sourceDataReferences = model().geometrySourceDataReferences(addr_);
    if (sourceDataReferences)
        return model().resolve<SourceDataReferenceCollection>(sourceDataReferences);
    return {};
}

void Geometry::setSourceDataReferences(simfil::ModelNode::Ptr const& refs)
{
    model().setGeometrySourceDataReferences(addr_, refs->addr());
}

void Geometry::append(Point const& p)
{
    if (geomViewData_)
        throw std::runtime_error("Cannot append to geometry view.");

    if (geomType() == GeomType::GltfNodeIndex) {
        throw std::runtime_error("Cannot append coordinates to a GltfNodeIndex geometry.");
    }

    glm::vec3 storedPoint{};
    if (geomType() == GeomType::AABB && storage_->size(static_cast<simfil::ArrayIndex>(addr_.index())) == 1) {
        // The second AABB slot stores size, not another anchor-relative point.
        storedPoint = glm::vec3{
            static_cast<float>(p.x),
            static_cast<float>(p.y),
            static_cast<float>(p.z)};
    } else {
        // Regular geometry vertices are stored relative to the tile anchor to
        // preserve precision while keeping the on-disk representation compact.
        auto const anchor = model().geometryAnchor();
        storedPoint = glm::vec3{
            static_cast<float>(p.x - anchor.x),
            static_cast<float>(p.y - anchor.y),
            static_cast<float>(p.z - anchor.z)};
    }

    storage_->emplace_back(static_cast<simfil::ArrayIndex>(addr_.index()), storedPoint);
}

void Geometry::setAabb(Point const& origin, Point const& size)
{
    if (geomType() != GeomType::AABB) {
        raise("setAabb is only valid on AABB geometries.");
    }
    if (geomViewData_) {
        raise("Cannot mutate geometry view.");
    }

    auto const arrayIndex = static_cast<simfil::ArrayIndex>(addr_.index());
    auto const currentSize = storage_->size(arrayIndex);
    if (currentSize == 0) {
        // New AABBs reuse append() so the special origin/size storage layout is
        // applied consistently.
        append(origin);
        append(size);
        return;
    }
    if (currentSize != 2) {
        raiseFmt("AABB geometry expects exactly two stored entries, found {}.", currentSize);
    }

    auto const anchor = model().geometryAnchor();
    auto originSlot = storage_->at(arrayIndex, 0);
    auto sizeSlot = storage_->at(arrayIndex, 1);
    if (!originSlot || !sizeSlot) {
        raise("Failed to access AABB storage.");
    }
    originSlot->get() = glm::vec3{
        static_cast<float>(origin.x - anchor.x),
        static_cast<float>(origin.y - anchor.y),
        static_cast<float>(origin.z - anchor.z)};
    sizeSlot->get() = glm::vec3{
        static_cast<float>(size.x),
        static_cast<float>(size.y),
        static_cast<float>(size.z)};
}

Point Geometry::aabbOrigin() const
{
    if (geomType() != GeomType::AABB) {
        raise("aabbOrigin is only valid on AABB geometries.");
    }
    if (numPoints() != 2) {
        raiseFmt("AABB geometry expects exactly two points, found {}.", numPoints());
    }
    return pointAt(0);
}

Point Geometry::aabbSize() const
{
    if (geomType() != GeomType::AABB) {
        raise("aabbSize is only valid on AABB geometries.");
    }
    if (numPoints() != 2) {
        raiseFmt("AABB geometry expects exactly two points, found {}.", numPoints());
    }
    return pointAt(1);
}

void Geometry::setGltfNodeIndex(uint32_t index)
{
    if (geomType() != GeomType::GltfNodeIndex) {
        raise("setGltfNodeIndex is only valid on GltfNodeIndex geometries.");
    }
    if (geomViewData_) {
        raise("Cannot mutate geometry view.");
    }
    if (index > MaxExactGltfNodeIndex) {
        // The node index lives in a float-backed storage slot, so only integers
        // within the exact mantissa range are lossless.
        raiseFmt(
            "GLTF node index {} exceeds the exact float storage limit of {}.",
            index,
            MaxExactGltfNodeIndex);
    }

    auto const arrayIndex = geometryBufferIndex(addr_);
    ensureGltfNodeStorageInitialized(*storage_, arrayIndex);
    auto indexSlot = storage_->at(arrayIndex, GltfNodeIndexSlot);
    if (!indexSlot) {
        raise("Failed to access GLTF node index storage.");
    }
    indexSlot->get() = glm::vec3{0.0F, 0.0F, static_cast<float>(index)};
}

uint32_t Geometry::gltfNodeIndex() const
{
    if (geomType() != GeomType::GltfNodeIndex) {
        raise("gltfNodeIndex is only valid on GltfNodeIndex geometries.");
    }
    auto const arrayIndex = geometryBufferIndex(addr_);
    auto indexSlot = storage_->at(arrayIndex, GltfNodeIndexSlot);
    if (!indexSlot) {
        raise("Failed to access GLTF node index storage.");
    }
    return static_cast<uint32_t>(std::lround(indexSlot->get().z));
}

void Geometry::setGltfNodeBounds(Point const& origin, Point const& size)
{
    if (geomType() != GeomType::GltfNodeIndex) {
        raise("setGltfNodeBounds is only valid on GltfNodeIndex geometries.");
    }
    if (geomViewData_) {
        raise("Cannot mutate geometry view.");
    }

    auto const arrayIndex = geometryBufferIndex(addr_);
    ensureGltfNodeStorageInitialized(*storage_, arrayIndex);
    auto originSlot = storage_->at(arrayIndex, GltfNodeAabbOriginSlot);
    auto sizeSlot = storage_->at(arrayIndex, GltfNodeAabbSizeSlot);
    if (!originSlot || !sizeSlot) {
        raise("Failed to access GLTF node bounds storage.");
    }

    auto const anchor = model().geometryAnchor();
    originSlot->get() = glm::vec3{
        static_cast<float>(origin.x - anchor.x),
        static_cast<float>(origin.y - anchor.y),
        static_cast<float>(origin.z - anchor.z)};
    sizeSlot->get() = glm::vec3{
        static_cast<float>(size.x),
        static_cast<float>(size.y),
        static_cast<float>(size.z)};
}

Point Geometry::gltfNodeAabbOrigin() const
{
    if (geomType() != GeomType::GltfNodeIndex) {
        raise("gltfNodeAabbOrigin is only valid on GltfNodeIndex geometries.");
    }
    auto const arrayIndex = geometryBufferIndex(addr_);
    auto originSlot = storage_->at(arrayIndex, GltfNodeAabbOriginSlot);
    if (!originSlot) {
        raise("Failed to access GLTF node bounds origin.");
    }
    auto point = model().geometryAnchor();
    point += originSlot->get();
    return point;
}

Point Geometry::gltfNodeAabbSize() const
{
    if (geomType() != GeomType::GltfNodeIndex) {
        raise("gltfNodeAabbSize is only valid on GltfNodeIndex geometries.");
    }
    auto const arrayIndex = geometryBufferIndex(addr_);
    auto sizeSlot = storage_->at(arrayIndex, GltfNodeAabbSizeSlot);
    if (!sizeSlot) {
        raise("Failed to access GLTF node bounds size.");
    }
    return Point{
        sizeSlot->get().x,
        sizeSlot->get().y,
        sizeSlot->get().z};
}

GeomType Geometry::geomType() const {
    return geomViewData_ ? geomViewData_->type_ : geometryTypeForColumn(addr_.column());
}

bool Geometry::iterate(const IterCallback& cb) const
{
    for (auto i = 0; i < size(); ++i) {
        if (!cb(*at(i))) return false;
    }
    return true;
}

size_t Geometry::numPoints() const
{
    if (geomType() == GeomType::GltfNodeIndex) {
        // GLTF node geometries expose only their node id and bounds; they do
        // not behave like a coordinate buffer.
        return 0;
    }
    auto vertexBufferNode = model_ptr<PointBufferNode>::make(model_, addr_);
    return vertexBufferNode->size();
}

Point Geometry::pointAt(size_t index) const
{
    if (geomType() == GeomType::GltfNodeIndex) {
        raise("GltfNodeIndex geometries do not expose coordinates.");
    }
    auto vertexBufferNode = model_ptr<PointBufferNode>::make(model_, addr_);
    return vertexBufferNode->pointAt(static_cast<int64_t>(index));
}

uint32_t Geometry::numPolygonRings() const
{
    if (geomType() != GeomType::Polygon || geomViewData_) {
        return 0;
    }
    return model().polygonRingCount(addr_);
}

uint32_t Geometry::polygonRingStart(uint32_t ringIndex) const
{
    if (geomType() != GeomType::Polygon || geomViewData_) {
        raise("Polygon ring starts are only available on concrete polygon geometries.");
    }
    return model().polygonRingStart(addr_, ringIndex);
}

void Geometry::setPolygonRingStarts(std::span<uint32_t const> ringStarts)
{
    if (geomType() != GeomType::Polygon || geomViewData_) {
        raise("Polygon ring starts can only be set on concrete polygon geometries.");
    }
    model().setPolygonRingStarts(addr_, ringStarts);
}

double Geometry::length() const
{
    if (geomType() == GeomType::GltfNodeIndex || geomType() == GeomType::AABB) {
        // Bounds-only geometries and GLTF node references do not define a
        // traversable line length.
        return 0.0;
    }
    auto length = 0.0;
    if (numPoints() < 2) return length;
    for (auto i = 0; i < numPoints()-1; ++i)
    {
        auto pos = pointAt(i);
        auto posNext = pointAt(i+1);
        length += pos.geographicDistanceTo(posNext);
    }
    return length;
}

std::vector<Point> Geometry::pointsFromPositionBound(const Point& start, const std::optional<Point>& end) const
{
    // Find the line segments which are closest to start/end.
    uint32_t startClosestIndex = 0;
    uint32_t endClosestIndex = 0;
    double startClosestDistance = std::numeric_limits<double>::max();
    double endClosestDistance = std::numeric_limits<double>::max();
    glm::dvec3 startOffsetFromClosest;
    glm::dvec3 endOffsetFromClosest;

    // Function which works generically for start or end in the loop below.
    // Updates index, offset and distance if the line segment [newIndex->newIndex+1]
    // is closer to the point than [index->index+1].
    auto updateClosestIndex = [&](auto& index, auto& offset, auto& distance, auto newIndex, auto const& point) {
        auto linePointA = pointAt(newIndex);
        auto linePointB = pointAt(newIndex+1);
        auto newDistance = glm::distance(glm::vec2(linePointA), glm::vec2(point));
        auto newOffset = projectPointOnLine(point, linePointA, linePointB);
        if (newDistance < distance && newOffset) {
            distance = newDistance;
            index = newIndex;
            offset = *newOffset;
        }
    };

    // Loop which actually finds the closest indices of the line shape points.
    for (auto i = 0; i <  numPoints()-1; ++i) {
        updateClosestIndex(startClosestIndex, startOffsetFromClosest, startClosestDistance, i, start);
        if (end)
            updateClosestIndex(endClosestIndex, endOffsetFromClosest, endClosestDistance, i, *end);
    }

    // Make sure that end comes after start.
    if (end && endClosestIndex < startClosestIndex) {
        std::swap(startClosestIndex, endClosestIndex);
        std::swap(startClosestDistance, endClosestDistance);
        std::swap(startOffsetFromClosest, endOffsetFromClosest);
    }

    // Assemble geometry - just a point if end is not given.
    std::vector<Point> result;

    // Add the start point.
    auto startClosestPoint = pointAt(startClosestIndex);
    result.emplace_back(
         startClosestPoint.x + startOffsetFromClosest.x,
         startClosestPoint.y + startOffsetFromClosest.y,
         startClosestPoint.z + startOffsetFromClosest.z);

    // Add additional line points.
    if (end) {
        for (auto i = startClosestIndex + 1; i <= endClosestIndex; ++i) {
            result.emplace_back(pointAt(i));
        }
        auto endClosestPoint = pointAt(endClosestIndex);
        result.emplace_back(
            endClosestPoint.x + endOffsetFromClosest.x,
            endClosestPoint.y + endOffsetFromClosest.y,
            endClosestPoint.z + endOffsetFromClosest.z);
    }

    return result;
}

std::vector<Point> Geometry::pointsFromLengthBound(double start, std::optional<double> end) const
{
    if (numPoints() == 0) {
        return {};
    }
    if (numPoints() == 1) {
        return {pointAt(0)};
    }

    // Make sure that end comes after start.
    if (end && *end < start) {
        std::swap(start, *end);
    }

    // Datasource validity lengths may differ slightly from the display
    // geometry length due to simplification or independent source measures.
    // Clamp to the available rendered line instead of letting endpoint lookup
    // fall through to the initial point, which would render a diagonal chord.
    auto const lineLength = length();
    if (lineLength <= 0.) {
        return {pointAt(0)};
    }
    start = std::clamp(start, 0., lineLength);
    if (end) {
        *end = std::clamp(*end, 0., lineLength);
    }

    int32_t innerIndexStart = 0, innerIndexEnd = 0;
    auto startPos = pointAt(innerIndexStart), endPos = pointAt(innerIndexEnd);
    double coveredLength = 0;
    bool startReached = false;
    for (auto i = 0; i < numPoints()-1; ++i)
    {
        auto pos = pointAt(i);
        auto posNext = pointAt(i+1);
        auto dist = pos.geographicDistanceTo(posNext);
        coveredLength += dist;

        if (!startReached && start <= coveredLength)
        {
            innerIndexStart = i;
            // Note: We use a fast linear calculation here instead of proper geodesic trigonometry.
            // I calculated, that the approximate error for this is roughly 0.001% at the equator, so
            // the error on a 1km long line would be about 1 centimeter.
            auto lerp = static_cast<double>(dist - (coveredLength - start)) / static_cast<double>(dist);
            startPos = pos + (posNext - pos) * lerp;
            startReached = true;
            if (!end)
                break;
        }
        if (startReached && end && *end <= coveredLength) {
            innerIndexEnd = i;
            auto lerp = static_cast<double>(dist - (coveredLength - *end)) / static_cast<double>(dist);
            endPos = pos + (posNext - pos) * lerp;
            break;
        }
    }

    // Assemble geometry - just a point if end is not given.
    std::vector<Point> result;

    // Add the start point.
    result.emplace_back(startPos);

    // Add additional line points.
    if (end) {
        for (auto i = innerIndexStart + 1; i <= innerIndexEnd; ++i) {
            result.emplace_back(pointAt(i));
        }
        result.emplace_back(endPos);
    }

    return result;
}

Point Geometry::percentagePositionFromGeometries(std::vector<model_ptr<Geometry>> const& geoms,
    std::vector<double> const& lengths, uint32_t numBits, double position)
{
    double totalLength = std::accumulate(lengths.begin(), lengths.end(), 0.0);
    auto maxPos = static_cast<double>((1 << numBits) - 1);
    auto percentagePosition = (position / maxPos) * totalLength;
    Point positionPoint;
    for (size_t i = 0; i < lengths.size(); i++) {
        if (lengths[i] < percentagePosition) {
            percentagePosition -= lengths[i];
        }
        else {
            // Once the target falls into a segment geometry, reuse the
            // length-bound sampling helper to get the final point.
            auto points = geoms[i]->pointsFromLengthBound(percentagePosition, std::nullopt);
            if (points.empty()) {
                break;
            }
            positionPoint = points[0];
            break;
        }
    }
    return positionPoint;
}

/** ModelNode impls. for bounds helper views */

BoundsInfoNode::BoundsInfoNode(ModelNode const& baseNode, simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(baseNode, key),
      baseGeometryAddress_(decodeGeometryHelperBaseAddress(addr_, std::get<int64_t>(data_)))
{}

ValueType BoundsInfoNode::type() const
{
    return ValueType::Object;
}

ModelNode::Ptr BoundsInfoNode::at(int64_t i) const
{
    if (i == 0) return get(StringPool::OriginStr);
    if (i == 1) return get(StringPool::SizeStr);
    throw std::out_of_range("bounds-info: Out of range.");
}

uint32_t BoundsInfoNode::size() const
{
    return 2;
}

ModelNode::Ptr BoundsInfoNode::get(const StringId& field) const
{
    if (field == StringPool::OriginStr) {
        return makeGeometryPointView(
            model(),
            baseGeometryAddress_,
            GeometryPointViewKind::BoundsOrigin);
    }
    if (field == StringPool::SizeStr) {
        return makeGeometryPointView(
            model(),
            baseGeometryAddress_,
            GeometryPointViewKind::BoundsSize);
    }
    return {};
}

StringId BoundsInfoNode::keyAt(int64_t i) const
{
    if (i == 0) return StringPool::OriginStr;
    if (i == 1) return StringPool::SizeStr;
    throw std::out_of_range("bounds-info: Out of range.");
}

bool BoundsInfoNode::iterate(const IterCallback& cb) const
{
    if (!cb(*at(0))) return false;
    if (!cb(*at(1))) return false;
    return true;
}

BoundsPolygonCoordinatesNode::BoundsPolygonCoordinatesNode(
    ModelNode const& baseNode,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(baseNode, key),
      baseGeometryAddress_(decodeGeometryHelperBaseAddress(addr_, std::get<int64_t>(data_)))
{}

ValueType BoundsPolygonCoordinatesNode::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr BoundsPolygonCoordinatesNode::at(int64_t i) const
{
    if (i != 0) {
        throw std::out_of_range("bounds-polygon: Out of range.");
    }
    return makeBoundsRingView(model(), baseGeometryAddress_);
}

uint32_t BoundsPolygonCoordinatesNode::size() const
{
    return 1;
}

ModelNode::Ptr BoundsPolygonCoordinatesNode::get(const StringId&) const
{
    return {};
}

StringId BoundsPolygonCoordinatesNode::keyAt(int64_t) const
{
    return {};
}

bool BoundsPolygonCoordinatesNode::iterate(const IterCallback& cb) const
{
    return cb(*at(0));
}

BoundsRingNode::BoundsRingNode(ModelNode const& baseNode, simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(baseNode, key),
      baseGeometryAddress_(decodeGeometryHelperBaseAddress(addr_, std::get<int64_t>(data_)))
{}

ValueType BoundsRingNode::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr BoundsRingNode::at(int64_t i) const
{
    switch (i) {
    case 0:
        return makeGeometryPointView(model(), baseGeometryAddress_, GeometryPointViewKind::BoundsCorner0);
    case 1:
        return makeGeometryPointView(model(), baseGeometryAddress_, GeometryPointViewKind::BoundsCorner1);
    case 2:
        return makeGeometryPointView(model(), baseGeometryAddress_, GeometryPointViewKind::BoundsCorner2);
    case 3:
        return makeGeometryPointView(model(), baseGeometryAddress_, GeometryPointViewKind::BoundsCorner3);
    case 4:
        return makeGeometryPointView(model(), baseGeometryAddress_, GeometryPointViewKind::BoundsCorner4);
    default:
        throw std::out_of_range("bounds-ring: Out of range.");
    }
}

uint32_t BoundsRingNode::size() const
{
    return 5;
}

ModelNode::Ptr BoundsRingNode::get(const StringId&) const
{
    return {};
}

StringId BoundsRingNode::keyAt(int64_t) const
{
    return {};
}

bool BoundsRingNode::iterate(const IterCallback& cb) const
{
    for (int64_t i = 0; i < 5; ++i) {
        if (!cb(*at(i))) {
            return false;
        }
    }
    return true;
}

/** ModelNode impls. for PolygonNode */

PolygonNode::PolygonNode(ModelConstPtr pool, ModelNodeAddress const& a, simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(std::move(pool), a, key)
{}

ValueType PolygonNode::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr PolygonNode::at(int64_t index) const
{
    if (index >= 0 && index < size()) {
        // Ring 0 is the outer ring; following rings are holes.
        return model().resolve(
            ModelNodeAddress{TileFeatureModelLayerBase::ColumnId::LinearRing, addr_.index()},
            index);
    }

    throw std::out_of_range("PolygonNode: index out of bounds.");
}

uint32_t PolygonNode::size() const
{
    return model().polygonRingCount(ModelNodeAddress{TileFeatureModelLayerBase::ColumnId::PolygonGeometries, addr_.index()});
}

ModelNode::Ptr PolygonNode::get(const StringId&) const
{
    return {};
}

StringId PolygonNode::keyAt(int64_t) const
{
    return {};
}

bool PolygonNode::iterate(IterCallback const& cb) const
{
    for (uint32_t i = 0; i < size(); ++i) {
        if (!cb(*at(i))) {
            return false;
        }
    }
    return true;
}

/** ModelNode impls. for MeshNode */

MeshNode::MeshNode(ModelConstPtr pool,
    ModelNodeAddress const& a,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(std::move(pool), a, key)
{
    auto vertex_buffer = model_ptr<PointBufferNode>::make(
        model_,
        ModelNodeAddress{TileFeatureModelLayerBase::ColumnId::MeshGeometries, addr_.index()});
    assert(vertex_buffer->size() % 3 == 0);
    size_ = vertex_buffer->size() / 3;
}

ValueType MeshNode::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr MeshNode::at(int64_t index) const
{
    if (0 <= index && index < size_)
        return model().resolve(
            ModelNodeAddress{TileFeatureModelLayerBase::ColumnId::MeshTriangleCollection, addr_.index()},
            index);

    throw std::out_of_range("MeshNode: index out of bounds.");
}

uint32_t MeshNode::size() const
{
    return size_;
}

bool MeshNode::iterate(IterCallback const& cb) const
{
    for (auto i = 0; i < size(); ++i)
        if (!cb(*at(i)))
            return false;
    return true;
}

MeshTriangleCollectionNode::MeshTriangleCollectionNode(const ModelNode& base, simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(base, key),
      index_(std::get<int64_t>(data_) * 3)
{}

ValueType MeshTriangleCollectionNode::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr MeshTriangleCollectionNode::at(int64_t index) const
{
    if (index == 0)
        return model().resolve(
            ModelNodeAddress{TileFeatureModelLayerBase::ColumnId::MeshTriangleLinearRing, addr_.index()},
            index_);

    throw std::out_of_range("MeshTriangleCollectionNode: index out of bounds.");
}

uint32_t MeshTriangleCollectionNode::size() const
{
    return 1;
}

bool MeshTriangleCollectionNode::iterate(IterCallback const& cb) const
{
    if (!cb(*at(0)))
        return false;
    return true;
}

/** ModelNode impls. for LinearRingNode (a closed, simple polygon in CCW order) */

LinearRingNode::LinearRingNode(const ModelNode& base, simfil::detail::mp_key key)
    : LinearRingNode(base, std::optional<size_t>{}, key)
{}

LinearRingNode::LinearRingNode(const ModelNode& base, std::optional<size_t> length, simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(base, key)
{
    if (addr_.column() == TileFeatureModelLayerBase::ColumnId::LinearRing) {
        if (std::get_if<int64_t>(&data_)) {
            ringIndex_ = static_cast<uint32_t>(std::get<int64_t>(data_));
        }
        auto const polygonAddress = ModelNodeAddress{TileFeatureModelLayerBase::ColumnId::PolygonGeometries, addr_.index()};
        offset_ = model().polygonRingStart(polygonAddress, ringIndex_);
        auto const end = model().polygonRingEnd(polygonAddress, ringIndex_);
        size_ = end - offset_;
        desiredOrientation_ = ringIndex_ == 0 ? Orientation::CCW : Orientation::CW;
    }
    else {
        if (std::get_if<int64_t>(&data_)) {
            offset_ = std::get<int64_t>(data_);
        }
    }

    auto buffer = vertexBuffer();
    if (addr_.column() != TileFeatureModelLayerBase::ColumnId::LinearRing) {
        size_ = length.value_or(buffer->size() - offset_);
    }

    auto isClosed = [&]()
    {
        const auto n = size_;
        if (n < 3)
            return false;

        const auto& first = buffer->pointAt(0 + offset_);
        const auto& last = buffer->pointAt(n - 1 + offset_);

        return first == last;
    };

    closed_ = isClosed();

    // The signed area of a simple polygon can be calculated
    // using the shoelace formula:
    //
    //        n-1
    //   2 A = Σ  x_i y_(i+1) - y_i x_(i+1)
    //        i=0
    //
    // If the area is negative, the polygon is in clock-wise orientation.
    // We assume the polygon in in the x-y plane, returning 0 for polygons
    // on a different plane.
    auto signedArea = [&]()
    {
        if (size_ <= 0)
            return 0.0;

        auto area = 0.0;
        auto z = buffer->pointAt(0 + offset_).z;
        auto const n = closed_ ? size_ - 1U : size_;
        for (auto i = 0U; i < n; ++i) {
            const auto& a = buffer->pointAt(i + offset_);
            const auto& b = buffer->pointAt(((i + 1U) % n) + offset_);
            if (a.z != z)
                return 0.0;

            area += a.x * b.y - a.y * b.x;
        }
        return area / 2;
    };

    orientation_ = signedArea() < 0 ? Orientation::CW : Orientation::CCW;
}

ValueType LinearRingNode::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr LinearRingNode::at(int64_t index) const
{
    auto buffer = vertexBuffer();
    if (0 > index || index >= size())
        throw std::out_of_range("LinearRingNode: index out of range.");

    // GeoJSON rings must be closed. If the ring is not closed,
    // we return the first point as last point again.
    if (!closed_ && index == size() - 1)
        return buffer->at(0 + offset_);

    // GeoJSON/NDS expect outer rings to be CCW and hole rings to be CW. When
    // the stored ring is already explicitly closed, reverse only the unique
    // vertices and keep the final closing coordinate equal to the first one.
    if (orientation_ != desiredOrientation_) {
        auto const uniqueVertexCount = closed_ ? size_ - 1U : size_;
        if (index == static_cast<int64_t>(uniqueVertexCount)) {
            return buffer->at(offset_);
        }
        if (index > 0) {
            index = static_cast<int64_t>(uniqueVertexCount) - index;
        }
    }

    return buffer->at(index + offset_);
}

ModelNode::Ptr LinearRingNode::get(const StringId&) const
{
    return {};
}

StringId LinearRingNode::keyAt(int64_t) const
{
    return {};
}

bool LinearRingNode::iterate(const IterCallback& cb) const
{
    for (auto i = 0; i < size(); ++i)
        if (!cb(*at(i)))
            return false;
    return true;
}

uint32_t LinearRingNode::size() const
{
    return size_ + (closed_ ? 0 : 1);
}

model_ptr<PointBufferNode> LinearRingNode::vertexBuffer() const
{
    using Col = TileFeatureModelLayerBase::ColumnId;
    switch (addr_.column()) {
    case Col::LinearRing:
        return model_ptr<PointBufferNode>::make(
            model_,
            ModelNodeAddress{Col::PolygonGeometries, addr_.index()});
    case Col::MeshTriangleLinearRing:
        return model_ptr<PointBufferNode>::make(
            model_,
            ModelNodeAddress{Col::MeshGeometries, addr_.index()});
    default:
        return model_ptr<PointBufferNode>::make(model_, addr_);
    }
}

/** ModelNode impls. for VertexBufferNode */

PointBufferNode::PointBufferNode(
    ModelConstPtr pool_,
    ModelNodeAddress const& baseGeometryAddress,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(
        std::move(pool_),
        ModelNodeAddress{
            baseGeometryAddress.column() == TileFeatureModelLayerBase::ColumnId::GeometryViews ?
                TileFeatureModelLayerBase::ColumnId::PointBuffersView :
                TileFeatureModelLayerBase::ColumnId::PointBuffers,
            baseGeometryAddress.index()},
        key),
      baseGeomAddress_(baseGeometryAddress)
{
    storage_ = &model().vertexBufferStorage();

    // Resolve geometry views to their base geometry while preserving the
    // selected point range.
    if (baseGeomAddress_.column() == TileFeatureModelLayerBase::ColumnId::GeometryViews) {
        auto const* viewData = model().geometryViewData(baseGeomAddress_);
        if (!viewData) {
            throw std::runtime_error("Failed to resolve geometry view.");
        }
        offset_ = viewData->offset_;
        size_ = viewData->size_;
        baseGeomAddress_ = viewData->baseGeometry_;

        while (baseGeomAddress_.column() == TileFeatureModelLayerBase::ColumnId::GeometryViews) {
            // Nested views accumulate offsets until a real base geometry buffer
            // is reached, so point access stays O(1) afterwards.
            viewData = model().geometryViewData(baseGeomAddress_);
            if (!viewData) {
                throw std::runtime_error("Failed to resolve nested geometry view.");
            }
            offset_ += viewData->offset_;
            baseGeomAddress_ = viewData->baseGeometry_;
        }

        if (!isBaseGeometryColumn(baseGeomAddress_.column())) {
            throw std::runtime_error("Geometry view must resolve to a base geometry.");
        }
        baseVertexArray_ = static_cast<simfil::ArrayIndex>(baseGeomAddress_.index());
        auto maxSize = storage_->size(baseVertexArray_);
        if (offset_ + size_ > maxSize)
            throw std::runtime_error("Geometry view is out of bounds.");
    }
    else {
        if (!isBaseGeometryColumn(baseGeomAddress_.column())) {
            throw std::runtime_error("PointBuffer expects geometry or geometry-view address.");
        }
        baseVertexArray_ = static_cast<simfil::ArrayIndex>(baseGeomAddress_.index());
        size_ = storage_->size(baseVertexArray_);
    }
}

ValueType PointBufferNode::type() const {
    return ValueType::Array;
}

ModelNode::Ptr PointBufferNode::at(int64_t i) const {
    if (i < 0 || i >= size())
        throw std::out_of_range("vertex-buffer: Out of range.");
    auto const absoluteIndex = i + offset_;
    if (baseGeomAddress_.column() == TileFeatureModelLayerBase::ColumnId::AabbGeometries &&
        absoluteIndex == 1) {
        return makeGeometryPointView(
            model(),
            baseGeomAddress_,
            GeometryPointViewKind::RawSize);
    }
    auto const pointNodeAddress = ModelNodeAddress{
        TileFeatureModelLayerBase::ColumnId::Points,
        baseGeomAddress_.index()};
    return model().resolve(pointNodeAddress, absoluteIndex);
}

uint32_t PointBufferNode::size() const {
    return size_;
}

ModelNode::Ptr PointBufferNode::get(const StringId &) const {
    return {};
}

StringId PointBufferNode::keyAt(int64_t) const {
    return {};
}

bool PointBufferNode::iterate(const IterCallback& cb) const
{
    for (auto i = 0u; i < size_; ++i) {
        if (!cb(*at(static_cast<int64_t>(i)))) {
            return false;
        }
    }
    return true;
}

Point PointBufferNode::pointAt(int64_t index) const
{
    if (index < 0 || index >= static_cast<int64_t>(size_)) {
        throw std::out_of_range("vertex-buffer: Out of range.");
    }
    auto vertexResult = storage_->at(
        baseVertexArray_,
        static_cast<size_t>(index + offset_));
    if (!vertexResult) {
        throw std::out_of_range("vertex-buffer: Out of range.");
    }
    if (baseGeomAddress_.column() == TileFeatureModelLayerBase::ColumnId::AabbGeometries &&
        index + static_cast<int64_t>(offset_) == 1) {
        return Point{
            vertexResult->get().x,
            vertexResult->get().y,
            vertexResult->get().z};
    }
    auto point = model().geometryAnchor();
    point += vertexResult->get();
    return point;
}

}
