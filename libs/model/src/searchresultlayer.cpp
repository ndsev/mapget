#include "searchresultlayer.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <type_traits>

#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/bitsery.h>
#include <bitsery/ext/std_optional.h>
#include <bitsery/ext/std_bitset.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>

#include "mapget/log.h"
#include "pointnode.h"
#include "simfil/model/bitsery-traits.h"
#include "simfil/simfil.h"
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

/** Convert parsed SIMFIL diagnostics to the JSON shape used by tests and status tooling. */
nlohmann::json diagnosticsToJson(simfil::Diagnostics const& diagnostics)
{
    auto result = nlohmann::json::array();
    auto messages = simfil::diagnostics(diagnostics);
    if (!messages) {
        return result;
    }
    for (auto const& message : *messages) {
        auto item = nlohmann::json::object({
            {"message", message.message},
            {"location", {
                {"offset", message.location.offset},
                {"size", message.location.size},
            }},
        });
        if (message.fix) {
            item["fix"] = *message.fix;
        }
        result.push_back(std::move(item));
    }
    return result;
}

/** Serialize parsed diagnostics with the same binary layout as simfil::Diagnostics::write/read. */
template<typename S>
void readWriteDiagnostics(S& s, simfil::Diagnostics& data)
{
    s.container(data.exprIndex_, std::numeric_limits<uint16_t>::max(), [](auto& s2, std::uint32_t& v) {
        s2.value4b(v);
    });
    s.container(data.fieldData_, std::numeric_limits<uint16_t>::max(), [](auto& s2, simfil::Diagnostics::FieldExprData& data) {
        s2.value4b(data.location.offset);
        s2.value4b(data.location.size);
        s2.value4b(data.hits);
        s2.value4b(data.evaluations);
        s2.text1b(data.name, 0xff);
    });
    s.container(data.comparisonData_, std::numeric_limits<uint16_t>::max(), [](auto& s2, simfil::Diagnostics::ComparisonExprData& data) {
        s2.value4b(data.location.offset);
        s2.value4b(data.location.size);
        s2.ext(data.leftTypes.flags, bitsery::ext::StdBitset{});
        s2.ext(data.rightTypes.flags, bitsery::ext::StdBitset{});
        s2.value4b(data.evaluations);
        s2.value4b(data.trueResults);
        s2.value4b(data.falseResults);
    });
}
} // namespace

struct TileSearchResultLayer::Impl
{
    Point geometryAnchor_{};
    std::optional<uint32_t> stage_;
    std::vector<std::string> resultFields_;
    simfil::ModelColumn<SearchResult::Data, simfil::detail::ColumnPageSize / 2> searchResults_;
    simfil::Diagnostics diagnostics_;

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
        s.object(searchResults_);
        readWriteDiagnostics(s, diagnostics_);
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
    return attributeIndex() ? 6U : 3U;
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
    case 4: return attributeIndex() ? static_cast<simfil::StringId>(StringPool::ValidityIndexStr) : simfil::StringPool::Empty;
    case 5: return attributeIndex() ? static_cast<simfil::StringId>(StringPool::ValidityCountStr) : simfil::StringPool::Empty;
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
    readWriteCommonColumns(s);
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

void TileSearchResultLayer::setDiagnostics(simfil::Diagnostics const& diagnostics)
{
    impl_->diagnostics_.exprIndex_.clear();
    impl_->diagnostics_.fieldData_.clear();
    impl_->diagnostics_.comparisonData_.clear();
    impl_->diagnostics_.append(diagnostics);
}

simfil::Diagnostics const& TileSearchResultLayer::diagnostics() const
{
    return impl_->diagnostics_;
}

model_ptr<SearchResult> TileSearchResultLayer::newSearchResult(
    model_ptr<FeatureId> const& featureId,
    model_ptr<GeometryCollection> const& geometry,
    std::span<simfil::ModelNode::Ptr const> values,
    std::optional<uint32_t> attributeIndex,
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

    auto const resultIndex = static_cast<uint32_t>(impl_->searchResults_.size());
    impl_->searchResults_.emplace_back(SearchResult::Data{
        featureId->addr(),
        geometry->addr(),
        static_cast<simfil::ArrayIndex>(valueArray->addr().index()),
        attributeIndex.value_or(SearchResult::InvalidAttributeIndex),
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

    auto const featureIdIndex = static_cast<uint32_t>(featureIds_.size());
    featureIds_.emplace_back(FeatureId::Data{
        false,
        idCompositionIndex,
        *typeIdStringId,
        idPartValuesToArrayIndex(*this, composition, featureIdParts),
        externalMapIdStringId,
    });
    return FeatureId(
        featureIds_.back(),
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
        return makeGeometry(ColumnId::PointGeometries, pointBuffers_.new_array(initialCapacity, fixedSize));
    case GeomType::Line:
        return makeGeometry(ColumnId::LineGeometries, pointBuffers_.new_array(initialCapacity, fixedSize));
    case GeomType::Polygon:
        return makeGeometry(ColumnId::PolygonGeometries, pointBuffers_.new_array(initialCapacity, fixedSize));
    case GeomType::Mesh:
        return makeGeometry(ColumnId::MeshGeometries, pointBuffers_.new_array(initialCapacity, fixedSize));
    case GeomType::AABB:
        return makeGeometry(ColumnId::AabbGeometries, pointBuffers_.new_array(2, true));
    case GeomType::GltfNodeIndex:
        return makeGeometry(ColumnId::GltfNodeIndexGeometries, pointBuffers_.new_array(3, true));
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
    geomViews_.emplace_back(geomType, offset, size, base->addr());
    return Geometry(
        &geomViews_.back(),
        shared_from_this(),
        {ColumnId::GeometryViews, static_cast<uint32_t>(geomViews_.size() - 1)},
        mpKey_);
}

model_ptr<SourceDataReferenceCollection> TileSearchResultLayer::newSourceDataReferenceCollection(
    std::span<QualifiedSourceDataReference> list)
{
    auto& arena = sourceDataReferences_;
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
    readWriteCommonColumns(s);
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
    auto diagnosticsJson = diagnosticsToJson(impl_->diagnostics_);
    if (!diagnosticsJson.empty()) {
        result["diagnostics"] = std::move(diagnosticsJson);
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
