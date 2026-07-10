#include "featurelayer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <bitsery/bitsery.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/ext/std_optional.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>

#include <nlohmann/json-schema.hpp>

#include "simfil/environment.h"
#include "simfil/model/arena.h"
#include "simfil/model/bitsery-traits.h"
#include "simfil/model/nodes.h"
#include "mapget/log.h"
#include "simfil/model/string-pool.h"
#include "simfilutil.h"
#include "simfilexpressioncache.h"
#include "sourcedatareference.h"
#include "sourceinfo.h"
#include "hash.h"

/** Bitsery serialization traits */
namespace bitsery
{

template <typename S>
void serialize(S& s, glm::vec3& v) {
    s.value4b(v.x);
    s.value4b(v.y);
    s.value4b(v.z);
}

template <typename S>
void serialize(S& s, mapget::TileGlbAttachment& attachment)
{
    s.text1b(attachment.name_, std::numeric_limits<uint32_t>::max());
    s.container1b(attachment.bytes_, std::numeric_limits<uint32_t>::max());
}

}

namespace
{
    using GeometryPointBufferArena =
        simfil::ArrayArena<glm::vec3, simfil::detail::ColumnPageSize * 2>;

    /**
     * Views into the sourceDataAddresses_ array are stored as a single u32, which
     * uses 20 bits for the index and 4 bits for the length.
     */
    constexpr uint32_t SourceAddressArenaIndexBits = 20;
    constexpr uint32_t SourceAddressArenaIndexMax = (~static_cast<uint32_t>(0)) >> (32 - SourceAddressArenaIndexBits);
    constexpr uint32_t SourceAddressArenaSizeBits  = 4;
    constexpr uint32_t SourceAddressArenaSizeMax = (~static_cast<uint32_t>(0)) >> (32 - SourceAddressArenaSizeBits);
    std::tuple<size_t, size_t> modelAddressToSourceDataAddressList(uint32_t addr)
    {
        const auto index = addr >> SourceAddressArenaSizeBits;
        const auto size = addr & SourceAddressArenaSizeMax;

        return {index, size};
    }

    uint32_t sourceDataAddressListToModelAddress(uint32_t index, uint32_t size)
    {
        if (index > SourceAddressArenaIndexMax)
            throw std::out_of_range("Index out of range");
        if (size > SourceAddressArenaIndexMax)
            throw std::out_of_range("Size out of range");
        return (index << SourceAddressArenaSizeBits) | size;
    }
}

namespace mapget
{

namespace
{
bool isBufferedGeometryColumn(uint8_t column)
{
    using Col = TileFeatureLayer::ColumnId;
    return column == Col::LineGeometries ||
           column == Col::PolygonGeometries ||
           column == Col::MeshGeometries ||
           column == Col::AabbGeometries ||
           column == Col::GltfNodeIndexGeometries;
}

bool isBaseGeometryColumn(uint8_t column)
{
    using Col = TileFeatureLayer::ColumnId;
    return column == Col::PointGeometries ||
           column == Col::GltfNodeIndexGeometries ||
           isBufferedGeometryColumn(column);
}

GeomType geometryTypeForColumn(uint8_t column)
{
    using Col = TileFeatureLayer::ColumnId;
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

constexpr uint8_t InvalidGeometryStage = std::numeric_limits<uint8_t>::max();

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

    // Base geometries use point-buffer array handles as their geometry index.
    // Regular handles and singleton handles live in disjoint source domains:
    // regular arrays are plain indices, while singleton handles are encoded as
    // 0x00800000 | payload. We remap them into one collision-free auxiliary
    // storage space for geometry stages and extra geometry data by reserving
    // even indices for regular arrays and odd indices for singleton payloads:
    //   regular n    -> 2 * n
    //   singleton p  -> 2 * p + 1
    // This keeps the storage compact without materializing huge sparse columns.
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

void ensureFeatureComplexDataRefCapacity(
    simfil::ModelColumn<simfil::ModelNodeAddress, simfil::detail::ColumnPageSize / 2>& refs,
    uint32_t index)
{
    while (refs.size() <= static_cast<size_t>(index)) {
        refs.emplace_back(simfil::ModelNodeAddress{});
    }
}
}

struct FeatureAddrWithIdHash
{
    MODEL_COLUMN_TYPE(8);

    ModelNodeAddress featureAddr_{};
    uint32_t idHash_ = 0;

    FeatureAddrWithIdHash() = default;
    FeatureAddrWithIdHash(ModelNodeAddress featureAddr, uint32_t idHash)
        : featureAddr_(featureAddr),
          idHash_(idHash)
    {}

    bool operator< (FeatureAddrWithIdHash const& other) const {
        return std::tie(idHash_, featureAddr_) < std::tie(other.idHash_, other.featureAddr_);
    }
};

struct TileFeatureLayer::Impl {
    ModelNodeAddress featureIdPrefix_;
    Point geometryAnchor_{};
    std::optional<TileGlbAttachment> glbAttachment_;

    simfil::ModelColumn<Feature::BasicData, simfil::detail::ColumnPageSize / 4> features_;
    simfil::ModelColumn<Feature::ComplexData, simfil::detail::ColumnPageSize / 4> complexFeatureData_;
    simfil::ModelColumn<simfil::ModelNodeAddress, simfil::detail::ColumnPageSize / 2> complexFeatureDataRefs_;
    simfil::ModelColumn<Attribute::Data, simfil::detail::ColumnPageSize> attributes_;
    simfil::ModelColumn<Validity::Data, simfil::detail::ColumnPageSize> validities_;
    simfil::ModelColumn<simfil::ArrayIndex, simfil::detail::ColumnPageSize / 2> attrLayers_;
    simfil::ModelColumn<simfil::ArrayIndex, simfil::detail::ColumnPageSize / 2> attrLayerLists_;
    simfil::ModelColumn<Relation::Data, simfil::detail::ColumnPageSize / 2> relations_;

    /**
     * Indexing of features by their id hash. The hash-feature pairs are kept
     * in a vector, which is kept in a sorted state. This allows finding a
     * feature by its id in O(log(n)) time.
     */
    simfil::ModelColumn<FeatureAddrWithIdHash, simfil::detail::ColumnPageSize / 4> featureHashIndex_;
    bool featureHashIndexNeedsSorting_ = false;

    void sortFeatureHashIndex() {
        if (!featureHashIndexNeedsSorting_)
            return;
        featureHashIndexNeedsSorting_ = false;
        std::sort(featureHashIndex_.begin(), featureHashIndex_.end());
    }

    // SIMFIL schema lookup and compiled expression cache.
    std::shared_ptr<LayerSchema const> layerSchema_;
    SimfilExpressionCache expressionCache_;

    static std::unique_ptr<simfil::Environment> makeSchemaAwareEnvironment(
        std::shared_ptr<simfil::StringPool> stringPool,
        std::shared_ptr<LayerSchema const> layerSchema)
    {
        auto env = makeEnvironment(stringPool);
        installLayerSchema(*env, std::move(layerSchema), std::move(stringPool));
        return env;
    }

    static std::unique_ptr<simfil::Environment> makeSchemaAwareCompletionEnvironment(
        std::shared_ptr<simfil::StringPool> stringPool,
        std::shared_ptr<LayerSchema const> layerSchema)
    {
        auto env = makeEnvironment(stringPool);
        installCompletionLayerSchema(*env, std::move(layerSchema), std::move(stringPool));
        return env;
    }

    // (De-)Serialization
    template<typename S>
    void readWrite(S& s) {
        s.value8b(geometryAnchor_.x);
        s.value8b(geometryAnchor_.y);
        s.value8b(geometryAnchor_.z);
        s.ext(glbAttachment_, bitsery::ext::StdOptional{});
        s.object(features_);
        s.object(complexFeatureData_);
        s.object(complexFeatureDataRefs_);
        s.object(attributes_);
        s.object(validities_);
        s.object(attrLayers_);
        s.object(attrLayerLists_);
        s.object(featureIdPrefix_);
        s.object(relations_);
        sortFeatureHashIndex();
        s.object(featureHashIndex_);
    }

