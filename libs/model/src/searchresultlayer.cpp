#include "searchresultlayer.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <type_traits>

#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/bitsery.h>
#include <bitsery/ext/std_optional.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>

#include "mapget/log.h"
#include "pointnode.h"
#include "simfil/model/bitsery-traits.h"
#include "sourcedatareference.h"

namespace bitsery
{

template <typename S>
void serialize(S& s, glm::vec3& v)
{
    s.value4b(v.x);
    s.value4b(v.y);
    s.value4b(v.z);
}

} // namespace bitsery

namespace mapget
{
namespace
{
using GeometryPointBufferArena = TileFeatureModelLayerBase::GeometryStorage;

constexpr uint32_t SourceAddressArenaIndexBits = 20;
constexpr uint32_t SourceAddressArenaIndexMax = (~static_cast<uint32_t>(0)) >> (32 - SourceAddressArenaIndexBits);
constexpr uint32_t SourceAddressArenaSizeBits = 4;
constexpr uint32_t SourceAddressArenaSizeMax = (~static_cast<uint32_t>(0)) >> (32 - SourceAddressArenaSizeBits);
constexpr uint8_t InvalidGeometryStage = std::numeric_limits<uint8_t>::max();

std::tuple<size_t, size_t> modelAddressToSourceDataAddressList(uint32_t addr)
{
    auto const index = addr >> SourceAddressArenaSizeBits;
    auto const size = addr & SourceAddressArenaSizeMax;
    return {index, size};
}

uint32_t sourceDataAddressListToModelAddress(uint32_t index, uint32_t size)
{
    if (index > SourceAddressArenaIndexMax) {
        raise("Source-data reference index out of range.");
    }
    if (size > SourceAddressArenaSizeMax) {
        raise("Source-data reference list size out of range.");
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

void ensureGeometryStageCapacity(
    simfil::ModelColumn<uint8_t, simfil::detail::ColumnPageSize>& stages,
    simfil::ArrayIndex index)
{
    if (index == simfil::InvalidArrayIndex) {
        raiseFmt("Invalid geometry buffer index {}.", index);
    }
    while (stages.size() <= static_cast<size_t>(index)) {
        stages.emplace_back(InvalidGeometryStage);
    }
}

uint32_t extraGeometryDataStorageIndex(simfil::ArrayIndex geometryIndex)
{
    if (geometryIndex == simfil::InvalidArrayIndex) {
        raiseFmt("Invalid geometry buffer index {}.", geometryIndex);
    }
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

std::optional<uint8_t> geometryStageAt(
    simfil::ModelColumn<uint8_t, simfil::detail::ColumnPageSize> const& stages,
    uint32_t index)
{
    if (index >= stages.size()) {
        return std::nullopt;
    }
    auto const storedStage = stages.at(index);
    if (storedStage == InvalidGeometryStage) {
        return std::nullopt;
    }
    return storedStage;
}

simfil::ArrayIndex idPartValuesToArrayIndex(
    TileSearchResultLayer& layer,
    std::vector<IdPart> const& composition,
    KeyValueViewPairs const& idParts)
{
    auto idValues = layer.newArray(composition.size(), true);
    auto idPartsIter = idParts.begin();

    for (auto const& idPart : composition) {
        if (idPartsIter != idParts.end() && idPart.idPartLabel_ == idPartsIter->first) {
            idValues->append(std::visit(
                [&](auto&& v) -> simfil::ModelNode::Ptr { return layer.newValue(v); },
                idPartsIter->second));
            ++idPartsIter;
            continue;
        }

        if (!idPart.isOptional_) {
            raiseFmt("Missing non-optional ID part '{}' while materializing feature ID values.", idPart.idPartLabel_);
        }
        idValues->append(layer.resolve<simfil::ModelNode>(
            simfil::ModelNodeAddress{simfil::Model::Null, 1},
            simfil::ScalarValueType{}));
    }

    if (idPartsIter != idParts.end()) {
        raiseFmt("Unexpected trailing ID part '{}' while materializing feature ID values.", idPartsIter->first);
    }
    return static_cast<simfil::ArrayIndex>(idValues->addr().index());
}

nlohmann::json arrayToJson(model_ptr<Array> const& array)
{
    auto result = nlohmann::json::array();
    if (!array) {
        return result;
    }
    for (uint32_t i = 0; i < array->size(); ++i) {
        auto item = array->at(i);
        result.push_back(item ? item->toJson() : nlohmann::json());
    }
    return result;
}
} // namespace

struct TileSearchResultLayer::Impl
{
    Point geometryAnchor_{};
    std::optional<uint32_t> stage_;
    std::vector<std::string> resultFields_;
    simfil::ModelColumn<FeatureId::Data, simfil::detail::ColumnPageSize / 2> featureIds_;
    simfil::ModelColumn<SearchResult::Data, simfil::detail::ColumnPageSize / 2> searchResults_;
    simfil::ModelColumn<simfil::ModelNodeAddress, simfil::detail::ColumnPageSize / 2> geomSourceDataRefs_;
    simfil::ModelColumn<uint8_t, simfil::detail::ColumnPageSize> geomStages_;
    simfil::ModelColumn<GeometryViewData, simfil::detail::ColumnPageSize / 2> geomViews_;
    simfil::ModelColumn<QualifiedSourceDataReference, simfil::detail::ColumnPageSize / 2> sourceDataReferences_;
    GeometryStorage pointBuffers_;

    template<typename S>
    void readWrite(S& s)
    {
        s.value8b(geometryAnchor_.x);
        s.value8b(geometryAnchor_.y);
        s.value8b(geometryAnchor_.z);
        s.ext4b(stage_, bitsery::ext::StdOptional{});
        s.container(resultFields_, std::numeric_limits<uint32_t>::max(), [](auto& serializer, std::string& field) {
            serializer.text1b(field, std::numeric_limits<uint32_t>::max());
        });
        s.object(featureIds_);
        s.object(searchResults_);
        s.object(geomSourceDataRefs_);
        s.object(geomViews_);
        s.ext(pointBuffers_, bitsery::ext::ArrayArenaExt{});
        s.object(sourceDataReferences_);
        s.object(geomStages_);
    }
};

SearchResult::SearchResult(
    Data* data,
    simfil::ModelConstPtr pool,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileSearchResultLayer>(std::move(pool), address, key),
      data_(data)
{
}

model_ptr<FeatureId> SearchResult::featureId() const
{
    return model().resolve<FeatureId>(data_->featureId_);
}

model_ptr<GeometryCollection> SearchResult::geometry() const
{
    return model().resolve<GeometryCollection>(data_->geometry_);
}

std::optional<uint32_t> SearchResult::attributeIndex() const
{
    if (data_->attributeIndex_ == InvalidAttributeIndex) {
        return std::nullopt;
    }
    return data_->attributeIndex_;
}

std::optional<std::string_view> SearchResult::attributePath() const
{
    if (data_->attributePath_ == simfil::StringPool::Empty) {
        return std::nullopt;
    }
    return model().strings()->resolve(data_->attributePath_);
}

std::optional<uint32_t> SearchResult::validityIndex() const
{
    if (data_->validityIndex_ == InvalidAttributeIndex) {
        return std::nullopt;
    }
    return data_->validityIndex_;
}

std::optional<uint32_t> SearchResult::validityCount() const
{
    if (data_->validityIndex_ == InvalidAttributeIndex) {
        return std::nullopt;
    }
    return data_->validityCount_;
}

model_ptr<Array> SearchResult::values() const
{
    if (data_->values_ == simfil::InvalidArrayIndex) {
        return {};
    }
    return model().resolve<Array>(
        simfil::ModelNodeAddress{simfil::ModelPool::ColumnId::Arrays, static_cast<uint32_t>(data_->values_)});
}

nlohmann::json SearchResult::toJson() const
{
    auto result = nlohmann::json::object({
        {"type", "SearchResult"},
        {"featureId", featureId() ? featureId()->toString() : ""},
        {"geometry", geometry() ? geometry()->toJson() : nlohmann::json()},
        {"values", arrayToJson(values())},
    });
    if (auto attrIndex = attributeIndex()) {
        result["attributeIndex"] = *attrIndex;
        auto match = nlohmann::json::object({
            {"attributeIndex", *attrIndex},
        });
        if (auto path = attributePath()) {
            match["attributePath"] = std::string(*path);
        }
        if (auto index = validityIndex()) {
            match["validityIndex"] = *index;
        }
        if (auto count = validityCount()) {
            match["validityCount"] = *count;
        }
        result["match"] = std::move(match);
    }
    return result;
}

simfil::ValueType SearchResult::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr SearchResult::at(int64_t i) const
{
    return get(keyAt(i));
}

uint32_t SearchResult::size() const
{
    return attributeIndex() ? 7U : 3U;
}

simfil::ModelNode::Ptr SearchResult::get(simfil::StringId const& field) const
{
    if (field == StringPool::FeatureIdStr) {
        return featureId();
    }
    if (field == StringPool::GeometryStr) {
        return geometry();
    }
    if (field == StringPool::ValuesStr) {
        return values();
    }
    if (field == StringPool::AttributeIndexStr) {
        if (auto attrIndex = attributeIndex()) {
            return model().newValue(static_cast<int64_t>(*attrIndex));
        }
    }
    if (field == StringPool::AttributePathStr) {
        if (auto path = attributePath()) {
            return model().newValue(*path);
        }
    }
    if (field == StringPool::ValidityIndexStr) {
        if (auto index = validityIndex()) {
            return model().newValue(static_cast<int64_t>(*index));
        }
    }
    if (field == StringPool::ValidityCountStr) {
        if (auto count = validityCount()) {
            return model().newValue(static_cast<int64_t>(*count));
        }
    }
    return {};
}

simfil::StringId SearchResult::keyAt(int64_t i) const
{
    switch (i) {
    case 0: return StringPool::FeatureIdStr;
    case 1: return StringPool::GeometryStr;
    case 2: return StringPool::ValuesStr;
    case 3: return attributeIndex() ? static_cast<simfil::StringId>(StringPool::AttributeIndexStr) : simfil::StringPool::Empty;
    case 4: return attributeIndex() ? static_cast<simfil::StringId>(StringPool::AttributePathStr) : simfil::StringPool::Empty;
    case 5: return attributeIndex() ? static_cast<simfil::StringId>(StringPool::ValidityIndexStr) : simfil::StringPool::Empty;
    case 6: return attributeIndex() ? static_cast<simfil::StringId>(StringPool::ValidityCountStr) : simfil::StringPool::Empty;
    default: return simfil::StringPool::Empty;
    }
}

bool SearchResult::iterate(IterCallback const& cb) const
{
    for (uint32_t i = 0; i < size(); ++i) {
        auto value = at(i);
        if (value && !cb(*value)) {
            return false;
        }
    }
    return true;
}

TileSearchResultLayer::TileSearchResultLayer(
    TileId tileId,
    std::string const& nodeId,
    std::string const& mapId,
    std::shared_ptr<LayerInfo> const& layerInfo,
    std::shared_ptr<simfil::StringPool> const& strings)
    : TileFeatureModelLayerBase(tileId, nodeId, mapId, layerInfo, strings),
      impl_(std::make_unique<Impl>())
{
    impl_->geometryAnchor_ = tileId.center();
}

TileSearchResultLayer::TileSearchResultLayer(
    std::vector<uint8_t> const& input,
    LayerInfoResolveFun const& layerInfoResolveFun,
    StringPoolResolveFun const& stringPoolGetter)
    : TileFeatureModelLayerBase(input, layerInfoResolveFun, stringPoolGetter, &deserializationOffsetBytes_),
      impl_(std::make_unique<Impl>())
{
    impl_->geometryAnchor_ = tileId_.center();
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    if (deserializationOffsetBytes_ > input.size()) {
        raise("Failed to read TileSearchResultLayer: invalid deserialization offset.");
    }
    bitsery::Deserializer<Adapter> s(Adapter(
        input.begin() + static_cast<std::ptrdiff_t>(deserializationOffsetBytes_),
        input.end()));
    impl_->readWrite(s);
    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raiseFmt(
            "Failed to read TileSearchResultLayer: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error()));
    }
    auto const modelOffset = deserializationOffsetBytes_ + s.adapter().currentReadPos();
    if (auto result = ModelPool::read(input, modelOffset); !result) {
        raise(result.error().message);
    }
}

TileSearchResultLayer::~TileSearchResultLayer() = default;

void TileSearchResultLayer::setResultFields(std::vector<std::string> fields)
{
    impl_->resultFields_ = std::move(fields);
}

std::vector<std::string> const& TileSearchResultLayer::resultFields() const
{
    return impl_->resultFields_;
}

std::optional<uint32_t> TileSearchResultLayer::stage() const
{
    return impl_->stage_;
}

void TileSearchResultLayer::setStage(std::optional<uint32_t> stage)
{
    impl_->stage_ = stage;
}

model_ptr<SearchResult> TileSearchResultLayer::newSearchResult(
    model_ptr<FeatureId> const& featureId,
    model_ptr<GeometryCollection> const& geometry,
    std::span<simfil::ModelNode::Ptr const> values,
    std::optional<uint32_t> attributeIndex,
    std::optional<std::string_view> attributePath,
    std::optional<uint32_t> validityIndex,
    std::optional<uint32_t> validityCount)
{
    if (!featureId) {
        raise("Search result requires a feature id.");
    }
    if (!geometry) {
        raise("Search result requires geometry.");
    }

    auto valueArray = newArray(std::max<size_t>(values.size(), impl_->resultFields_.size()), true);
    for (auto const& value : values) {
        valueArray->append(value ? value : resolve<simfil::ModelNode>(
            simfil::ModelNodeAddress{simfil::Model::Null, 1},
            simfil::ScalarValueType{}));
    }
    while (valueArray->size() < impl_->resultFields_.size()) {
        valueArray->append(resolve<simfil::ModelNode>(
            simfil::ModelNodeAddress{simfil::Model::Null, 1},
            simfil::ScalarValueType{}));
    }

    simfil::StringId attributePathId = simfil::StringPool::Empty;
    if (attributePath && !attributePath->empty()) {
        auto emplaced = strings()->emplace(*attributePath);
        if (!emplaced) {
            raise(emplaced.error().message);
        }
        attributePathId = *emplaced;
    }

    auto const resultIndex = static_cast<uint32_t>(impl_->searchResults_.size());
    impl_->searchResults_.emplace_back(SearchResult::Data{
        featureId->addr(),
        geometry->addr(),
        static_cast<simfil::ArrayIndex>(valueArray->addr().index()),
        attributeIndex.value_or(SearchResult::InvalidAttributeIndex),
        attributePathId,
        validityIndex.value_or(SearchResult::InvalidAttributeIndex),
        validityCount.value_or(0U),
    });
    auto result = SearchResult(
        &impl_->searchResults_.back(),
        shared_from_this(),
        {ColumnId::SearchResults, resultIndex},
        mpKey_);
    addRoot(simfil::ModelNode::Ptr(result));
    setInfo("Size/SearchResults#results", numRoots());
    return result;
}

model_ptr<FeatureId> TileSearchResultLayer::newFeatureId(
    std::string_view const& typeId,
    KeyValueViewPairs const& featureIdParts,
    std::optional<std::string_view> externalMapId)
{
    if (!layerInfo_->validFeatureId(typeId, featureIdParts, false)) {
        raiseFmt("Could not find a matching ID composition of type {} for search result.", typeId);
    }

    auto typeIdStringId = strings()->emplace(typeId);
    if (!typeIdStringId) {
        raise(typeIdStringId.error().message);
    }
    auto const idCompositionIndex = *layerInfo_->matchingFeatureIdCompositionIndex(typeId, featureIdParts, false);
    auto const& composition = layerInfo_->getTypeInfo(typeId)->uniqueIdCompositions_[idCompositionIndex];

    simfil::StringId externalMapIdStringId = simfil::StringPool::Empty;
    if (externalMapId && *externalMapId != mapId()) {
        auto storedMapId = strings()->emplace(*externalMapId);
        if (!storedMapId) {
            raise(storedMapId.error().message);
        }
        externalMapIdStringId = *storedMapId;
    }

    auto const featureIdIndex = static_cast<uint32_t>(impl_->featureIds_.size());
    impl_->featureIds_.emplace_back(FeatureId::Data{
        false,
        idCompositionIndex,
        *typeIdStringId,
        idPartValuesToArrayIndex(*this, composition, featureIdParts),
        externalMapIdStringId,
    });
    return FeatureId(
        impl_->featureIds_.back(),
        shared_from_this(),
        {ColumnId::ExternalFeatureIds, featureIdIndex},
        mpKey_);
}

model_ptr<GeometryCollection> TileSearchResultLayer::newGeometryCollection(size_t initialCapacity, bool fixedSize)
{
    auto listIndex = arrayMemberStorage().new_array(initialCapacity, fixedSize);
    return GeometryCollection(shared_from_this(), {ColumnId::GeometryCollections, static_cast<uint32_t>(listIndex)}, mpKey_);
}

model_ptr<Geometry> TileSearchResultLayer::newGeometry(GeomType geomType, size_t initialCapacity, bool fixedSize)
{
    initialCapacity = std::max<size_t>(1, initialCapacity);
    auto const currentGeometryStage = [this]() -> std::optional<uint8_t> {
        auto stage = layerInfo_ ? layerInfo_->highFidelityStage_ : 0U;
        if (stage > std::numeric_limits<uint8_t>::max()) {
            raiseFmt("Geometry stage {} exceeds uint8_t range.", stage);
        }
        return static_cast<uint8_t>(stage);
    }();

    auto makeGeometry = [this, currentGeometryStage](uint8_t column, simfil::ArrayIndex vertexArray) {
        auto const geometryAddress = simfil::ModelNodeAddress{column, static_cast<uint32_t>(vertexArray)};
        setGeometryStage(geometryAddress, currentGeometryStage);
        return Geometry(shared_from_this(), geometryAddress, mpKey_);
    };

    switch (geomType) {
    case GeomType::Points:
        return makeGeometry(ColumnId::PointGeometries, impl_->pointBuffers_.new_array(initialCapacity, fixedSize));
    case GeomType::Line:
        return makeGeometry(ColumnId::LineGeometries, impl_->pointBuffers_.new_array(initialCapacity, fixedSize));
    case GeomType::Polygon:
        return makeGeometry(ColumnId::PolygonGeometries, impl_->pointBuffers_.new_array(initialCapacity, fixedSize));
    case GeomType::Mesh:
        return makeGeometry(ColumnId::MeshGeometries, impl_->pointBuffers_.new_array(initialCapacity, fixedSize));
    case GeomType::AABB:
        return makeGeometry(ColumnId::AabbGeometries, impl_->pointBuffers_.new_array(2, true));
    case GeomType::GltfNodeIndex:
        return makeGeometry(ColumnId::GltfNodeIndexGeometries, impl_->pointBuffers_.new_array(3, true));
    }
    raise("Unsupported geometry type.");
    return {};
}

model_ptr<Geometry> TileSearchResultLayer::newGeometryView(
    GeomType geomType,
    uint32_t offset,
    uint32_t size,
    model_ptr<Geometry> const& base)
{
    if (geomType == GeomType::AABB || geomType == GeomType::GltfNodeIndex) {
        raise("Geometry views are only supported for point-buffer-backed geometries.");
    }
    if (base->geomType() == GeomType::AABB || base->geomType() == GeomType::GltfNodeIndex) {
        raise("Geometry views cannot reference AABB or GltfNodeIndex geometries.");
    }
    impl_->geomViews_.emplace_back(geomType, offset, size, base->addr());
    return Geometry(
        &impl_->geomViews_.back(),
        shared_from_this(),
        {ColumnId::GeometryViews, static_cast<uint32_t>(impl_->geomViews_.size() - 1)},
        mpKey_);
}

model_ptr<SourceDataReferenceCollection> TileSearchResultLayer::newSourceDataReferenceCollection(
    std::span<QualifiedSourceDataReference> list)
{
    auto& arena = impl_->sourceDataReferences_;
    auto const index = static_cast<uint32_t>(arena.size());
    auto const size = static_cast<uint32_t>(list.size());
    arena.insert(arena.end(), list.begin(), list.end());
    return SourceDataReferenceCollection(
        index,
        size,
        shared_from_this(),
        ModelNodeAddress(ColumnId::SourceDataReferenceCollections, sourceDataAddressListToModelAddress(index, size)),
        mpKey_);
}

tl::expected<void, simfil::Error> TileSearchResultLayer::write(std::ostream& outputStream)
{
    TileLayer::write(outputStream);
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);
    impl_->readWrite(s);
    return ModelPool::write(outputStream);
}

nlohmann::json TileSearchResultLayer::toJson() const
{
    auto result = nlohmann::json::object({
        {"type", "SearchResultCollection"},
        {"mapgetTileId", tileId_.value_},
        {"mapId", mapId_},
        {"mapgetLayerId", layerInfo_->layerId_},
        {"geometryAnchor", {impl_->geometryAnchor_.x, impl_->geometryAnchor_.y, impl_->geometryAnchor_.z}},
        {"resultFields", impl_->resultFields_},
        {"info", info_},
        {"results", nlohmann::json::array()},
    });
    if (impl_->stage_) {
        result["mapgetStage"] = *impl_->stage_;
    }
    for (size_t i = 0; i < size(); ++i) {
        auto searchResult = at(i);
        if (searchResult) {
            result["results"].push_back(searchResult->toJson());
        }
    }
    return result;
}

size_t TileSearchResultLayer::size() const
{
    return numRoots();
}

model_ptr<SearchResult> TileSearchResultLayer::at(size_t index) const
{
    auto rootResult = root(index);
    if (!rootResult) {
        return {};
    }
    return resolve<SearchResult>(**rootResult);
}

Point TileSearchResultLayer::geometryAnchor() const
{
    return impl_->geometryAnchor_;
}

void TileSearchResultLayer::setGeometryAnchor(Point const& anchor)
{
    impl_->geometryAnchor_ = anchor;
}

tl::expected<void, simfil::Error> TileSearchResultLayer::resolve(simfil::ModelNode const& n, ResolveFn const& cb) const
{
    switch (n.addr().column()) {
    case ColumnId::SearchResults:
        cb(*resolve<SearchResult>(n));
        return {};
    case ColumnId::FeatureIds:
    case ColumnId::ExternalFeatureIds:
        cb(*resolve<FeatureId>(n));
        return {};
    case ColumnId::Points:
    case ColumnId::GeometryPointView:
        cb(*resolve<PointNode>(n));
        return {};
    case ColumnId::PointBuffers:
    case ColumnId::PointBuffersView:
        cb(*resolve<PointBufferNode>(n));
        return {};
    case ColumnId::PointGeometries:
    case ColumnId::LineGeometries:
    case ColumnId::PolygonGeometries:
    case ColumnId::MeshGeometries:
    case ColumnId::AabbGeometries:
    case ColumnId::GltfNodeIndexGeometries:
    case ColumnId::GeometryViews:
        cb(*resolve<Geometry>(n));
        return {};
    case ColumnId::GeometryCollections:
        cb(*resolve<GeometryCollection>(n));
        return {};
    case ColumnId::GeometryArrayView:
        cb(*resolve<GeometryArrayView>(n));
        return {};
    case ColumnId::GeometryBoundsInfoView:
        cb(*resolve<BoundsInfoNode>(n));
        return {};
    case ColumnId::GeometryBoundsPolygonCoordinatesView:
        cb(*resolve<BoundsPolygonCoordinatesNode>(n));
        return {};
    case ColumnId::GeometryBoundsRingView:
        cb(*resolve<BoundsRingNode>(n));
        return {};
    case ColumnId::Polygon:
        cb(*resolve<PolygonNode>(n));
        return {};
    case ColumnId::Mesh:
        cb(*resolve<MeshNode>(n));
        return {};
    case ColumnId::MeshTriangleCollection:
        cb(*resolve<MeshTriangleCollectionNode>(n));
        return {};
    case ColumnId::MeshTriangleLinearRing:
    case ColumnId::LinearRing:
        cb(*resolve<LinearRingNode>(n));
        return {};
    case ColumnId::SourceDataReferenceCollections:
        cb(*resolve<SourceDataReferenceCollection>(n));
        return {};
    case ColumnId::SourceDataReferences:
        cb(*resolve<SourceDataReferenceItem>(n));
        return {};
    default:
        return ModelPool::resolve(n, cb);
    }
}

TileFeatureModelLayerBase::GeometryStorage& TileSearchResultLayer::vertexBufferStorage()
{
    return impl_->pointBuffers_;
}

GeometryViewData const* TileSearchResultLayer::geometryViewData(simfil::ModelNodeAddress address) const
{
    if (address.column() != ColumnId::GeometryViews || address.index() >= impl_->geomViews_.size()) {
        return nullptr;
    }
    return &impl_->geomViews_.at(address.index());
}

std::optional<uint8_t> TileSearchResultLayer::geometryStage(simfil::ModelNodeAddress address) const
{
    if (!isBaseGeometryColumn(address.column()) && address.column() != ColumnId::GeometryViews) {
        return std::nullopt;
    }
    auto const storageIndex = address.column() == ColumnId::GeometryViews
        ? address.index()
        : extraGeometryDataStorageIndex(static_cast<simfil::ArrayIndex>(address.index()));
    return geometryStageAt(impl_->geomStages_, storageIndex);
}

void TileSearchResultLayer::setGeometryStage(simfil::ModelNodeAddress address, std::optional<uint8_t> stage)
{
    if (!isBaseGeometryColumn(address.column()) && address.column() != ColumnId::GeometryViews) {
        raise("Geometry stage can only be stored on geometry nodes.");
    }
    auto const storageIndex = address.column() == ColumnId::GeometryViews
        ? address.index()
        : extraGeometryDataStorageIndex(static_cast<simfil::ArrayIndex>(address.index()));
    ensureGeometryStageCapacity(impl_->geomStages_, storageIndex);
    impl_->geomStages_.at(storageIndex) = stage.value_or(InvalidGeometryStage);
}

simfil::ModelNodeAddress TileSearchResultLayer::geometrySourceDataReferences(simfil::ModelNodeAddress address) const
{
    if (!isBaseGeometryColumn(address.column())) {
        return {};
    }
    return geometrySourceRefsAt(
        impl_->geomSourceDataRefs_,
        extraGeometryDataStorageIndex(static_cast<simfil::ArrayIndex>(address.index())));
}

void TileSearchResultLayer::setGeometrySourceDataReferences(
    simfil::ModelNodeAddress address,
    simfil::ModelNodeAddress refsAddress)
{
    if (!isBaseGeometryColumn(address.column())) {
        raise("Source data references can only be stored on base geometry nodes.");
    }
    auto const storageIndex = extraGeometryDataStorageIndex(static_cast<simfil::ArrayIndex>(address.index()));
    ensureGeometrySourceRefCapacity(impl_->geomSourceDataRefs_, storageIndex);
    impl_->geomSourceDataRefs_.at(storageIndex) = refsAddress;
}

model_ptr<FeatureId> TileSearchResultLayer::resolveFeatureIdNode(simfil::ModelNode const& node) const
{
    if (node.addr().column() != ColumnId::ExternalFeatureIds) {
        raise("Cannot cast this node to a FeatureId.");
    }
    return FeatureId(impl_->featureIds_[node.addr().index()], shared_from_this(), node.addr(), mpKey_);
}

model_ptr<PointNode> TileSearchResultLayer::resolvePointNode(simfil::ModelNode const& node) const
{
    switch (node.addr().column()) {
    case ColumnId::Points:
        return PointNode(node, static_cast<simfil::ArrayIndex>(node.addr().index()), mpKey_);
    case ColumnId::GeometryPointView:
        return PointNode(node, mpKey_);
    default:
        raise("Cannot cast this node to a Point.");
    }
}

model_ptr<PointBufferNode> TileSearchResultLayer::resolvePointBufferNode(simfil::ModelNode const& node) const
{
    if (auto existing = dynamic_cast<PointBufferNode const*>(&node)) {
        return PointBufferNode(shared_from_this(), existing->baseGeometryAddress(), mpKey_);
    }
    switch (node.addr().column()) {
    case ColumnId::PointBuffers:
        return PointBufferNode(
            shared_from_this(),
            ModelNodeAddress{ColumnId::PointGeometries, node.addr().index()},
            mpKey_);
    case ColumnId::PointBuffersView:
        return PointBufferNode(
            shared_from_this(),
            ModelNodeAddress{ColumnId::GeometryViews, node.addr().index()},
            mpKey_);
    default:
        raise("Cannot cast this node to a PointBuffer.");
    }
}

model_ptr<Geometry> TileSearchResultLayer::resolveGeometryNode(simfil::ModelNode const& node) const
{
    switch (node.addr().column()) {
    case ColumnId::PointGeometries:
    case ColumnId::LineGeometries:
    case ColumnId::PolygonGeometries:
    case ColumnId::MeshGeometries:
    case ColumnId::AabbGeometries:
    case ColumnId::GltfNodeIndexGeometries:
        return Geometry(shared_from_this(), node.addr(), mpKey_);
    case ColumnId::GeometryViews: {
        auto* geomData = &impl_->geomViews_.at(node.addr().index());
        using MutableGeomData = std::remove_const_t<std::remove_reference_t<decltype(*geomData)>>;
        return Geometry(const_cast<MutableGeomData*>(geomData), shared_from_this(), node.addr(), mpKey_);
    }
    default:
        raise("Cannot cast this node to a Geometry.");
    }
}

model_ptr<GeometryCollection> TileSearchResultLayer::resolveGeometryCollectionNode(simfil::ModelNode const& node) const
{
    if (node.addr().column() != ColumnId::GeometryCollections &&
        !isBaseGeometryColumn(node.addr().column()) &&
        node.addr().column() != ColumnId::GeometryViews) {
        raise("Cannot cast this node to a GeometryCollection.");
    }
    return GeometryCollection(shared_from_this(), node.addr(), mpKey_);
}

model_ptr<GeometryArrayView> TileSearchResultLayer::resolveGeometryArrayViewNode(simfil::ModelNode const& node) const
{
    if (node.addr().column() != ColumnId::GeometryArrayView) {
        raise("Cannot cast this node to a GeometryArrayView.");
    }
    return GeometryArrayView(shared_from_this(), node.addr(), mpKey_);
}

model_ptr<BoundsInfoNode> TileSearchResultLayer::resolveBoundsInfoNode(simfil::ModelNode const& node) const
{
    if (node.addr().column() != ColumnId::GeometryBoundsInfoView) {
        raise("Cannot cast this node to BoundsInfo.");
    }
    return BoundsInfoNode(node, mpKey_);
}

model_ptr<BoundsPolygonCoordinatesNode> TileSearchResultLayer::resolveBoundsPolygonCoordinatesNode(
    simfil::ModelNode const& node) const
{
    if (node.addr().column() != ColumnId::GeometryBoundsPolygonCoordinatesView) {
        raise("Cannot cast this node to BoundsPolygonCoordinates.");
    }
    return BoundsPolygonCoordinatesNode(node, mpKey_);
}

model_ptr<BoundsRingNode> TileSearchResultLayer::resolveBoundsRingNode(simfil::ModelNode const& node) const
{
    if (node.addr().column() != ColumnId::GeometryBoundsRingView) {
        raise("Cannot cast this node to BoundsRing.");
    }
    return BoundsRingNode(node, mpKey_);
}

model_ptr<MeshNode> TileSearchResultLayer::resolveMeshNode(simfil::ModelNode const& node) const
{
    return MeshNode(shared_from_this(), node.addr(), mpKey_);
}

model_ptr<MeshTriangleCollectionNode> TileSearchResultLayer::resolveMeshTriangleCollectionNode(
    simfil::ModelNode const& node) const
{
    return MeshTriangleCollectionNode(node, mpKey_);
}

model_ptr<LinearRingNode> TileSearchResultLayer::resolveLinearRingNode(simfil::ModelNode const& node) const
{
    switch (node.addr().column()) {
    case ColumnId::LinearRing:
        return LinearRingNode(node, mpKey_);
    case ColumnId::MeshTriangleLinearRing:
        return LinearRingNode(node, 3, mpKey_);
    default:
        raise("Cannot cast this node to a LinearRing.");
    }
}

model_ptr<PolygonNode> TileSearchResultLayer::resolvePolygonNode(simfil::ModelNode const& node) const
{
    return PolygonNode(shared_from_this(), node.addr(), mpKey_);
}

model_ptr<SourceDataReferenceCollection> TileSearchResultLayer::resolveSourceDataReferenceCollectionNode(
    simfil::ModelNode const& node) const
{
    if (node.addr().column() != ColumnId::SourceDataReferenceCollections) {
        raise("Cannot cast this node to a SourceDataReferenceCollection.");
    }
    auto [index, size] = modelAddressToSourceDataAddressList(node.addr().index());
    return SourceDataReferenceCollection(index, size, shared_from_this(), node.addr(), mpKey_);
}

model_ptr<SourceDataReferenceItem> TileSearchResultLayer::resolveSourceDataReferenceItemNode(
    simfil::ModelNode const& node) const
{
    if (node.addr().column() != ColumnId::SourceDataReferences) {
        raise("Cannot cast this node to a SourceDataReferenceItem.");
    }
    auto const* data = &impl_->sourceDataReferences_.at(node.addr().index());
    return SourceDataReferenceItem(data, shared_from_this(), node.addr(), mpKey_);
}

using simfil::ModelNode;
using simfil::res::tag;

template<>
model_ptr<SearchResult> resolveInternal(tag<SearchResult>, TileSearchResultLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileSearchResultLayer::ColumnId::SearchResults) {
        raise("Cannot cast this node to a SearchResult.");
    }
    return SearchResult(
        const_cast<SearchResult::Data*>(&model.impl_->searchResults_.at(node.addr().index())),
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

} // namespace mapget