    Impl(
        std::shared_ptr<simfil::StringPool> stringPool,
        std::shared_ptr<LayerInfo> const& layerInfo)
        : layerSchema_(layerInfo ? layerInfo->layerSchema() : nullptr),
          expressionCache_(
              makeSchemaAwareEnvironment(std::move(stringPool), layerSchema_),
              [this]() {
                  auto compileStrings = std::make_shared<simfil::StringPool>(*expressionCache_.environment().strings());
                  return makeSchemaAwareCompletionEnvironment(std::move(compileStrings), layerSchema_);
              })
    {
    }

};

nlohmann::json TileGlbAttachment::toJsonMetadata() const
{
    return nlohmann::json::object({
        {"name", name_},
        {"mimeType", std::string(TileFeatureLayer::GLB_ATTACHMENT_MIME_TYPE)},
        {"sizeBytes", bytes_.size()},
    });
}

TileFeatureLayer::TileFeatureLayer(
    TileId tileId,
    std::string const& nodeId,
    std::string const& mapId,
    std::shared_ptr<LayerInfo> const& layerInfo,
    std::shared_ptr<simfil::StringPool> const& strings) :
    TileFeatureModelLayerBase(tileId, nodeId, mapId, layerInfo, strings),
    impl_(std::make_unique<Impl>(strings, layerInfo))
{
    impl_->geometryAnchor_ = Point(tileId.centerWgs84());
}

TileFeatureLayer::TileFeatureLayer(
    const std::vector<uint8_t>& input,
    LayerInfoResolveFun const& layerInfoResolveFun,
    StringPoolResolveFun const& stringPoolGetter
) :
    TileFeatureModelLayerBase(input, layerInfoResolveFun, stringPoolGetter, &deserializationOffsetBytes_),
    impl_(std::make_unique<Impl>(strings(), layerInfo_))
{
    impl_->geometryAnchor_ = Point(tileId_.centerWgs84());
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    if (deserializationOffsetBytes_ > input.size()) {
        raise("Failed to read TileFeatureLayer: invalid deserialization offset.");
    }
    bitsery::Deserializer<Adapter> s(Adapter(
        input.begin() + static_cast<std::ptrdiff_t>(deserializationOffsetBytes_),
        input.end()));
    s.ext4b(stage_, bitsery::ext::StdOptional{});
    impl_->readWrite(s);
    readWriteCommonColumns(s);
    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raise(fmt::format(
            "Failed to read TileFeatureLayer: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error())));
    }
    const auto modelOffset = deserializationOffsetBytes_ + s.adapter().currentReadPos();
    if (auto result = ModelPool::read(input, modelOffset); !result) {
        raise(result.error().message);
    }
}

Point TileFeatureLayer::geometryAnchor() const
{
    return impl_->geometryAnchor_;
}

void TileFeatureLayer::setGeometryAnchor(Point const& anchor)
{
    impl_->geometryAnchor_ = anchor;
}

TileFeatureLayer::~TileFeatureLayer() = default;

std::optional<uint32_t> TileFeatureLayer::stage() const
{
    return stage_;
}

void TileFeatureLayer::setStage(std::optional<uint32_t> stage)
{
    stage_ = stage;
}

TileGlbAttachment const* TileFeatureLayer::glbAttachment() const
{
    return impl_->glbAttachment_ ? &*impl_->glbAttachment_ : nullptr;
}

void TileFeatureLayer::setGlbAttachment(std::string name, std::vector<uint8_t> bytes)
{
    if (name.empty()) {
        raise("GLB attachment name must not be empty.");
    }
    impl_->glbAttachment_ = TileGlbAttachment{
        std::move(name),
        std::move(bytes),
    };
}

void TileFeatureLayer::clearGlbAttachment()
{
    impl_->glbAttachment_.reset();
}

void TileFeatureLayer::setExpectedFeatureSequence(std::vector<std::string> expectedFeatureIds)
{
    expectedFeatureIds_ = std::move(expectedFeatureIds);
}

void TileFeatureLayer::clearExpectedFeatureSequence()
{
    expectedFeatureIds_.clear();
}

bool TileFeatureLayer::hasExpectedFeatureSequence() const
{
    return !expectedFeatureIds_.empty();
}

void TileFeatureLayer::validateExpectedFeatureSequenceComplete() const
{
    if (expectedFeatureIds_.empty()) {
        return;
    }

    auto const createdFeatureCount = impl_->features_.size();
    if (createdFeatureCount == expectedFeatureIds_.size()) {
        return;
    }

    if (createdFeatureCount < expectedFeatureIds_.size()) {
        auto const& nextExpectedId = expectedFeatureIds_[createdFeatureCount];
        raiseFmt(
            "Feature sequence incomplete: created {} of {} expected features. Next expected id: {}.",
            createdFeatureCount,
            expectedFeatureIds_.size(),
            nextExpectedId);
    }

    raiseFmt(
        "Feature sequence overflow: created {} features, expected {}.",
        createdFeatureCount,
        expectedFeatureIds_.size());
}

Feature::ComplexData const* TileFeatureLayer::featureComplexDataOrNull(uint32_t featureIndex) const
{
    if (featureIndex >= impl_->complexFeatureDataRefs_.size()) {
        return nullptr;
    }
    auto const addr = impl_->complexFeatureDataRefs_.at(featureIndex);
    if (!addr) {
        return nullptr;
    }
    if (addr.column() != ColumnId::FeatureComplexData) {
        raiseFmt("Invalid complex feature data address column {}.", addr.column());
    }
    if (addr.index() >= impl_->complexFeatureData_.size()) {
        raiseFmt("Complex feature data index {} is out of bounds.", addr.index());
    }
    return &impl_->complexFeatureData_.at(addr.index());
}

Feature::ComplexData* TileFeatureLayer::featureComplexDataOrNull(uint32_t featureIndex)
{
    return const_cast<Feature::ComplexData*>(
        static_cast<TileFeatureLayer const&>(*this).featureComplexDataOrNull(featureIndex));
}

Feature::ComplexData& TileFeatureLayer::ensureFeatureComplexData(uint32_t featureIndex)
{
    ensureFeatureComplexDataRefCapacity(impl_->complexFeatureDataRefs_, featureIndex);
    auto& addr = impl_->complexFeatureDataRefs_.at(featureIndex);
    if (!addr) {
        auto const complexIndex = static_cast<uint32_t>(impl_->complexFeatureData_.size());
        impl_->complexFeatureData_.emplace_back(Feature::ComplexData{});
        addr = simfil::ModelNodeAddress{ColumnId::FeatureComplexData, complexIndex};
    }
    if (addr.column() != ColumnId::FeatureComplexData) {
        raiseFmt("Invalid complex feature data address column {}.", addr.column());
    }
    return impl_->complexFeatureData_.at(addr.index());
}

void TileFeatureLayer::attachOverlay(TileFeatureLayer::Ptr const& overlay)
{
    if (!overlay) {
        return;
    }
    if (overlay.get() == this) {
        raise("Cannot attach a feature layer as its own overlay.");
    }

    if (overlay->size() < size()) {
        raiseFmt(
            "Overlay feature count {} is smaller than base feature count {}.",
            overlay->size(),
            size());
    }

    // Search may assemble the same cached stage stack repeatedly. Treat an
    // already-attached overlay as a no-op so the chain cannot grow unbounded.
    for (auto cursor = this->overlay(); cursor; cursor = cursor->overlay()) {
        if (cursor == overlay) {
            return;
        }
    }

    TileFeatureLayer::Ptr currentOverlay;
    {
        std::lock_guard lock(overlayMutex_);
        if (!overlay_) {
            overlay_ = overlay;
            return;
        }
        currentOverlay = overlay_;
    }

    if (currentOverlay == overlay) {
        return;
    }
    currentOverlay->attachOverlay(overlay);
}

TileFeatureLayer::Ptr TileFeatureLayer::overlay() const
{
    std::lock_guard lock(overlayMutex_);
    return overlay_;
}

namespace
{

/**
 * Create a string representation of the given id parts.
 */
std::string idPartsToString(KeyValueViewPairs const& idParts) {
    fmt::memory_buffer result;
    fmt::format_to(std::back_inserter(result), FMT_STRING("{{"));
    for (auto i = 0; i < idParts.size(); ++i) {
        if (i > 0) {
            fmt::format_to(std::back_inserter(result), FMT_STRING(", "));
        }
        std::visit([&result, key = idParts[i].first](auto&& value){
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, bool>) {
                fmt::format_to(std::back_inserter(result), FMT_STRING("{}: {:d}"), key, value);
            }
            else {
                fmt::format_to(std::back_inserter(result), FMT_STRING("{}: {}"), key, value);
            }
        }, idParts[i].second);
    }
    fmt::format_to(std::back_inserter(result), FMT_STRING("}}"));
    return fmt::to_string(result);
}

/**
 * Remove id parts from a keysAndValues list which are optional under
 * the given composition. Note: The ordering of keys in the keysAndValues
 * list must match the ordering in the composition. E.g., if `areaId` comes
 * before `featureId`, it will only be recognized if this order is also
 * maintained in the keysAndValues list.
 */
KeyValueViewPairs
stripOptionalIdParts(KeyValueViewPairs const& keysAndValues, std::vector<IdPart> const& composition)
{
    KeyValueViewPairs result;
    result.reserve(keysAndValues.size());
    auto idPartIt = composition.begin();

    for (auto const& [key, value] : keysAndValues) {
        bool isOptional = true;
        while (idPartIt != composition.end()) {
            if (key == idPartIt->idPartLabel_) {
                isOptional = idPartIt->isOptional_;
                ++idPartIt;
                break;
            }
            ++idPartIt;
        }
        if (!isOptional)
            result.emplace_back(key, value);
    }

    return result;
}

/**
 * Materialize per-feature ID values aligned to the composition suffix that starts
 * after the tile-level common prefix. Omitted optional parts in that suffix are
 * stored as null sentinels to keep the local feature ID shape stable.
 */
simfil::ArrayIndex idPartValuesToArrayIndex(
    TileFeatureLayer& layer,
    std::vector<IdPart> const& composition,
    KeyValueViewPairs const& idParts,
    uint32_t compositionStartIndex = 0)
{
    auto idValues = layer.newArray(
        composition.size() - std::min<uint32_t>(
            compositionStartIndex,
            static_cast<uint32_t>(composition.size())),
        true);
    auto idPartsIter = idParts.begin();

    for (uint32_t compositionIndex = compositionStartIndex;
         compositionIndex < composition.size();
         ++compositionIndex) {
        auto const& idPart = composition[compositionIndex];
        if (idPartsIter != idParts.end() && idPart.idPartLabel_ == idPartsIter->first) {
            idValues->append(std::visit(
                [&](auto&& v) -> simfil::ModelNode::Ptr {
                    return layer.newValue(v);
                },
                idPartsIter->second));
            ++idPartsIter;
            continue;
        }

        if (!idPart.isOptional_) {
            raiseFmt(
                "Missing non-optional ID part '{}' while materializing feature ID values.",
                idPart.idPartLabel_);
        }

        idValues->append(
            layer.resolve<simfil::ModelNode>(
                simfil::ModelNodeAddress{simfil::Model::Null, 1},
                simfil::ScalarValueType{}));
    }

    if (idPartsIter != idParts.end()) {
        raiseFmt(
            "Unexpected trailing ID part '{}' while materializing feature ID values.",
            idPartsIter->first);
    }

    return static_cast<simfil::ArrayIndex>(idValues->addr().index());
}

}  // namespace

simfil::model_ptr<Feature> TileFeatureLayer::newFeature(
    const std::string_view& typeId,
    const KeyValueViewPairs& featureIdParts)
{
    if (featureIdParts.empty()) {
        raise("Tried to create an empty feature ID.");
    }

    KeyValueViewPairs fullFeatureIdParts;
    KeyValueViewPairs prefixFeatureIdParts;
    if (auto const idPrefix = getIdPrefix()) {
        fullFeatureIdParts.reserve(idPrefix->size() + featureIdParts.size());
        prefixFeatureIdParts.reserve(idPrefix->size());
        for (auto const& [key, value] : idPrefix->fields()) {
            auto const keyStr = strings()->resolve(key);
            if (!keyStr || !value) {
                continue;
            }

            std::visit(
                [&](auto&& v)
                {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate> ||
                                  std::is_same_v<T, double> ||
                                  std::is_same_v<T, simfil::ByteArray>) {
                    }
                    else if constexpr (std::is_same_v<T, std::string_view> ||
                                       std::is_same_v<T, std::string>) {
                        fullFeatureIdParts.emplace_back(*keyStr, std::string_view(v));
                        prefixFeatureIdParts.emplace_back(*keyStr, std::string_view(v));
                    }
                    else {
                        fullFeatureIdParts.emplace_back(*keyStr, static_cast<int64_t>(v));
                        prefixFeatureIdParts.emplace_back(*keyStr, static_cast<int64_t>(v));
                    }
                },
                value->value());
        }
    }
    // Validation runs against the full logical feature id, even though only the
    // non-prefix suffix is materialized into the compact feature storage.
    fullFeatureIdParts.insert(fullFeatureIdParts.end(), featureIdParts.begin(), featureIdParts.end());

    if (!layerInfo_->validFeatureId(typeId, fullFeatureIdParts, true)) {
        raise(fmt::format(
            "Could not find a matching ID composition of type {} with parts {}.",
            typeId,
            idPartsToString(fullFeatureIdParts)));
    }
    auto const& primaryIdComposition = getPrimaryIdComposition(typeId);
    // Stored feature ids omit the common tile prefix to save space, so we first
    // determine where the feature-local suffix starts within the composition.
    auto const localStartIndex = prefixFeatureIdParts.empty()
        ? 0U
        : *IdPart::compositionMatchEndIndex(
            primaryIdComposition,
            0,
            prefixFeatureIdParts,
            prefixFeatureIdParts.size());
    auto idPartValues = idPartValuesToArrayIndex(
        *this,
        primaryIdComposition,
        featureIdParts,
        localStartIndex);
    auto res = strings()->emplace(typeId);
    if (!res)
        raise(res.error().message);

    // Initial backend LOD strategy:
    // - stage 0 ("Low-Fi"): default to LOD_0 (no random culling); converters can
    //   override per-feature LOD semantically (e.g. road classes).
    // - other stages: default to MAX_LOD. During stage merge, stage-0 feature data
    //   remains authoritative for LOD.
    auto lodValue = static_cast<uint8_t>(Feature::MAX_LOD);
    if (stage_ && *stage_ == 0) {
        lodValue = static_cast<uint8_t>(Feature::LOD::LOD_0);
    }

    auto featureIndex = impl_->features_.size();
    impl_->features_.emplace_back(Feature::BasicData{
        Feature::TypeIdAndLOD{
            *res,
            lodValue},
        idPartValues,
        ModelNodeAddress{Null, 0},
    });
    auto result = Feature(
        impl_->features_.back(),
        nullptr,
        shared_from_this(),
        ModelNodeAddress{ColumnId::Features, (uint32_t)featureIndex},
        mpKey_);

    if (!expectedFeatureIds_.empty()) {
        if (featureIndex >= expectedFeatureIds_.size()) {
            raiseFmt(
                "Feature sequence mismatch: unexpected extra feature at index {}: {}.",
                featureIndex,
                result.id()->toString());
        }

        auto const& expectedFeatureId = expectedFeatureIds_[featureIndex];
        auto const actualFeatureId = result.id()->toString();
        if (actualFeatureId != expectedFeatureId) {
            // Overlay validation is sequence-based on purpose so stage imports
            // fail immediately when converters reorder or drop features.
            raiseFmt(
                "Feature sequence mismatch at index {}: expected {}, got {}.",
                featureIndex,
                expectedFeatureId,
                actualFeatureId);
        }
    }

    // Add feature hash index entry.
    auto fullStrippedFeatureId = stripOptionalIdParts(result.id()->keyValuePairs(), primaryIdComposition);
    auto hash = static_cast<uint32_t>(Hash().mix(typeId).mix(fullStrippedFeatureId).value());
    impl_->featureHashIndex_.emplace_back(FeatureAddrWithIdHash{result.addr(), hash});
    impl_->featureHashIndexNeedsSorting_ = true;

    // Note: Here we rely on the assertion that the root_ collection
    // contains only references to feature nodes, in the order
    // of the feature node column.
    addRoot(ModelNode::Ptr(result));
    setInfo("Size/Features#features", numRoots());
    return result;
}

model_ptr<FeatureId>
TileFeatureLayer::newFeatureId(
    const std::string_view& typeId,
    const KeyValueViewPairs& featureIdParts,
    std::optional<std::string_view> externalMapId)
{
    if (!layerInfo_->validFeatureId(typeId, featureIdParts, false)) {
        raise(fmt::format(
            "Could not find a matching ID composition of type {} with parts {}.",
            typeId,
            idPartsToString(featureIdParts)));
    }

    auto featureIdIndex = featureIds_.size();
    auto typeIdStringId = strings()->emplace(typeId);
    if (!typeIdStringId)
        raise(typeIdStringId.error().message);
    auto const idCompositionIndex =
        *layerInfo_->matchingFeatureIdCompositionIndex(typeId, featureIdParts, false);
    auto const& composition =
        layerInfo_->getTypeInfo(typeId)->uniqueIdCompositions_[idCompositionIndex];
    simfil::StringId externalMapIdStringId = simfil::StringPool::Empty;
    if (externalMapId && *externalMapId != mapId()) {
        auto storedMapId = strings()->emplace(*externalMapId);
        if (!storedMapId) {
            raise(storedMapId.error().message);
        }
        // Local references omit the redundant map payload so legacy JSON stays compact.
        externalMapIdStringId = *storedMapId;
    }
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
        {ColumnId::ExternalFeatureIds, static_cast<uint32_t>(featureIdIndex)},
        mpKey_);
}

model_ptr<Relation>
TileFeatureLayer::newRelation(const std::string_view& name, const model_ptr<FeatureId>& target)
{
    auto relationIndex = impl_->relations_.size();
    auto nameStringId = strings()->emplace(name);
    if (!nameStringId)
        raise(nameStringId.error().message);
    impl_->relations_.emplace_back(Relation::Data{
        .name_ = *nameStringId,
        .targetFeatureId_ = target->addr()
    });
    return Relation(
        &impl_->relations_.back(),
        shared_from_this(),
        {ColumnId::Relations, (uint32_t)relationIndex},
        mpKey_);
}

model_ptr<RelationReference>
TileFeatureLayer::newRelationReference(model_ptr<Relation> const& relation)
{
    if (!relation) {
        raise("Cannot create RelationReference for null relation.");
    }
    if (relation->addr().column() != ColumnId::Relations) {
        raise("RelationReference target must be a canonical Relation node.");
    }
    if (relation->owningModel().get() != this) {
        raise("RelationReference target must belong to this TileFeatureLayer.");
    }
    if (relation->addr().index() >= impl_->relations_.size()) {
        raise("RelationReference target index is out of range.");
    }
    return RelationReference(
        shared_from_this(),
        {ColumnId::RelationReferences, relation->addr().index()},
        mpKey_);
}

model_ptr<Object> TileFeatureLayer::getIdPrefix()
{
    return static_cast<TileFeatureLayer const&>(*this).getIdPrefix();
}

model_ptr<Object> TileFeatureLayer::getIdPrefix() const
{
    if (impl_->featureIdPrefix_)
        return resolve<simfil::Object>(impl_->featureIdPrefix_);
    return {};
}

model_ptr<Attribute>
TileFeatureLayer::newAttribute(
    const std::string_view& name,
    size_t initialCapacity,
    bool fixedSize)
{
    auto attrIndex = impl_->attributes_.size();
    auto nameStringId = strings()->emplace(name);
    if (!nameStringId)
        raise(nameStringId.error().message);
    impl_->attributes_.emplace_back(Attribute::Data{
        {Null, 0},
        objectMemberStorage().new_array(initialCapacity, fixedSize),
        *nameStringId,
    });
    return Attribute(
        &impl_->attributes_.back(),
        shared_from_this(),
        {ColumnId::Attributes, (uint32_t)attrIndex},
        mpKey_);
}

model_ptr<AttributeLayer> TileFeatureLayer::newAttributeLayer(size_t initialCapacity, bool fixedSize)
{
    auto layerIndex = impl_->attrLayers_.size();
    impl_->attrLayers_.emplace_back(objectMemberStorage().new_array(initialCapacity, fixedSize));
    return AttributeLayer(
        impl_->attrLayers_.back(),
        shared_from_this(),
        {ColumnId::AttributeLayers, (uint32_t)layerIndex},
        mpKey_);
}

model_ptr<AttributeLayerList> TileFeatureLayer::newAttributeLayers(size_t initialCapacity, bool fixedSize)
{
    auto listIndex = impl_->attrLayerLists_.size();
    impl_->attrLayerLists_.emplace_back(objectMemberStorage().new_array(initialCapacity, fixedSize));
    return AttributeLayerList(
        impl_->attrLayerLists_.back(),
        shared_from_this(),
        {ColumnId::AttributeLayerLists, (uint32_t)listIndex},
        mpKey_);
}

model_ptr<GeometryCollection> TileFeatureLayer::newGeometryCollection(size_t initialCapacity, bool fixedSize)
{
    auto listIndex = arrayMemberStorage().new_array(initialCapacity, fixedSize);
    return GeometryCollection(
        shared_from_this(),
        {ColumnId::GeometryCollections, (uint32_t)listIndex},
        mpKey_);
}

model_ptr<Geometry> TileFeatureLayer::newGeometry(
    GeomType geomType,
    size_t initialCapacity,
    bool fixedSize)
{
    initialCapacity = std::max<size_t>(1, initialCapacity);

    auto const currentGeometryStage = [this]() -> std::optional<uint8_t>
    {
        auto stage = stage_.value_or(layerInfo_ ? layerInfo_->highFidelityStage_ : 0U);
        if (stage > std::numeric_limits<uint8_t>::max()) {
            raiseFmt("Geometry stage {} exceeds uint8_t range.", stage);
        }
        return static_cast<uint8_t>(stage);
    }();

    auto makeGeometry =
        [this, currentGeometryStage](uint8_t column, simfil::ArrayIndex vertexArray)
    {
        auto const geometryAddress =
            simfil::ModelNodeAddress{column, static_cast<uint32_t>(vertexArray)};
        setGeometryStage(geometryAddress, currentGeometryStage);
        return Geometry(shared_from_this(), geometryAddress, mpKey_);
    };

    switch (geomType) {
    case GeomType::Points: {
        auto const vertexArray = pointBuffers_.new_array(initialCapacity, fixedSize);
        return makeGeometry(ColumnId::PointGeometries, vertexArray);
    }
    case GeomType::Line:
    {
        auto const vertexArray = pointBuffers_.new_array(initialCapacity, fixedSize);
        return makeGeometry(ColumnId::LineGeometries, vertexArray);
    }
    case GeomType::Polygon:
    {
        auto const vertexArray = pointBuffers_.new_array(initialCapacity, fixedSize);
        return makeGeometry(ColumnId::PolygonGeometries, vertexArray);
    }
    case GeomType::Mesh:
    {
        auto const vertexArray = pointBuffers_.new_array(initialCapacity, fixedSize);
        return makeGeometry(ColumnId::MeshGeometries, vertexArray);
    }
    case GeomType::AABB:
    {
        auto const vertexArray = pointBuffers_.new_array(2, true);
        return makeGeometry(ColumnId::AabbGeometries, vertexArray);
    }
    case GeomType::GltfNodeIndex:
    {
        auto const vertexArray = pointBuffers_.new_array(3, true);
        return makeGeometry(ColumnId::GltfNodeIndexGeometries, vertexArray);
    }
    }

    raise("Unsupported geometry type.");
    return {};
}

model_ptr<Geometry> TileFeatureLayer::newGeometryView(
    GeomType geomType,
    uint32_t offset,
    uint32_t size,
    const model_ptr<Geometry>& base)
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
        {ColumnId::GeometryViews, (uint32_t)geomViews_.size() - 1},
        mpKey_);
}

model_ptr<SourceDataReferenceCollection> TileFeatureLayer::newSourceDataReferenceCollection(std::span<QualifiedSourceDataReference> list)
{
    auto& arena = sourceDataReferences_;
    const auto index = arena.size();
    const auto size = list.size();

    arena.insert(arena.end(), list.begin(), list.end());

    return {
    SourceDataReferenceCollection(index, size, shared_from_this(),
        ModelNodeAddress(ColumnId::SourceDataReferenceCollections, sourceDataAddressListToModelAddress(index, size)),
        mpKey_)};
}

model_ptr<Validity> TileFeatureLayer::newValidity()
{
    impl_->validities_.emplace_back();
    return Validity(
        &impl_->validities_.back(),
        shared_from_this(),
        {ColumnId::Validities, (uint32_t)impl_->validities_.size() - 1},
        mpKey_);
}

model_ptr<MultiValidity> TileFeatureLayer::newValidityCollection(size_t initialCapacity, bool fixedSize)
{
    auto validityArrId = arrayMemberStorage().new_array(initialCapacity, fixedSize);
    return MultiValidity(
        shared_from_this(),
        {ColumnId::ValidityCollections, (uint32_t)validityArrId},
        mpKey_);
}

ModelNodeAddress TileFeatureLayer::materializeSimpleValidity(
    ModelNodeAddress simpleAddress,
    simfil::ArrayIndex ownerMembers,
    uint32_t ownerElementIndex,
    Validity::Direction direction)
{
    if (simpleAddress.column() != ColumnId::SimpleValidity) {
        raise("Cannot materialize non-simple validity node.");
    }
    auto memberAddress = arrayMemberStorage().at(ownerMembers, ownerElementIndex);
    if (!memberAddress) {
        raise("Simple validity owner slot could not be resolved.");
    }
    if (memberAddress->get().value_ != simpleAddress.value_) {
        raise("Simple validity owner slot no longer points at the expected simple validity.");
    }

    impl_->validities_.emplace_back();
    auto upgradedAddress = ModelNodeAddress{
        ColumnId::Validities,
        static_cast<uint32_t>(impl_->validities_.size() - 1)};
    auto& upgraded = impl_->validities_.back();
    upgraded.direction_ = direction;
    memberAddress->get() = upgradedAddress;
    return upgradedAddress;
}

// Short aliases to keep resolve hook signatures compact.
using simfil::ModelNode;
using simfil::res::tag;

template<>
model_ptr<AttributeLayer> resolveInternal(tag<AttributeLayer>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::AttributeLayers)
        raise("Cannot cast this node to an AttributeLayer.");
    return AttributeLayer(
        model.impl_->attrLayers_[node.addr().index()],
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<AttributeLayerList> resolveInternal(tag<AttributeLayerList>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::AttributeLayerLists &&
        node.addr().column() != TileFeatureLayer::ColumnId::FeatureAttributeLayerListView)
        raise("Cannot cast this node to an AttributeLayerList.");
    return AttributeLayerList(
        node.addr().column() == TileFeatureLayer::ColumnId::AttributeLayerLists
            ? model.impl_->attrLayerLists_[node.addr().index()]
            : simfil::InvalidArrayIndex,
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<Attribute> resolveInternal(tag<Attribute>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::Attributes)
        raise("Cannot cast this node to an Attribute.");
    return Attribute(
        &model.impl_->attributes_[node.addr().index()],
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<Feature> resolveInternal(tag<Feature>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::Features) {
        raise("Cannot cast this node to a Feature.");
    }
    auto* complexData =
        const_cast<TileFeatureLayer&>(model).featureComplexDataOrNull(node.addr().index());
    model_ptr<Feature> result = Feature(
        model.impl_->features_[node.addr().index()],
        complexData,
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);

    auto overlay = model.overlay();
    if (overlay && node.addr().index() < overlay->size()) {
        result->setExtensionAddress(
            overlay.get(),
            ModelNodeAddress{
                TileFeatureLayer::ColumnId::Features,
                static_cast<uint32_t>(node.addr().index())});
    }
    return result;
}

template<>
model_ptr<RelationArrayView> resolveInternal(tag<RelationArrayView>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::FeatureRelationsView)
        raise("Cannot cast this node to a RelationArrayView.");

    auto result = RelationArrayView(
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
    return result;
}

model_ptr<FeatureId> TileFeatureLayer::resolveFeatureIdNode(ModelNode const& node) const
{
    switch (node.addr().column()) {
    case TileFeatureLayer::ColumnId::FeatureIds: {
        auto const featureIndex = node.addr().index();
        if (featureIndex >= impl_->features_.size()) {
            raiseFmt(
                "Cannot cast FeatureIds node {} to a FeatureId: feature index out of range ({} features).",
                featureIndex,
                impl_->features_.size());
        }
        auto const& featureData = impl_->features_.at(featureIndex);
        return FeatureId(
            FeatureId::Data{
                true,
                0,
                featureData.typeIdAndLod_.typeId_,
                featureData.idPartValues_,
                simfil::StringPool::Empty},
            shared_from_this(),
            node.addr(),
            mpKey_);
    }
    case TileFeatureLayer::ColumnId::ExternalFeatureIds: {
        auto const featureIdIndex = node.addr().index();
        if (featureIdIndex >= featureIds_.size()) {
            raiseFmt(
                "Cannot cast ExternalFeatureIds node {} to a FeatureId: external feature-id index out of range ({} feature ids).",
                featureIdIndex,
                featureIds_.size());
        }
        return FeatureId(
            featureIds_.at(featureIdIndex),
            shared_from_this(),
            node.addr(),
            mpKey_);
    }
    default:
        raise("Cannot cast this node to a FeatureId.");
    }
}

template<>
model_ptr<Relation> resolveInternal(tag<Relation>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::Relations)
        raise("Cannot cast this node to a Relation.");
    if (node.addr().index() >= model.impl_->relations_.size())
        raise("Relation index is out of range.");
    return Relation(
        &model.impl_->relations_[node.addr().index()],
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<RelationReference> resolveInternal(tag<RelationReference>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::RelationReferences)
        raise("Cannot cast this node to a RelationReference.");
    if (node.addr().index() >= model.impl_->relations_.size())
        raise("RelationReference target index is out of range.");
    return RelationReference(
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

model_ptr<PointNode> TileFeatureLayer::resolvePointNode(ModelNode const& node) const
{
    switch (node.addr().column()) {
    case TileFeatureLayer::ColumnId::Points:
        return PointNode(
            node,
            static_cast<simfil::ArrayIndex>(node.addr().index()),
            mpKey_);
    case TileFeatureLayer::ColumnId::ValidityPoints:
        return PointNode(node, &impl_->validities_.at(node.addr().index()), mpKey_);
    case TileFeatureLayer::ColumnId::GeometryPointView:
        return PointNode(node, mpKey_);
    default:
        raise("Cannot cast this node to a Point.");
    }
}

template<>
model_ptr<Validity> resolveInternal(tag<Validity>, TileFeatureLayer const& model, ModelNode const& node)
{
    switch (node.addr().column()) {
    case TileFeatureLayer::ColumnId::Validities: {
        auto const validityIndex = node.addr().index();
        if (validityIndex >= model.impl_->validities_.size()) {
            raiseFmt(
                "Cannot cast Validities node {} to a Validity: validity index out of range ({} validities).",
                validityIndex,
                model.impl_->validities_.size());
        }
        return Validity(
            &model.impl_->validities_.at(validityIndex),
            model.shared_from_this(),
            node.addr(),
            model.mpKey_);
    }
    case TileFeatureLayer::ColumnId::SimpleValidity: {
        auto const direction = static_cast<Validity::Direction>(node.addr().index());
        if (direction < Validity::Empty || direction > Validity::None) {
            raiseFmt(
                "Cannot cast this node to a Validity: invalid simple validity direction value {}.",
                node.addr().index());
        }
        return Validity(
            direction,
            model.shared_from_this(),
            node.addr(),
            node.runtimeData(),
            model.mpKey_);
    }
    default:
        raise("Cannot cast this node to a Validity.");
    }
}

tl::expected<void, simfil::Error> TileFeatureLayer::resolve(const ModelNode& n, const simfil::Model::ResolveFn& cb) const
{
    // Merged views may return nodes copied from overlay tiles. Resolve those
    // through their owner instead of interpreting overlay addresses locally.
    if (auto owner = n.owningModel(); owner && owner.get() != this) {
        return owner->resolve(n, cb);
    }

    switch (n.addr().column())
    {
    case ColumnId::Features:
        cb(*resolve<Feature>(n));
        return {};
    case ColumnId::FeatureProperties:
    {
        auto rootResult = root(n.addr().index());
        if (!rootResult || !*rootResult) {
            raise("FeatureProperties index out of bounds.");
        }
        cb(Feature::FeaturePropertyView(resolve<Feature>(**rootResult), mpKey_));
        return {};
    }
    case ColumnId::FeatureRelationsView:
        cb(*resolve<RelationArrayView>(n));
        return {};
    case ColumnId::FeatureGeometryCollectionView:
        cb(*resolve<GeometryCollection>(n));
        return {};
    case ColumnId::FeatureGeometryArrayView:
        cb(*resolve<GeometryArrayView>(n));
        return {};
    case ColumnId::FeatureAttributeLayerListView:
        cb(*resolve<AttributeLayerList>(n));
        return {};
    case ColumnId::FeatureIds:
    case ColumnId::ExternalFeatureIds:
        cb(*resolve<FeatureId>(n));
        return {};
    case ColumnId::Attributes:
        cb(*resolve<Attribute>(n));
        return {};
    case ColumnId::AttributeLayers:
        cb(*resolve<AttributeLayer>(n));
        return {};
    case ColumnId::AttributeLayerLists:
        cb(*resolve<AttributeLayerList>(n));
        return {};
    case ColumnId::Relations:
        cb(*resolve<Relation>(n));
        return {};
    case ColumnId::RelationReferences:
        cb(*resolve<RelationReference>(n));
        return {};
    case ColumnId::Points:
        cb(*resolve<PointNode>(n));
        return {};
    case ColumnId::PointBuffers:
    case ColumnId::PointBuffersView:
        cb(*resolve<PointBufferNode>(n));
        return {};
    case ColumnId::GeometryPointView:
        cb(*resolve<PointNode>(n));
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
        cb(*resolve<LinearRingNode>(n));
        return {};
    case ColumnId::LinearRing:
        cb(*resolve<LinearRingNode>(n));
        return {};
    case ColumnId::SourceDataReferenceCollections:
        cb(*resolve<SourceDataReferenceCollection>(n));
        return {};
    case ColumnId::SourceDataReferences:
        cb(*resolve<SourceDataReferenceItem>(n));
        return {};
    case ColumnId::Validities:
    case ColumnId::SimpleValidity:
        cb(*resolve<Validity>(n));
        return {};
    case ColumnId::ValidityPoints:
        cb(*resolve<PointNode>(n));
        return {};
    case ColumnId::ValidityCollections:
        cb(*resolve<MultiValidity>(n));
        return {};
    }

    return ModelPool::resolve(n, cb);
}

tl::expected<TileFeatureLayer::QueryResult, simfil::Error>
TileFeatureLayer::evaluate(std::string_view query, ModelNode const& node, bool anyMode, bool autoWildcard)
{
    return impl_->expressionCache_.eval(query, node, anyMode, autoWildcard);
}

tl::expected<TileFeatureLayer::QueryResult, simfil::Error>
TileFeatureLayer::evaluate(std::string_view query, bool anyMode, bool autoWildcard)
{
    auto rootResult = root(0);
    if (!rootResult) {
        return tl::unexpected(rootResult.error());
    }
    return evaluate(query, **rootResult, anyMode, autoWildcard);
}

tl::expected<std::vector<simfil::Diagnostics::Message>, simfil::Error>
TileFeatureLayer::collectQueryDiagnostics(std::string_view query, const simfil::Diagnostics& diag, bool anyMode)
{
    auto rootResult = root(0);
    auto rootSchema = rootResult && *rootResult
        ? (*rootResult)->schema()
        : simfil::NoSchemaId;
    return impl_->expressionCache_.diagnostics(query, diag, anyMode, rootSchema);
}

tl::expected<std::vector<simfil::CompletionCandidate>, simfil::Error>
TileFeatureLayer::complete(std::string_view query, int point, ModelNode const& node, simfil::CompletionOptions const& opts)
{
    auto completionStrings = std::make_shared<simfil::StringPool>(*strings());
    auto completionEnv = Impl::makeSchemaAwareCompletionEnvironment(std::move(completionStrings), impl_->layerSchema_);
    return simfil::complete(*completionEnv, query, point, node, opts);
}

void TileFeatureLayer::setIdPrefix(const KeyValueViewPairs& prefix)
{
    // The prefix must be set, before any feature is added.
    if (numRoots() > 0)
        throw std::runtime_error("Cannot set feature id prefix after a feature was added.");

    // Check that the prefix is compatible with all primary id composites.
    // The primary id composition is the first one in the list.
    for (auto& featureType : this->layerInfo_->featureTypes_) {
        for (auto& candidateComposition : featureType.uniqueIdCompositions_) {
            std::string error;
            auto compositionMatched = IdPart::idPartsMatchComposition(
                candidateComposition,
                0,
                prefix,
                prefix.size(),
                false,
                &error);
            if (!compositionMatched) {
                raise(fmt::format(
                    "Tile feature ID prefix is not compatible with an id composite in type {}: {}",
                    featureType.name_,
                    error));
            }
            break;
        }
    }

    auto idPrefix = newObject(prefix.size(), true);
    for (auto const& [k, v] : prefix) {
        auto&& kk = k;
        std::visit([&](auto&& x){
            idPrefix->addField(kk, x);
        }, v);
    }
    impl_->featureIdPrefix_ = idPrefix->addr();
}

TileFeatureLayer::Iterator TileFeatureLayer::begin() const
{
    return TileFeatureLayer::Iterator{*this, 0};
}

TileFeatureLayer::Iterator TileFeatureLayer::end() const
{
    return TileFeatureLayer::Iterator{*this, size()};
}

tl::expected<void, simfil::Error> TileFeatureLayer::write(std::ostream& outputStream)
{
    TileLayer::write(outputStream);
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);
    s.ext4b(stage_, bitsery::ext::StdOptional{});
    impl_->readWrite(s);
    readWriteCommonColumns(s);
    return ModelPool::write(outputStream);
}

nlohmann::json TileFeatureLayer::toJson() const
{
    auto result = nlohmann::json::object();

    result["type"] = "FeatureCollection";
    result["mapgetTileId"] = tileId_.value();
    result["mapId"] = mapId_;
    result["mapgetLayerId"] = layerInfo_->layerId_;
    result["geometryAnchor"] = {
        impl_->geometryAnchor_.x,
        impl_->geometryAnchor_.y,
        impl_->geometryAnchor_.z};

    if (impl_->glbAttachment_) {
        result["glbAttachment"] = impl_->glbAttachment_->toJsonMetadata();
    }

    // Preserve the binary timestamp representation exactly in strict JSON.
    result["timestamp"] = std::chrono::duration_cast<std::chrono::microseconds>(
        timestamp_.time_since_epoch()).count();

    // Add TTL if set (in milliseconds)
    if (ttl_)
        result["ttl"] = ttl_->count();

    // Add error information if present
    if (error_ || errorCode_) {
        auto errorObj = nlohmann::json::object();
        if (errorCode_)
            errorObj["code"] = *errorCode_;
        if (error_)
            errorObj["message"] = *error_;
        result["error"] = errorObj;
    }

    // Add features
    auto features = nlohmann::json::array();
    for (auto f : *this)
        features.push_back(f->toJson());
    result["features"] = features;

    return result;
}

void TileFeatureLayer::validateSchema() const
{
    if (!layerInfo_ || !layerInfo_->featureModelSchema_) {
        raise("TileFeatureLayer::validateSchema: layer has no featureModelSchema.");
    }

    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(layerInfo_->featureModelSchema_->toJsonSchema());
    for (auto const& feature : *this) {
        validator.validate(feature->toJson());
    }
}

std::shared_ptr<LayerSchema const> TileFeatureLayer::layerSchema() const
{
    return impl_->layerSchema_;
}

LayerSchema::Entry const* TileFeatureLayer::getSchema(std::string_view typeName) const
{
    return impl_->layerSchema_ ? impl_->layerSchema_->getSchema(typeName) : nullptr;
}

simfil::SchemaId TileFeatureLayer::featureSchemaId(std::string_view featureType) const
{
    return impl_->layerSchema_ ? impl_->layerSchema_->featureSchema(featureType) : simfil::NoSchemaId;
}

simfil::SchemaId TileFeatureLayer::featurePropertiesSchemaId(std::string_view featureType) const
{
    return impl_->layerSchema_ ? impl_->layerSchema_->featurePropertiesSchema(featureType) : simfil::NoSchemaId;
}

simfil::SchemaId TileFeatureLayer::attributeLayerMapSchemaId(std::string_view featureType) const
{
    return impl_->layerSchema_ ? impl_->layerSchema_->attributeLayerMapSchema(featureType) : simfil::NoSchemaId;
}

simfil::SchemaId TileFeatureLayer::schemaIdForKey(std::string_view key) const
{
    return impl_->layerSchema_ ? impl_->layerSchema_->schemaId(key) : simfil::NoSchemaId;
}

simfil::SchemaId TileFeatureLayer::childSchemaId(
    simfil::SchemaId parent,
    std::string_view fieldName,
    std::optional<simfil::Schema::Kind> preferredKind) const
{
    return impl_->layerSchema_
        ? impl_->layerSchema_->childSchema(parent, fieldName, preferredKind)
        : simfil::NoSchemaId;
}

simfil::SchemaId TileFeatureLayer::childSchemaId(
    simfil::SchemaId parent,
    simfil::StringId field,
    std::optional<simfil::Schema::Kind> preferredKind) const
{
    if (!impl_->layerSchema_) {
        return simfil::NoSchemaId;
    }
    auto fieldName = strings()->resolve(field);
    return fieldName
        ? impl_->layerSchema_->childSchema(parent, *fieldName, preferredKind)
        : simfil::NoSchemaId;
}

void TileFeatureLayer::applyObjectSchema(simfil::Object& object, simfil::SchemaId schemaId) const
{
    if (schemaId == simfil::NoSchemaId) {
        return;
    }
    auto const current = object.schema();
    auto const target = current == simfil::NoSchemaId || current == schemaId
        ? schemaId
        : simfil::NoSchemaId;
    if (auto result = object.setSchema(target); !result) {
        log().warn("Failed to set object schema: {}", result.error().message);
    }
}

void TileFeatureLayer::applyArraySchema(simfil::Array& array, simfil::SchemaId schemaId) const
{
    if (schemaId == simfil::NoSchemaId) {
        return;
    }
    auto const current = array.schema();
    auto const target = current == simfil::NoSchemaId || current == schemaId
        ? schemaId
        : simfil::NoSchemaId;
    if (auto result = array.setSchema(target); !result) {
        log().warn("Failed to set array schema: {}", result.error().message);
    }
}

nlohmann::json TileFeatureLayer::serializationSizeStats() const
{
    auto featureLayer = nlohmann::json::object();

    featureLayer["features"] = impl_->features_.byte_size();
    featureLayer["feature-complex-data"] = impl_->complexFeatureData_.byte_size();
    featureLayer["feature-complex-data-refs"] = impl_->complexFeatureDataRefs_.byte_size();
    featureLayer["attributes"] = impl_->attributes_.byte_size();
    featureLayer["validities"] = impl_->validities_.byte_size();
    featureLayer["feature-ids"] = featureIds_.byte_size();
    featureLayer["attribute-layers"] = impl_->attrLayers_.byte_size();
    featureLayer["attribute-layer-lists"] = impl_->attrLayerLists_.byte_size();
    featureLayer["relations"] = impl_->relations_.byte_size();
    featureLayer["feature-hash-index"] = impl_->featureHashIndex_.byte_size();
    featureLayer["point-geometries"] = 0;
    featureLayer["geometries"] = geomViews_.byte_size();
    featureLayer["geometry-source-data-references"] = geomSourceDataRefs_.byte_size();
    featureLayer["geometry-stages"] = geomStages_.byte_size();
    featureLayer["geometry-views"] = geomViews_.byte_size();
    featureLayer["polygon-ring-start-refs"] = polygonRingStartRefs_.byte_size();
    featureLayer["polygon-ring-starts"] = polygonRingStarts_.byte_size();
    featureLayer["point-buffers"] = pointBuffers_.byte_size();
    featureLayer["source-data-references"] = sourceDataReferences_.byte_size();
    featureLayer["glb-attachment-present"] = impl_->glbAttachment_.has_value();
    featureLayer["glb-attachment-name"] =
        impl_->glbAttachment_ ? impl_->glbAttachment_->name_.size() : 0;
    featureLayer["glb-attachment-payload"] =
        impl_->glbAttachment_ ? impl_->glbAttachment_->bytes_.size() : 0;

    auto singletonStatsToJson = [](auto const& stats) {
        return nlohmann::json::object({
            {"handles", stats.handleCount},
            {"occupied", stats.occupiedCount},
            {"empty", stats.emptyCount},
            {"singleton-storage-bytes", stats.singletonStorageBytes},
            {"hypothetical-regular-bytes", stats.hypotheticalRegularBytes},
            {"estimated-saved-bytes", stats.estimatedSavedBytes},
        });
    };

    auto pointBufferSingletonStats = pointBuffers_.singleton_stats();
    auto objectMemberSingletonStats = objectMemberStorage().singleton_stats();
    auto arrayMemberSingletonStats = arrayMemberStorage().singleton_stats();
    auto arrayArenaSingletons = nlohmann::json::object({
        {"point-buffers", singletonStatsToJson(pointBufferSingletonStats)},
        {"object-members", singletonStatsToJson(objectMemberSingletonStats)},
        {"array-members", singletonStatsToJson(arrayMemberSingletonStats)},
    });

    auto geometryUsage = nlohmann::json::object({
        {"total", 0},
        {"base", 0},
        {"view", 0},
        {"with-source-data-references", 0},
        {"base-vertex-buffer-allocated", 0},
        {"by-type", nlohmann::json::object({
            {"points", 0},
            {"line", 0},
            {"polygon", 0},
            {"mesh", 0},
            {"aabb", 0},
            {"gltf-node-index", 0},
        })},
        {"base-by-type", nlohmann::json::object({
            {"points", 0},
            {"line", 0},
            {"polygon", 0},
            {"mesh", 0},
            {"aabb", 0},
            {"gltf-node-index", 0},
        })},
        {"view-by-type", nlohmann::json::object({
            {"points", 0},
            {"line", 0},
            {"polygon", 0},
            {"mesh", 0},
            {"aabb", 0},
            {"gltf-node-index", 0},
        })},
    });

    auto validityUsage = nlohmann::json::object({
        {"total", 0},
        {"simple-column", 0},
        {"direction-only", 0},
        {"with-direction", 0},
        {"with-geometry-stage", 0},
        {"with-feature-id", 0},
        {"simple-geometry-with-address", 0},
        {"by-direction", nlohmann::json::object({
            {"empty", 0},
            {"positive", 0},
            {"negative", 0},
            {"both", 0},
            {"none", 0},
        })},
        {"by-geometry-description", nlohmann::json::object({
            {"none", 0},
            {"simple-geometry", 0},
            {"offset-point", 0},
            {"offset-range", 0},
        })},
        {"by-offset-type", nlohmann::json::object({
            {"invalid", 0},
            {"geo-pos", 0},
            {"buffer", 0},
            {"relative-length", 0},
            {"metric-length", 0},
        })},
    });

    auto featureLodUsage = nlohmann::json::object();
    for (uint32_t lod = 0; lod <= static_cast<uint32_t>(Feature::MAX_LOD); ++lod) {
        featureLodUsage[fmt::format("lod_{}", lod)] = 0;
    }

    auto increment = [](nlohmann::json& obj, std::string_view key) {
        obj[std::string(key)] = obj[std::string(key)].get<int64_t>() + 1;
    };

    auto geometryTypeKey = [](GeomType type) -> const char* {
        switch (type) {
        case GeomType::Points: return "points";
        case GeomType::Line: return "line";
        case GeomType::Polygon: return "polygon";
        case GeomType::Mesh: return "mesh";
        case GeomType::AABB: return "aabb";
        case GeomType::GltfNodeIndex: return "gltf-node-index";
        }
        return "points";
    };

    std::unordered_set<uint32_t> countedGeometryAddresses;
    auto countGeometryAddress = [&](auto&& self, simfil::ModelNodeAddress geometryAddress) -> void
    {
        if (!geometryAddress) {
            return;
        }

        if (geometryAddress.column() == ColumnId::GeometryCollections) {
            auto collection = resolve<GeometryCollection>(geometryAddress);
            if (!collection) {
                return;
            }
            collection->forEachGeometry(
                [&](auto&& geometry)
                {
                    self(self, geometry->addr());
                    return true;
                });
            return;
        }

        if (!isBaseGeometryColumn(geometryAddress.column()) &&
            geometryAddress.column() != ColumnId::GeometryViews) {
            return;
        }

        if (!countedGeometryAddresses.insert(geometryAddress.value_).second) {
            return;
        }

        auto geometryType = geometryAddress.column() == ColumnId::GeometryViews
            ? geomViews_.at(geometryAddress.index()).type_
            : geometryTypeForColumn(geometryAddress.column());
        auto typeKey = geometryTypeKey(geometryType);

        increment(geometryUsage, "total");
        increment(geometryUsage["by-type"], typeKey);
        if (geometrySourceDataReferences(geometryAddress)) {
            increment(geometryUsage, "with-source-data-references");
        }

        if (geometryAddress.column() == ColumnId::GeometryViews) {
            increment(geometryUsage, "view");
            increment(geometryUsage["view-by-type"], typeKey);
            return;
        }

        increment(geometryUsage, "base");
        increment(geometryUsage["base-by-type"], typeKey);
        increment(geometryUsage, "base-vertex-buffer-allocated");
    };

    for (auto const& feature : *this) {
        if (auto geometry = feature->geomOrNull()) {
            geometry->forEachGeometry(
                [&](auto&& geometryEntry)
                {
                    countGeometryAddress(countGeometryAddress, geometryEntry->addr());
                    return true;
                });
        }
    }

    for (auto const& featureData : impl_->features_) {
        auto const lod = std::min<uint8_t>(
            featureData.typeIdAndLod_.lod_,
            static_cast<uint8_t>(Feature::MAX_LOD));
        increment(featureLodUsage, fmt::format("lod_{}", lod));
    }

    for (auto const& validity : impl_->validities_) {
        increment(validityUsage, "total");

        switch (validity.direction_) {
        case Validity::Empty: increment(validityUsage["by-direction"], "empty"); break;
        case Validity::Positive: increment(validityUsage["by-direction"], "positive"); break;
        case Validity::Negative: increment(validityUsage["by-direction"], "negative"); break;
        case Validity::Both: increment(validityUsage["by-direction"], "both"); break;
        case Validity::None: increment(validityUsage["by-direction"], "none"); break;
        }

        if (validity.direction_ != Validity::Empty) {
            increment(validityUsage, "with-direction");
        }
        if (validity.referencedStage_ != ValidityData::InvalidReferencedStage) {
            increment(validityUsage, "with-geometry-stage");
        }
        if (validity.featureAddress_) {
            increment(validityUsage, "with-feature-id");
        }

        switch (validity.geomDescrType_) {
        case Validity::NoGeometry:
            increment(validityUsage["by-geometry-description"], "none");
            break;
        case Validity::SimpleGeometry:
            increment(validityUsage["by-geometry-description"], "simple-geometry");
            if (validity.geomDescr_.simpleGeometry_) {
                increment(validityUsage, "simple-geometry-with-address");
            }
            break;
        case Validity::OffsetPointValidity:
            increment(validityUsage["by-geometry-description"], "offset-point");
            break;
        case Validity::OffsetRangeValidity:
            increment(validityUsage["by-geometry-description"], "offset-range");
            break;
        case Validity::FeatureTransition:
            increment(validityUsage["by-geometry-description"], "feature-transition");
            break;
        }

        switch (validity.geomOffsetType_) {
        case Validity::InvalidOffsetType:
            increment(validityUsage["by-offset-type"], "invalid");
            break;
        case Validity::GeoPosOffset:
            increment(validityUsage["by-offset-type"], "geo-pos");
            break;
        case Validity::BufferOffset:
            increment(validityUsage["by-offset-type"], "buffer");
            break;
        case Validity::RelativeLengthOffset:
            increment(validityUsage["by-offset-type"], "relative-length");
            break;
        case Validity::MetricLengthOffset:
            increment(validityUsage["by-offset-type"], "metric-length");
            break;
        }

        if (validity.direction_ != Validity::Empty &&
            validity.geomDescrType_ == Validity::NoGeometry &&
            validity.geomOffsetType_ == Validity::InvalidOffsetType &&
            validity.referencedStage_ == ValidityData::InvalidReferencedStage &&
            !validity.featureAddress_) {
            increment(validityUsage, "direction-only");
        }
    }

    auto countSimpleValidity = [&](Validity::Direction direction) {
        increment(validityUsage, "total");
        increment(validityUsage, "simple-column");

        switch (direction) {
        case Validity::Empty: increment(validityUsage["by-direction"], "empty"); break;
        case Validity::Positive: increment(validityUsage["by-direction"], "positive"); break;
        case Validity::Negative: increment(validityUsage["by-direction"], "negative"); break;
        case Validity::Both: increment(validityUsage["by-direction"], "both"); break;
        case Validity::None: increment(validityUsage["by-direction"], "none"); break;
        }
        if (direction != Validity::Empty) {
            increment(validityUsage, "with-direction");
        }

        increment(validityUsage["by-geometry-description"], "none");
        increment(validityUsage["by-offset-type"], "invalid");
        increment(validityUsage, "direction-only");
    };

    std::unordered_set<uint32_t> seenValidityCollections;
    auto collectSimpleValidities = [&](simfil::ModelNodeAddress const& validityCollectionAddress) {
        if (!validityCollectionAddress) {
            return;
        }
        if (!seenValidityCollections.insert(validityCollectionAddress.value_).second) {
            return;
        }

        auto collection = resolve<MultiValidity>(validityCollectionAddress);
        if (!collection) {
            return;
        }

        for (auto const& validity : *collection) {
            auto const validityAddress = validity->addr();
            if (validityAddress.column() != ColumnId::SimpleValidity) {
                continue;
            }
            if (validityAddress.index() > static_cast<uint32_t>(Validity::None)) {
                continue;
            }
            countSimpleValidity(static_cast<Validity::Direction>(validityAddress.index()));
        }
    };

    for (auto const& attribute : impl_->attributes_) {
        collectSimpleValidities(attribute.validities_);
    }
    for (auto const& relation : impl_->relations_) {
        collectSimpleValidities(relation.sourceValidity_);
        collectSimpleValidities(relation.targetValidity_);
    }

    int64_t featureLayerTotal = 0;
    for (const auto& [_, value] : featureLayer.items()) {
        if (value.is_number_integer())
            featureLayerTotal += value.get<int64_t>();
    }

    auto modelStats = ModelPool::serializationSizeStats();
    auto modelPool = nlohmann::json::object({
        {"roots", static_cast<int64_t>(modelStats.rootsBytes)},
        {"int64", static_cast<int64_t>(modelStats.int64Bytes)},
        {"double", static_cast<int64_t>(modelStats.doubleBytes)},
        {"string-data", static_cast<int64_t>(modelStats.stringDataBytes)},
        {"string-ranges", static_cast<int64_t>(modelStats.stringRangeBytes)},
        {"object-members", static_cast<int64_t>(modelStats.objectMemberBytes)},
        {"array-members", static_cast<int64_t>(modelStats.arrayMemberBytes)},
    });

    int64_t modelPoolTotal = static_cast<int64_t>(modelStats.totalBytes());

    return {
        {"feature-layer", featureLayer},
        {"geometry-usage", geometryUsage},
        {"feature-lod-usage", featureLodUsage},
        {"validity-usage", validityUsage},
        {"array-arena-singletons", arrayArenaSingletons},
        {"model-pool", modelPool},
        {"feature-layer-total-bytes", featureLayerTotal},
        {"model-pool-total-bytes", modelPoolTotal},
        {"total-bytes", featureLayerTotal + modelPoolTotal}
    };
}

size_t TileFeatureLayer::size() const
{
    return numRoots();
}

uint64_t TileFeatureLayer::numVertices() const
{
    return static_cast<uint64_t>(pointBuffers_.byte_size() / sizeof(glm::vec3));
}

model_ptr<Feature> TileFeatureLayer::at(size_t i) const
{
    auto rootResult = root(i);
    if (!rootResult)
        return {};
    return resolve<Feature>(**rootResult);
}

model_ptr<Feature>
TileFeatureLayer::find(const std::string_view& type, const KeyValueViewPairs& queryIdParts) const
{
    auto const& primaryIdComposition = getPrimaryIdComposition(type);
    auto queryIdPartsStripped = stripOptionalIdParts(queryIdParts, primaryIdComposition);
    auto hash = static_cast<uint32_t>(Hash().mix(type).mix(queryIdPartsStripped).value());

    impl_->sortFeatureHashIndex();
    auto it = std::lower_bound(
        impl_->featureHashIndex_.begin(),
        impl_->featureHashIndex_.end(),
        FeatureAddrWithIdHash{ModelNodeAddress{0, 0}, hash},
        [](auto&& l, auto&& r) { return l.idHash_ < r.idHash_; });

    // Iterate through potential matches to handle hash collisions.
    while (it != impl_->featureHashIndex_.end() && it->idHash_ == hash)
    {
        auto feature = resolve<Feature>(it->featureAddr_);
        if (feature->id()->typeId() == type) {
            auto featureIdParts = stripOptionalIdParts(feature->id()->keyValuePairs(), primaryIdComposition);
            // Ensure that ID parts match exactly, not just the hash.
            if (featureIdParts.size() != queryIdPartsStripped.size()) {
                ++it;
                continue;
            }
            bool exactMatch = true;
            for (auto i = 0; i < featureIdParts.size(); ++i) {
                if (featureIdParts[i] != queryIdPartsStripped[i]) {
                    exactMatch = false;
                    break;
                }
            }
            if (exactMatch)
                return feature;
        }
        // Move to the next potential match.
        ++it;
    }

    return {};
}

model_ptr<Feature>
TileFeatureLayer::find(const std::string_view& type, const KeyValuePairs& queryIdParts) const
{
    return find(type, castToKeyValueView(queryIdParts));
}

std::vector<IdPart> const& TileFeatureLayer::getPrimaryIdComposition(const std::string_view& typeId) const
{
    auto typeIt = this->layerInfo_->featureTypes_.begin();
    while (typeIt != this->layerInfo_->featureTypes_.end()) {
        if (typeIt->name_ == typeId)
            break;
        ++typeIt;
    }
    if (typeIt == this->layerInfo_->featureTypes_.end()) {
        raise(fmt::format("Could not find feature type {}", typeId));
    }
    if (typeIt->uniqueIdCompositions_.empty()) {
        raise(fmt::format("No composition for feature type {}!", typeId));
    }
    return typeIt->uniqueIdCompositions_.front();
}

tl::expected<void, simfil::Error>
TileFeatureLayer::setStrings(std::shared_ptr<simfil::StringPool> const& newDict)
{
    auto oldDict = strings();
    // Reset simfil environment and clear expression cache
    impl_->expressionCache_.reset(Impl::makeSchemaAwareEnvironment(newDict, impl_->layerSchema_));
    if (auto res = ModelPool::setStrings(newDict); !res)
        return tl::unexpected<simfil::Error>(std::move(res.error()));

    if (!oldDict || *newDict == *oldDict)
        return {};

    // Re-map old string IDs to new string IDs
    for (auto& attr : impl_->attributes_) {
        if (auto resolvedName = oldDict->resolve(attr.name_)) {
            if (auto res = newDict->emplace(*resolvedName))
                attr.name_ = *res;
            else
                return tl::unexpected<simfil::Error>(res.error());
        }
    }
    for (auto& fid : featureIds_) {
        if (auto resolvedName = oldDict->resolve(fid.typeId_)) {
            if (auto res = newDict->emplace(*resolvedName))
                fid.typeId_ = *res;
            else
                return tl::unexpected<simfil::Error>(res.error());
        }
        if (fid.extMapId_ != simfil::StringPool::Empty) {
            if (auto resolvedName = oldDict->resolve(fid.extMapId_)) {
                if (auto res = newDict->emplace(*resolvedName))
                    fid.extMapId_ = *res;
                else
                    return tl::unexpected<simfil::Error>(res.error());
            }
        }
    }
    for (auto& feature : impl_->features_) {
        if (auto resolvedName = oldDict->resolve(feature.typeIdAndLod_.typeId_)) {
            if (auto res = newDict->emplace(*resolvedName))
                feature.typeIdAndLod_.typeId_ = *res;
            else
                return tl::unexpected<simfil::Error>(res.error());
        }
    }
    for (auto& rel : impl_->relations_) {
        if (auto resolvedName = oldDict->resolve(rel.name_)) {
            if (auto res = newDict->emplace(*resolvedName))
                rel.name_ = *res;
            else
                return tl::unexpected<simfil::Error>(res.error());
        }
    }
    return {};
}

ModelNode::Ptr TileFeatureLayer::clone(
    CloneCache& cache,
    const TileFeatureLayer::Ptr& otherLayer,
    const ModelNode::Ptr& otherNode)
{
    auto const cacheKey = CloneCacheKey{otherLayer.get(), otherNode->addr().value_};
    auto it = cache.find(cacheKey);
    if (it != cache.end()) {
        return it->second;
    }

    using namespace simfil;
    ModelNode::Ptr& newCacheNode = cache[cacheKey];
    switch (otherNode->addr().column()) {
    case Objects: {
        auto resolved = otherLayer->resolve<simfil::Object>(otherNode);
        auto newNode = newObject(resolved->size(), true);
        newCacheNode = newNode;
        for (auto [key, value] : resolved->fields()) {
            if (auto keyStr = otherLayer->strings()->resolve(key)) {
                newNode->addField(*keyStr, clone(cache, otherLayer, value));
            }
        }
        break;
    }
    case Arrays: {
        auto resolved = otherLayer->resolve<simfil::Array>(otherNode);
        auto newNode = newArray(resolved->size(), true);
        newCacheNode = newNode;
        for (auto value : *resolved) {
            newNode->append(clone(cache, otherLayer, value));
        }
        break;
    }
    case ColumnId::GeometryArrayView:
    case ColumnId::FeatureGeometryArrayView: {
        auto resolved = otherLayer->resolve<GeometryArrayView>(*otherNode);
        auto newNode = newArray(resolved->size(), true);
        newCacheNode = newNode;
        for (auto value : *resolved) {
            newNode->append(clone(cache, otherLayer, value));
        }
        break;
    }
    case ColumnId::PointGeometries:
    case ColumnId::LineGeometries:
    case ColumnId::PolygonGeometries:
    case ColumnId::MeshGeometries:
    case ColumnId::AabbGeometries:
    case ColumnId::GltfNodeIndexGeometries:
    case ColumnId::GeometryViews: {
        // TODO: This implementation is not great, because it does not respect
        //  Geometry views - it just converts every Geometry to a self-contained one.
        auto resolved = otherLayer->resolve<Geometry>(*otherNode);
        auto newNode = newGeometry(resolved->geomType(), resolved->numPoints(), true);
        newCacheNode = newNode;
        switch (resolved->geomType()) {
        case GeomType::GltfNodeIndex:
            newNode->setGltfNodeIndex(resolved->gltfNodeIndex());
            newNode->setGltfNodeBounds(
                resolved->gltfNodeAabbOrigin(),
                resolved->gltfNodeAabbSize());
            break;
        case GeomType::AABB:
            newNode->setAabb(resolved->aabbOrigin(), resolved->aabbSize());
            break;
        default:
            resolved->forEachPoint(
                [&newNode](auto&& pt)
                {
                    newNode->append(pt);
                    return true;
                });
            if (resolved->geomType() == GeomType::Polygon && resolved->numPolygonRings() > 1) {
                std::vector<uint32_t> ringStarts;
                ringStarts.reserve(resolved->numPolygonRings());
                for (uint32_t ringIndex = 0; ringIndex < resolved->numPolygonRings(); ++ringIndex) {
                    ringStarts.push_back(resolved->polygonRingStart(ringIndex));
                }
                newNode->setPolygonRingStarts(ringStarts);
            }
            break;
        }
        if (auto geometryStage = resolved->stage()) {
            if (*geometryStage > std::numeric_limits<uint8_t>::max()) {
                raiseFmt("Geometry stage {} exceeds uint8_t range during clone.", *geometryStage);
            }
            setGeometryStage(newNode->addr(), static_cast<uint8_t>(*geometryStage));
        }
        break;
    }
    case ColumnId::GeometryCollections:
    case ColumnId::FeatureGeometryCollectionView: {
        auto resolved = otherLayer->resolve<GeometryCollection>(*otherNode);
        auto newNode = newGeometryCollection(resolved->numGeometries(), true);
        newCacheNode = newNode;
        resolved->forEachGeometry(
            [this, &newNode, &cache, &otherLayer](auto&& geom)
            {
                newNode->addGeometry(resolve<Geometry>(*clone(cache, otherLayer, geom)));
                return true;
            });
        break;
    }
    case Int64: {
        otherLayer->resolve(*otherNode, Lambda([this, &newCacheNode](auto&& resolved){
            auto value = std::get<int64_t>(resolved.value());
            auto newNode = newValue(value);
            newCacheNode = newNode;
        }));
        break;
    }
    case Double: {
        otherLayer->resolve(*otherNode, Lambda([this, &newCacheNode](auto&& resolved){
            auto value = std::get<double>(resolved.value());
            auto newNode = newValue(value);
            newCacheNode = newNode;
        }));
        break;
    }
    case String: {
        otherLayer->resolve(*otherNode, Lambda([this, &newCacheNode](auto&& resolved){
            auto value = std::get<std::string_view>(resolved.value());
            auto newNode = newValue(value);
            newCacheNode = newNode;
        }));
        break;
    }
    case ColumnId::Features:
    case ColumnId::FeatureProperties: {
        raise("Cannot clone entire feature yet.");
    }
    case ColumnId::FeatureRelationsView: {
        auto resolved = otherLayer->resolve<RelationArrayView>(*otherNode);
        auto newNode = newArray(resolved->size(), true);
        newCacheNode = newNode;
        for (auto value : *resolved) {
            newNode->append(clone(cache, otherLayer, value));
        }
        break;
    }
    case ColumnId::FeatureIds: {
        auto resolved = otherLayer->resolve<FeatureId>(*otherNode);
        auto newNode = newFeatureId(
            resolved->typeId(),
            resolved->keyValuePairs(),
            resolved->externalMapId());
        newCacheNode = newNode;
        break;
    }
    case ColumnId::ExternalFeatureIds: {
        auto resolved = otherLayer->resolve<FeatureId>(*otherNode);
        auto newNode = newFeatureId(
            resolved->typeId(),
            resolved->keyValuePairs(),
            resolved->externalMapId());
        newCacheNode = newNode;
        break;
    }
    case ColumnId::Attributes: {
        auto resolved = otherLayer->resolve<Attribute>(*otherNode);
        auto newNode = newAttribute(resolved->name());
        newCacheNode = newNode;
        if (resolved->validityOrNull()) {
            newNode->setValidity(
                resolve<MultiValidity>(*clone(cache, otherLayer, resolved->validityOrNull())));
        }
        resolved->forEachField(
            [this, &newNode, &cache, &otherLayer](auto&& key, auto&& value)
            {
                newNode->addField(key, clone(cache, otherLayer, value));
                return true;
            });
        break;
    }
    case ColumnId::Validities: {
        auto resolved = otherLayer->resolve<Validity>(*otherNode);
        auto newNode = newValidity();
        newCacheNode = newNode;
        newNode->setDirection(resolved->direction());
        switch (resolved->geometryDescriptionType()) {
        case Validity::NoGeometry:
            break;
        case Validity::SimpleGeometry:
            newNode->setSimpleGeometry(resolve<Geometry>(
                *clone(cache, otherLayer, resolved->simpleGeometry())));
            break;
        case Validity::OffsetPointValidity:
            if (resolved->geometryOffsetType() == Validity::GeoPosOffset) {
                newNode->setOffsetPoint(*resolved->offsetPoint());
            }
            else {
                newNode->setOffsetPoint(resolved->geometryOffsetType(), resolved->offsetPoint()->x);
            }
            break;
        case Validity::OffsetRangeValidity:
            if (resolved->geometryOffsetType() == Validity::GeoPosOffset) {
                newNode->setOffsetRange(resolved->offsetRange()->first, resolved->offsetRange()->second);
            }
            else {
                newNode->setOffsetRange(resolved->geometryOffsetType(), resolved->offsetRange()->first.x, resolved->offsetRange()->second.x);
            }
            break;
        case Validity::FeatureTransition:
            newNode->setFeatureTransition(
                resolve<Feature>(*clone(cache, otherLayer, resolved->transitionFromFeature())),
                *resolved->transitionFromConnectedEnd(),
                resolve<Feature>(*clone(cache, otherLayer, resolved->transitionToFeature())),
                *resolved->transitionToConnectedEnd(),
                *resolved->transitionNumber());
            break;
        }
        break;
    }
    case ColumnId::SimpleValidity: {
        auto resolved = otherLayer->resolve<Validity>(*otherNode);
        newCacheNode = model_ptr<ModelNode>::make(
            shared_from_this(),
            ModelNodeAddress{
                ColumnId::SimpleValidity,
                static_cast<uint32_t>(resolved->direction())});
        break;
    }
    case ColumnId::ValidityCollections: {
        auto resolved = otherLayer->resolve<MultiValidity>(*otherNode);
        auto newNode = newValidityCollection(resolved->size(), true);
        newCacheNode = newNode;
        for (auto value : *resolved) {
            newNode->append(resolve<Validity>(*clone(cache, otherLayer, value)));
        }
        break;
    }
    case ColumnId::AttributeLayers: {
        auto resolved = otherLayer->resolve<AttributeLayer>(*otherNode);
        auto newNode = newAttributeLayer(resolved->size(), true);
        newCacheNode = newNode;
        for (auto [key, value] : resolved->fields()) {
            if (auto keyStr = otherLayer->strings()->resolve(key)) {
                if (*keyStr == AttributeLayer::InstanceIdField) {
                    auto const idValue = value->value();
                    if (auto const* signedId = std::get_if<int64_t>(&idValue); signedId && *signedId >= 0) {
                        newNode->setId(static_cast<uint64_t>(*signedId));
                    }
                    continue;
                }
                auto cloned = clone(cache, otherLayer, value);
                newNode->addField(*keyStr, resolve<Attribute>(*cloned));
            }
        }
        break;
    }
    case ColumnId::AttributeLayerLists:
    case ColumnId::FeatureAttributeLayerListView: {
        auto resolved = otherLayer->resolve<AttributeLayerList>(*otherNode);
        auto newNode = newAttributeLayers(resolved->size(), true);
        newCacheNode = newNode;
        for (auto [key, value] : resolved->fields()) {
            if (auto keyStr = otherLayer->strings()->resolve(key)) {
                auto cloned = clone(cache, otherLayer, value);
                newNode->addLayer(*keyStr, resolve<AttributeLayer>(*cloned));
            }
        }
        break;
    }
    case ColumnId::Relations: {
        auto resolved = otherLayer->resolve<Relation>(*otherNode);
        auto newNode = newRelation(
            resolved->name(),
            resolve<FeatureId>(*clone(cache, otherLayer, resolved->target())));
        if (resolved->sourceValidityOrNull()) {
            newNode->setSourceValidity(resolve<MultiValidity>(
                *clone(cache, otherLayer, resolved->sourceValidityOrNull())));
        }
        if (resolved->targetValidityOrNull()) {
            newNode->setTargetValidity(resolve<MultiValidity>(
                *clone(cache, otherLayer, resolved->targetValidityOrNull())));
        }
        newCacheNode = newNode;
        break;
    }
    case ColumnId::RelationReferences: {
        auto resolved = otherLayer->resolve<RelationReference>(*otherNode);
        auto newNode = newRelationReference(
            resolve<Relation>(*clone(cache, otherLayer, resolved->relation())));
        newCacheNode = newNode;
        break;
    }
    case ColumnId::SourceDataReferenceCollections: {
        auto resolved = otherLayer->resolve<SourceDataReferenceCollection>(*otherNode);
        auto items = std::vector<QualifiedSourceDataReference>(
            otherLayer->sourceDataReferences_.begin() + resolved->offset_,
            otherLayer->sourceDataReferences_.begin() + resolved->offset_ + resolved->size_);
        newCacheNode = newSourceDataReferenceCollection({items.begin(), items.end()});
        break;
    }
    case ColumnId::Points:
    case ColumnId::Mesh:
    case ColumnId::MeshTriangleCollection:
    case ColumnId::MeshTriangleLinearRing:
    case ColumnId::Polygon:
    case ColumnId::LinearRing:
    case ColumnId::PointBuffers:
    case ColumnId::PointBuffersView:
    case ColumnId::SourceDataReferences:
    case ColumnId::ValidityPoints:
        raiseFmt("Encountered unexpected column type {} in clone().", otherNode->addr().column());
    default: {
        newCacheNode = resolve(otherNode->addr());
    }
    }
    cache.insert({cacheKey, newCacheNode});
    return newCacheNode;
}

void TileFeatureLayer::clone(
    CloneCache& clonedModelNodes,
    const TileFeatureLayer::Ptr& otherLayer,
    const Feature& otherFeature,
    const std::string_view& type,
    KeyValueViewPairs idParts)
{
    auto cloneTarget = find(type, idParts);
    if (!cloneTarget) {
        // Remove tile ID prefix from idParts to create a new feature.
        if (auto idPrefix = getIdPrefix(); idPrefix && idParts.size() >= idPrefix->size()) {
            idParts = KeyValueViewPairs(
                idParts.begin()+idPrefix->size(), idParts.end());
        }
        cloneTarget = newFeature(type, idParts);
    }

    auto lookupOrClone =
        [&](ModelNode::Ptr const& n) -> ModelNode::Ptr
    {
        return clone(clonedModelNodes, otherLayer, n);
    };

    auto owningLayer =
        [](auto const& nodePtr) -> TileFeatureLayer::Ptr
    {
        return std::static_pointer_cast<TileFeatureLayer>(nodePtr->model().shared_from_this());
    };

    // Adopt attributes
    if (auto attrs = otherFeature.attributesOrNull()) {
        auto baseAttrs = cloneTarget->attributes();
        for (auto const& [key, value] : attrs->fields()) {
            if (auto keyStr = otherLayer->strings()->resolve(key)) {
                baseAttrs->addField(*keyStr, lookupOrClone(value));
            }
        }
    }

    // Adopt attribute layers
    if (auto attrLayers = otherFeature.attributeLayersOrNull()) {
        auto baseAttrLayers = cloneTarget->attributeLayers();
        attrLayers->forEachLayer(
            [this, &baseAttrLayers, &clonedModelNodes, &owningLayer](std::string_view layerName, model_ptr<AttributeLayer> const& layer)
            {
                auto cloned = clone(clonedModelNodes, owningLayer(layer), ModelNode::Ptr(layer));
                baseAttrLayers->addLayer(layerName, resolve<AttributeLayer>(*cloned));
                return true;
            });
    }

    // Adopt geometries
    if (auto geom = otherFeature.geomOrNull()) {
        auto baseGeom = cloneTarget->geom();
        geom->forEachGeometry(
            [this, &baseGeom, &clonedModelNodes, &owningLayer](auto&& geomElement)
            {
                baseGeom->addGeometry(
                    resolve<Geometry>(*clone(clonedModelNodes, owningLayer(geomElement), ModelNode::Ptr(geomElement))));
                return true;
            });
    }

    // Adopt relations
    if (otherFeature.numRelations()) {
        otherFeature.forEachRelation(
            [this, &cloneTarget, &clonedModelNodes, &owningLayer](auto&& rel)
            {
                auto newRel = resolve<Relation>(*clone(clonedModelNodes, owningLayer(rel), ModelNode::Ptr(rel)));
                cloneTarget->addRelation(newRel);
                return true;
            });
    }
}

std::optional<uint8_t> TileFeatureLayer::geometryStage(simfil::ModelNodeAddress address) const
{
    switch (address.column()) {
    case ColumnId::PointGeometries:
    case ColumnId::LineGeometries:
    case ColumnId::PolygonGeometries:
    case ColumnId::MeshGeometries:
    case ColumnId::AabbGeometries:
    case ColumnId::GltfNodeIndexGeometries: {
        auto const compactIndex = extraGeometryDataStorageIndex(address.index());
        if (auto storedStage = geometryStageAt(geomStages_, compactIndex)) {
            return storedStage;
        }
        auto fallbackStage = stage_.value_or(layerInfo_ ? layerInfo_->highFidelityStage_ : 0U);
        if (fallbackStage > std::numeric_limits<uint8_t>::max()) {
            raiseFmt("Geometry stage {} exceeds uint8_t range.", fallbackStage);
        }
        return static_cast<uint8_t>(fallbackStage);
    }
    case ColumnId::GeometryViews:
        return geometryStage(geomViews_.at(address.index()).baseGeometry_);
    default:
        return std::nullopt;
    }
}

model_ptr<Feature> TileFeatureLayer::find(const std::string_view& featureId) const
{
    ParsedFeatureId parsed;
    if (!parseFeatureIdString(featureId, *layerInfo_, parsed)) {
        return {};
    }
    return find(parsed.typeId_, parsed.keyValuePairs_);
}

}
