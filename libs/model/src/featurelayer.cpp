#include "featurelayer.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <bitsery/bitsery.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/ext/std_optional.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>
#include <noserde.hpp>

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
void serialize(S& s, glm::fvec3& v) {
    s.value4b(v.x);
    s.value4b(v.y);
    s.value4b(v.z);
}

}

namespace
{
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

    class CountingStreambuf : public std::streambuf
    {
    public:
        size_t size() const { return size_; }

    protected:
        std::streamsize xsputn(const char* /*s*/, std::streamsize count) override
        {
            size_ += static_cast<size_t>(count);
            return count;
        }

        int overflow(int ch) override
        {
            if (ch != EOF)
                ++size_;
            return ch;
        }

    private:
        size_t size_ = 0;
    };

    template <class Fn>
    size_t measureBytes(Fn&& fn)
    {
        CountingStreambuf buf;
        std::ostream os(&buf);
        bitsery::Serializer<bitsery::OutputStreamAdapter> s(os);
        fn(s);
        return buf.size();
    }
}

namespace mapget
{

struct FeatureAddrWithIdHash
{
    ModelNodeAddress featureAddr_{};
    uint64_t idHash_ = 0;

    FeatureAddrWithIdHash() = default;
    FeatureAddrWithIdHash(ModelNodeAddress featureAddr, uint64_t idHash)
        : featureAddr_(featureAddr),
          idHash_(idHash)
    {}

    bool operator< (FeatureAddrWithIdHash const& other) const {
        return std::tie(idHash_, featureAddr_) < std::tie(other.idHash_, other.featureAddr_);
    }
};

struct TileFeatureLayer::Impl {
    ModelNodeAddress featureIdPrefix_;

    noserde::Buffer<Feature::Data, simfil::detail::ColumnPageSize / 4> features_;
    noserde::Buffer<Attribute::Data, simfil::detail::ColumnPageSize> attributes_;
    noserde::Buffer<Validity::Data, simfil::detail::ColumnPageSize> validities_;
    noserde::Buffer<FeatureId::Data, simfil::detail::ColumnPageSize / 2> featureIds_;
    noserde::Buffer<simfil::ArrayIndex, simfil::detail::ColumnPageSize / 2> attrLayers_;
    noserde::Buffer<simfil::ArrayIndex, simfil::detail::ColumnPageSize / 2> attrLayerLists_;
    noserde::Buffer<Relation::Data, simfil::detail::ColumnPageSize / 2> relations_;
    noserde::Buffer<Geometry::Data, simfil::detail::ColumnPageSize / 2> geom_;
    noserde::Buffer<QualifiedSourceDataReference, simfil::detail::ColumnPageSize / 2> sourceDataReferences_;
    Geometry::Storage pointBuffers_;
    std::unordered_map<uint32_t, std::pair<TileFeatureLayer const*, ModelNodeAddress>> mergedArrayExtensions_;

    /**
     * Indexing of features by their id hash. The hash-feature pairs are kept
     * in a vector, which is kept in a sorted state. This allows finding a
     * feature by its id in O(log(n)) time.
     */
    noserde::Buffer<FeatureAddrWithIdHash, simfil::detail::ColumnPageSize / 4> featureHashIndex_;
    bool featureHashIndexNeedsSorting_ = false;

    void sortFeatureHashIndex() {
        if (!featureHashIndexNeedsSorting_)
            return;
        featureHashIndexNeedsSorting_ = false;
        std::sort(featureHashIndex_.begin(), featureHashIndex_.end());
    }

    // Simfil compiled expression cache and environment
    SimfilExpressionCache expressionCache_;

    // (De-)Serialization
    template<typename S>
    void readWrite(S& s) {
        s.object(features_);
        s.object(attributes_);
        s.object(validities_);
        s.object(featureIds_);
        s.object(attrLayers_);
        s.object(attrLayerLists_);
        s.object(featureIdPrefix_);
        s.object(relations_);
        sortFeatureHashIndex();
        s.object(featureHashIndex_);
        s.object(geom_);
        s.ext(pointBuffers_, bitsery::ext::ArrayArenaExt{});
        s.object(sourceDataReferences_);
    }

    explicit Impl(std::shared_ptr<simfil::StringPool> stringPool)
        : expressionCache_(makeEnvironment(std::move(stringPool)))
    {
    }

};

TileFeatureLayer::TileFeatureLayer(
    TileId tileId,
    std::string const& nodeId,
    std::string const& mapId,
    std::shared_ptr<LayerInfo> const& layerInfo,
    std::shared_ptr<simfil::StringPool> const& strings) :
    ModelPool(strings),
    impl_(std::make_unique<Impl>(strings)),
    TileLayer(tileId, nodeId, mapId, layerInfo)
{
}

TileFeatureLayer::TileFeatureLayer(
    const std::vector<uint8_t>& input,
    LayerInfoResolveFun const& layerInfoResolveFun,
    StringPoolResolveFun const& stringPoolGetter
) :
    TileLayer(input, layerInfoResolveFun, &deserializationOffsetBytes_),
    ModelPool(stringPoolGetter(nodeId_)),
    impl_(std::make_unique<Impl>(stringPoolGetter(nodeId_)))
{
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    if (deserializationOffsetBytes_ > input.size()) {
        raise("Failed to read TileFeatureLayer: invalid deserialization offset.");
    }
    bitsery::Deserializer<Adapter> s(Adapter(
        input.begin() + static_cast<std::ptrdiff_t>(deserializationOffsetBytes_),
        input.end()));
    s.ext4b(stage_, bitsery::ext::StdOptional{});
    impl_->readWrite(s);
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

TileFeatureLayer::~TileFeatureLayer() = default;

std::optional<uint32_t> TileFeatureLayer::stage() const
{
    return stage_;
}

void TileFeatureLayer::setStage(std::optional<uint32_t> stage)
{
    stage_ = stage;
}

void TileFeatureLayer::attachOverlay(TileFeatureLayer::Ptr const& overlay)
{
    if (!overlay) {
        return;
    }

    if (overlay->size() < size()) {
        raiseFmt(
            "Overlay feature count {} is smaller than base feature count {}.",
            overlay->size(),
            size());
    }

    if (overlay_) {
        overlay_->attachOverlay(overlay);
        return;
    }

    overlay_ = overlay;
}

TileFeatureLayer::Ptr TileFeatureLayer::overlay() const
{
    return overlay_;
}

void TileFeatureLayer::setMergedArrayExtension(
    ModelNodeAddress baseAddress,
    TileFeatureLayer const* extensionModel,
    ModelNodeAddress extensionAddress)
{
    if (!baseAddress || !extensionModel || !extensionAddress) {
        clearMergedArrayExtension(baseAddress);
        return;
    }
    impl_->mergedArrayExtensions_[baseAddress.value_] = {
        extensionModel,
        extensionAddress};
}

void TileFeatureLayer::clearMergedArrayExtension(ModelNodeAddress baseAddress)
{
    if (!baseAddress) {
        return;
    }
    impl_->mergedArrayExtensions_.erase(baseAddress.value_);
}

std::optional<std::pair<TileFeatureLayer const*, ModelNodeAddress>>
TileFeatureLayer::mergedArrayExtension(ModelNodeAddress baseAddress) const
{
    if (!baseAddress) {
        return {};
    }
    auto it = impl_->mergedArrayExtensions_.find(baseAddress.value_);
    if (it == impl_->mergedArrayExtensions_.end()) {
        return {};
    }
    return it->second;
}

namespace
{

/**
 * Create a string representation of the given id parts.
 */
std::string idPartsToString(KeyValueViewPairs const& idParts) {
    std::stringstream result;
    result << "{";
    for (auto i = 0; i < idParts.size(); ++i) {
        if (i > 0)
            result << ", ";
        result << idParts[i].first << ": ";
        std::visit([&result](auto&& value){
           result << value;
        }, idParts[i].second);
    }
    result << "}";
    return result.str();
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

}  // namespace

simfil::model_ptr<Feature> TileFeatureLayer::newFeature(
    const std::string_view& typeId,
    const KeyValueViewPairs& featureIdParts)
{
    if (featureIdParts.empty()) {
        raise("Tried to create an empty feature ID.");
    }

    uint32_t idPrefixLength = 0;
    if (auto const idPrefix = getIdPrefix())
        idPrefixLength = idPrefix->size();

    if (!layerInfo_->validFeatureId(typeId, featureIdParts, true, idPrefixLength)) {
        raise(fmt::format(
            "Could not find a matching ID composition of type {} with parts {}.",
            typeId,
            idPartsToString(featureIdParts)));
    }

    auto featureIdIndex = impl_->featureIds_.size();
    auto featureIdObject = newObject(featureIdParts.size());
    auto res = strings()->emplace(typeId);
    if (!res)
        raise(res.error().message);

    impl_->featureIds_.emplace_back(FeatureId::Data{
        true,
        *res,
        featureIdObject->addr()
    });
    for (auto const& [k, v] : featureIdParts) {
        auto&& kk = k;
        std::visit([&](auto&& x){
            featureIdObject->addField(kk, x);
        }, v);
    }

    auto featureIndex = impl_->features_.size();
    impl_->features_.emplace_back(Feature::Data{
        ModelNodeAddress{ColumnId::FeatureIds, (uint32_t)featureIdIndex},
        ModelNodeAddress{Null, 0},
        ModelNodeAddress{Null, 0},
        ModelNodeAddress{Null, 0},
        ModelNodeAddress{Null, 0},
    });
    auto result = Feature(
        impl_->features_.back(),
        shared_from_this(),
        ModelNodeAddress{ColumnId::Features, (uint32_t)featureIndex},
        mpKey_);

    // Add feature hash index entry.
    auto const& primaryIdComposition = getPrimaryIdComposition(typeId);
    auto fullStrippedFeatureId = stripOptionalIdParts(result.id()->keyValuePairs(), primaryIdComposition);
    auto hash = Hash().mix(typeId).mix(fullStrippedFeatureId).value();
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
    const KeyValueViewPairs& featureIdParts)
{
    if (!layerInfo_->validFeatureId(typeId, featureIdParts, false)) {
        raise(fmt::format(
            "Could not find a matching ID composition of type {} with parts {}.",
            typeId,
            idPartsToString(featureIdParts)));
    }

    auto featureIdObject = newObject(featureIdParts.size());
    auto featureIdIndex = impl_->featureIds_.size();
    auto typeIdStringId = strings()->emplace(typeId);
    if (!typeIdStringId)
        raise(typeIdStringId.error().message);
    impl_->featureIds_.emplace_back(FeatureId::Data{
        false,
        *typeIdStringId,
        featureIdObject->addr()
    });
    for (auto const& [k, v] : featureIdParts) {
        auto&& kk = k;
        std::visit([&](auto&& x){
            featureIdObject->addField(kk, x);
        }, v);
    }
    return FeatureId(
        impl_->featureIds_.back(),
        shared_from_this(),
        {ColumnId::FeatureIds, (uint32_t)featureIdIndex},
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
        *nameStringId,
        target->addr()
    });
    return Relation(
        &impl_->relations_.back(),
        shared_from_this(),
        {ColumnId::Relations, (uint32_t)relationIndex},
        mpKey_);
}

model_ptr<Object> TileFeatureLayer::getIdPrefix()
{
    if (impl_->featureIdPrefix_)
        return resolve<simfil::Object>(impl_->featureIdPrefix_);
    return {};
}

model_ptr<Attribute>
TileFeatureLayer::newAttribute(const std::string_view& name, size_t initialCapacity)
{
    auto attrIndex = impl_->attributes_.size();
    auto nameStringId = strings()->emplace(name);
    if (!nameStringId)
        raise(nameStringId.error().message);
    impl_->attributes_.emplace_back(Attribute::Data{
        {Null, 0},
        objectMemberStorage().new_array(initialCapacity),
        *nameStringId,
    });
    return Attribute(
        &impl_->attributes_.back(),
        shared_from_this(),
        {ColumnId::Attributes, (uint32_t)attrIndex},
        mpKey_);
}

model_ptr<AttributeLayer> TileFeatureLayer::newAttributeLayer(size_t initialCapacity)
{
    auto layerIndex = impl_->attrLayers_.size();
    impl_->attrLayers_.emplace_back(objectMemberStorage().new_array(initialCapacity));
    return AttributeLayer(
        impl_->attrLayers_.back(),
        shared_from_this(),
        {ColumnId::AttributeLayers, (uint32_t)layerIndex},
        mpKey_);
}

model_ptr<AttributeLayerList> TileFeatureLayer::newAttributeLayers(size_t initialCapacity)
{
    auto listIndex = impl_->attrLayerLists_.size();
    impl_->attrLayerLists_.emplace_back(objectMemberStorage().new_array(initialCapacity));
    return AttributeLayerList(
        impl_->attrLayerLists_.back(),
        shared_from_this(),
        {ColumnId::AttributeLayerLists, (uint32_t)listIndex},
        mpKey_);
}

model_ptr<GeometryCollection> TileFeatureLayer::newGeometryCollection(size_t initialCapacity)
{
    auto listIndex = arrayMemberStorage().new_array(initialCapacity);
    return GeometryCollection(
        shared_from_this(),
        {ColumnId::GeometryCollections, (uint32_t)listIndex},
        mpKey_);
}

model_ptr<Geometry> TileFeatureLayer::newGeometry(GeomType geomType, size_t initialCapacity)
{
    initialCapacity = std::max((size_t)1, initialCapacity);
    impl_->geom_.emplace_back(geomType, initialCapacity);
    return Geometry(
        &impl_->geom_.back(),
        shared_from_this(),
        {ColumnId::Geometries, (uint32_t)impl_->geom_.size() - 1},
        mpKey_);
}

model_ptr<Geometry> TileFeatureLayer::newGeometryView(
    GeomType geomType,
    uint32_t offset,
    uint32_t size,
    const model_ptr<Geometry>& base)
{
    impl_->geom_.emplace_back(geomType, offset, size, base->addr());
    return Geometry(
        &impl_->geom_.back(),
        shared_from_this(),
        {ColumnId::Geometries, (uint32_t)impl_->geom_.size() - 1},
        mpKey_);
}

model_ptr<SourceDataReferenceCollection> TileFeatureLayer::newSourceDataReferenceCollection(std::span<QualifiedSourceDataReference> list)
{
    auto& arena = impl_->sourceDataReferences_;
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

model_ptr<MultiValidity> TileFeatureLayer::newValidityCollection(size_t initialCapacity)
{
    auto validityArrId = arrayMemberStorage().new_array(initialCapacity);
    return MultiValidity(
        shared_from_this(),
        {ColumnId::ValidityCollections, (uint32_t)validityArrId},
        mpKey_);
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
    if (node.addr().column() != TileFeatureLayer::ColumnId::AttributeLayerLists)
        raise("Cannot cast this node to an AttributeLayerList.");
    return AttributeLayerList(
        model.impl_->attrLayerLists_[node.addr().index()],
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
    if (node.addr().column() != TileFeatureLayer::ColumnId::Features)
        raise("Cannot cast this node to a Feature.");
    auto result = Feature(
        model.impl_->features_[node.addr().index()],
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);

    if (model.overlay_ && node.addr().index() < model.overlay_->size()) {
        result.setExtension(model.overlay_->at(node.addr().index()));
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

    if (model.overlay_ && node.addr().index() < model.overlay_->size()) {
        result.setExtension(model.overlay_->resolve<RelationArrayView>(
            ModelNodeAddress{TileFeatureLayer::ColumnId::FeatureRelationsView, node.addr().index()}));
    } else {
        result.setExtension({});
    }
    return result;
}

template<>
model_ptr<FeatureId> resolveInternal(tag<FeatureId>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::FeatureIds)
        raise("Cannot cast this node to a FeatureId.");
    return FeatureId(
        model.impl_->featureIds_[node.addr().index()],
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<Relation> resolveInternal(tag<Relation>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::Relations)
        raise("Cannot cast this node to a Relation.");
    return Relation(
        &model.impl_->relations_[node.addr().index()],
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<PointNode> resolveInternal(tag<PointNode>, TileFeatureLayer const& model, ModelNode const& node)
{
    switch (node.addr().column()) {
    case TileFeatureLayer::ColumnId::Points:
        return PointNode(node, &model.impl_->geom_.at(node.addr().index()), model.mpKey_);
    case TileFeatureLayer::ColumnId::ValidityPoints:
        return PointNode(node, &model.impl_->validities_.at(node.addr().index()), model.mpKey_);
    default:
        raise("Cannot cast this node to a Point.");
    }
}

template<>
model_ptr<PointBufferNode> resolveInternal(tag<PointBufferNode>, TileFeatureLayer const& model, ModelNode const& node)
{
    return PointBufferNode(
        &model.impl_->geom_.at(node.addr().index()),
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<Geometry> resolveInternal(tag<Geometry>, TileFeatureLayer const& model, ModelNode const& node)
{
    auto* geomData = &model.impl_->geom_.at(node.addr().index());
    using MutableGeomData =
        std::remove_const_t<std::remove_reference_t<decltype(*geomData)>>;
    return Geometry(
        const_cast<MutableGeomData*>(geomData), // FIXME: const_cast?!
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<GeometryCollection> resolveInternal(tag<GeometryCollection>, TileFeatureLayer const& model, ModelNode const& node)
{
    return GeometryCollection(
        model.shared_from_this(), node.addr(), model.mpKey_);
}

template<>
model_ptr<GeometryArrayView> resolveInternal(tag<GeometryArrayView>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::GeometryArrayView)
        raise("Cannot cast this node to a GeometryArrayView.");
    return GeometryArrayView(
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<MeshNode> resolveInternal(tag<MeshNode>, TileFeatureLayer const& model, ModelNode const& node)
{
    return MeshNode(
        &model.impl_->geom_.at(node.addr().index()),
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<MeshTriangleCollectionNode> resolveInternal(tag<MeshTriangleCollectionNode>, TileFeatureLayer const& model, ModelNode const& node)
{
    return MeshTriangleCollectionNode(node, model.mpKey_);
}

template<>
model_ptr<LinearRingNode> resolveInternal(tag<LinearRingNode>, TileFeatureLayer const& model, ModelNode const& node)
{
    switch (node.addr().column()) {
    case TileFeatureLayer::ColumnId::LinearRing:
        return LinearRingNode(node, model.mpKey_);
    case TileFeatureLayer::ColumnId::MeshTriangleLinearRing:
        return LinearRingNode(node, 3, model.mpKey_);
    default:
        raise("Cannot cast this node to a LinearRing.");
    }
}

template<>
model_ptr<PolygonNode> resolveInternal(tag<PolygonNode>, TileFeatureLayer const& model, ModelNode const& node)
{
    return PolygonNode(
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<SourceDataReferenceCollection> resolveInternal(tag<SourceDataReferenceCollection>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::SourceDataReferenceCollections)
        raise("Cannot cast this node to an SourceDataReferenceCollection.");

    auto [index, size] = modelAddressToSourceDataAddressList(node.addr().index());
    return SourceDataReferenceCollection(index, size, model.shared_from_this(), node.addr(), model.mpKey_);
}

template<>
model_ptr<SourceDataReferenceItem> resolveInternal(tag<SourceDataReferenceItem>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::SourceDataReferences)
        raise("Cannot cast this node to an SourceDataReferenceItem.");

    const auto* data = &model.impl_->sourceDataReferences_.at(node.addr().index());
    return SourceDataReferenceItem(data, model.shared_from_this(), node.addr(), model.mpKey_);
}

template<>
model_ptr<Validity> resolveInternal(tag<Validity>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::Validities)
        raise("Cannot cast this node to a Validity.");
    return Validity(
        &model.impl_->validities_[node.addr().index()],
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<MultiValidity> resolveInternal(tag<MultiValidity>, TileFeatureLayer const& model, ModelNode const& node)
{
    if (node.addr().column() != TileFeatureLayer::ColumnId::ValidityCollections)
        raise("Cannot cast this node to a ValidityCollection.");
    return MultiValidity(
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

tl::expected<void, simfil::Error> TileFeatureLayer::resolve(const ModelNode& n, const simfil::Model::ResolveFn& cb) const
{
    switch (n.addr().column())
    {
    case ColumnId::Features:
        cb(*resolve<Feature>(n));
        return {};
    case ColumnId::FeatureProperties:
        cb(Feature::FeaturePropertyView(resolve<Feature>(ModelNodeAddress{ColumnId::Features, n.addr().index()}), mpKey_));
        return {};
    case ColumnId::FeatureRelationsView:
        cb(*resolve<RelationArrayView>(n));
        return {};
    case ColumnId::FeatureIds:
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
    case ColumnId::Points:
        cb(*resolve<PointNode>(n));
        return {};
    case ColumnId::PointBuffers:
        cb(*resolve<PointBufferNode>(n));
        return {};
    case ColumnId::Geometries:
        cb(*resolve<Geometry>(n));
        return {};
    case ColumnId::GeometryCollections:
        cb(*resolve<GeometryCollection>(n));
        return {};
    case ColumnId::GeometryArrayView:
        cb(*resolve<GeometryArrayView>(n));
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
    return impl_->expressionCache_.diagnostics(query, diag, anyMode);
}

tl::expected<std::vector<simfil::CompletionCandidate>, simfil::Error>
TileFeatureLayer::complete(std::string_view query, int point, ModelNode const& node, simfil::CompletionOptions const& opts)
{
    return impl_->expressionCache_.completions(query, point, node, opts);
}

void TileFeatureLayer::setIdPrefix(const KeyValueViewPairs& prefix)
{
    // The prefix must be set, before any feature is added.
    if (!impl_->features_.empty())
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

    auto idPrefix = newObject(prefix.size());
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
    return ModelPool::write(outputStream);
}

nlohmann::json TileFeatureLayer::toJson() const
{
    auto result = nlohmann::json::object();

    result["type"] = "FeatureCollection";
    result["mapgetTileId"] = tileId_.value_;
    result["mapId"] = mapId_;
    result["mapgetLayerId"] = layerInfo_->layerId_;

    // Add ID prefix if set
    if (impl_->featureIdPrefix_) {
        auto prefix = const_cast<TileFeatureLayer*>(this)->getIdPrefix();
        if (prefix)
            result["idPrefix"] = prefix->toJson();
    }

    // Add timestamp as ISO 8601 string
    {
        auto time_t_val = std::chrono::system_clock::to_time_t(timestamp_);
        auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
            timestamp_.time_since_epoch()).count() % 1000000;
        std::tm tm_val{};
#ifdef _WIN32
        gmtime_s(&tm_val, &time_t_val);
#else
        gmtime_r(&time_t_val, &tm_val);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_val);
        result["timestamp"] = fmt::format("{}.{:06d}Z", buf, microseconds);
    }

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

nlohmann::json TileFeatureLayer::serializationSizeStats() const
{
    auto featureLayer = nlohmann::json::object();

    featureLayer["features"] = static_cast<int64_t>(measureBytes(
        [&](auto& s) { s.object(const_cast<noserde::Buffer<Feature::Data, simfil::detail::ColumnPageSize / 4>&>(impl_->features_)); }));
    featureLayer["attributes"] = static_cast<int64_t>(measureBytes(
        [&](auto& s) { s.object(const_cast<noserde::Buffer<Attribute::Data, simfil::detail::ColumnPageSize>&>(impl_->attributes_)); }));
    featureLayer["validities"] = static_cast<int64_t>(measureBytes(
        [&](auto& s) { s.object(const_cast<noserde::Buffer<Validity::Data, simfil::detail::ColumnPageSize>&>(impl_->validities_)); }));
    featureLayer["feature-ids"] = static_cast<int64_t>(measureBytes(
        [&](auto& s) { s.object(const_cast<noserde::Buffer<FeatureId::Data, simfil::detail::ColumnPageSize / 2>&>(impl_->featureIds_)); }));
    featureLayer["attribute-layers"] = static_cast<int64_t>(measureBytes(
        [&](auto& s) { s.object(const_cast<noserde::Buffer<simfil::ArrayIndex, simfil::detail::ColumnPageSize / 2>&>(impl_->attrLayers_)); }));
    featureLayer["attribute-layer-lists"] = static_cast<int64_t>(measureBytes(
        [&](auto& s) { s.object(const_cast<noserde::Buffer<simfil::ArrayIndex, simfil::detail::ColumnPageSize / 2>&>(impl_->attrLayerLists_)); }));
    featureLayer["feature-id-prefix"] = static_cast<int64_t>(measureBytes(
        [&](auto& s) { s.object(const_cast<ModelNodeAddress&>(impl_->featureIdPrefix_)); }));
    featureLayer["relations"] = static_cast<int64_t>(measureBytes(
        [&](auto& s) { s.object(const_cast<noserde::Buffer<Relation::Data, simfil::detail::ColumnPageSize / 2>&>(impl_->relations_)); }));
    featureLayer["feature-hash-index"] = static_cast<int64_t>(measureBytes(
        [&](auto& s) { s.object(const_cast<noserde::Buffer<FeatureAddrWithIdHash, simfil::detail::ColumnPageSize / 4>&>(impl_->featureHashIndex_)); }));
    featureLayer["geometries"] = static_cast<int64_t>(measureBytes(
        [&](auto& s) { s.object(const_cast<noserde::Buffer<Geometry::Data, simfil::detail::ColumnPageSize / 2>&>(impl_->geom_)); }));
    featureLayer["point-buffers"] = static_cast<int64_t>(measureBytes(
        [&](auto& s) { s.ext(const_cast<Geometry::Storage&>(impl_->pointBuffers_), bitsery::ext::ArrayArenaExt{}); }));
    featureLayer["source-data-references"] = static_cast<int64_t>(measureBytes(
        [&](auto& s) { s.object(const_cast<noserde::Buffer<QualifiedSourceDataReference, simfil::detail::ColumnPageSize / 2>&>(impl_->sourceDataReferences_)); }));

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
    auto hash = Hash().mix(type).mix(queryIdPartsStripped).value();

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
    impl_->expressionCache_.reset(makeEnvironment(newDict));
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
    for (auto& validity : impl_->validities_) {
        if (auto resolvedName = strings()->resolve(validity.referencedGeomName_)) {
            if (auto res = newDict->emplace(*resolvedName))
                validity.referencedGeomName_ = *res;
            else
                return tl::unexpected<simfil::Error>(res.error());
        }
    }
    for (auto& fid : impl_->featureIds_) {
        if (auto resolvedName = oldDict->resolve(fid.typeId_)) {
            if (auto res = newDict->emplace(*resolvedName))
                fid.typeId_ = *res;
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
    std::unordered_map<uint32_t, ModelNode::Ptr>& cache,
    const TileFeatureLayer::Ptr& otherLayer,
    const ModelNode::Ptr& otherNode)
{
    auto it = cache.find(otherNode->addr().value_);
    if (it != cache.end()) {
        return it->second;
    }

    using namespace simfil;
    ModelNode::Ptr& newCacheNode = cache[otherNode->addr().value_];
    switch (otherNode->addr().column()) {
    case Objects: {
        auto resolved = otherLayer->resolve<simfil::Object>(otherNode);
        auto newNode = newObject(resolved->size());
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
        auto newNode = newArray(resolved->size());
        newCacheNode = newNode;
        for (auto value : *resolved) {
            newNode->append(clone(cache, otherLayer, value));
        }
        break;
    }
    case ColumnId::GeometryArrayView: {
        auto resolved = otherLayer->resolve<GeometryArrayView>(*otherNode);
        auto newNode = newArray(resolved->size());
        newCacheNode = newNode;
        for (auto value : *resolved) {
            newNode->append(clone(cache, otherLayer, value));
        }
        break;
    }
    case ColumnId::Geometries: {
        // TODO: This implementation is not great, because it does not respect
        //  Geometry views - it just converts every Geometry to a self-contained one.
        // TODO: Clone geometry name.
        auto resolved = otherLayer->resolve<Geometry>(*otherNode);
        auto newNode = newGeometry(resolved->geomType(), resolved->numPoints());
        newCacheNode = newNode;
        resolved->forEachPoint(
            [&newNode](auto&& pt)
            {
                newNode->append(pt);
                return true;
            });
        break;
    }
    case ColumnId::GeometryCollections: {
        auto resolved = otherLayer->resolve<GeometryCollection>(*otherNode);
        auto newNode = newGeometryCollection(resolved->numGeometries());
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
        auto newNode = newArray(resolved->size());
        newCacheNode = newNode;
        for (auto value : *resolved) {
            newNode->append(clone(cache, otherLayer, value));
        }
        break;
    }
    case ColumnId::FeatureIds: {
        auto resolved = otherLayer->resolve<FeatureId>(*otherNode);
        auto newNode = newFeatureId(resolved->typeId(), resolved->keyValuePairs());
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
        }
        break;
    }
    case ColumnId::ValidityCollections: {
        auto resolved = otherLayer->resolve<MultiValidity>(*otherNode);
        auto newNode = newValidityCollection(resolved->size());
        newCacheNode = newNode;
        for (auto value : *resolved) {
            newNode->append(resolve<Validity>(*clone(cache, otherLayer, value)));
        }
        break;
    }
    case ColumnId::AttributeLayers: {
        auto resolved = otherLayer->resolve<AttributeLayer>(*otherNode);
        auto newNode = newAttributeLayer(resolved->size());
        newCacheNode = newNode;
        for (auto [key, value] : resolved->fields()) {
            if (auto keyStr = otherLayer->strings()->resolve(key)) {
                auto cloned = clone(cache, otherLayer, value);
                newNode->addField(*keyStr, resolve<Attribute>(*cloned));
            }
        }
        break;
    }
    case ColumnId::AttributeLayerLists: {
        auto resolved = otherLayer->resolve<AttributeLayerList>(*otherNode);
        auto newNode = newAttributeLayers(resolved->size());
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
    case ColumnId::SourceDataReferenceCollections: {
        auto resolved = otherLayer->resolve<SourceDataReferenceCollection>(*otherNode);
        auto items = std::vector<QualifiedSourceDataReference>(
            otherLayer->impl_->sourceDataReferences_.begin() + resolved->offset_,
            otherLayer->impl_->sourceDataReferences_.begin() + resolved->offset_ + resolved->size_);
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
    case ColumnId::SourceDataReferences:
    case ColumnId::ValidityPoints:
        raiseFmt("Encountered unexpected column type {} in clone().", otherNode->addr().column());
    default: {
        newCacheNode = resolve(otherNode->addr());
    }
    }
    cache.insert({otherNode->addr().value_, newCacheNode});
    return newCacheNode;
}

void TileFeatureLayer::clone(
    std::unordered_map<uint32_t, ModelNode::Ptr>& clonedModelNodes,
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
        for (auto const& [key, value] : attrLayers->fields()) {
            if (auto keyStr = otherLayer->strings()->resolve(key)) {
                baseAttrLayers->addLayer(*keyStr, resolve<AttributeLayer>(*lookupOrClone(value)));
            }
        }
    }

    // Adopt geometries
    if (auto geom = otherFeature.geomOrNull()) {
        auto baseGeom = cloneTarget->geom();
        geom->forEachGeometry(
            [this, &baseGeom, &lookupOrClone](auto&& geomElement)
            {
                baseGeom->addGeometry(
                    resolve<Geometry>(*lookupOrClone(geomElement)));
                return true;
            });
    }

    // Adopt relations
    if (otherFeature.numRelations()) {
        otherFeature.forEachRelation(
            [this, &cloneTarget, &lookupOrClone](auto&& rel)
            {
                auto newRel = resolve<Relation>(*lookupOrClone(rel));
                cloneTarget->addRelation(newRel);
                return true;
            });
    }
}

Geometry::Storage& TileFeatureLayer::vertexBufferStorage()
{
    return impl_->pointBuffers_;
}

model_ptr<Feature> TileFeatureLayer::find(const std::string_view& featureId) const
{
    using namespace std::ranges;
    auto tokensRange = featureId | views::split('.');
    auto tokens = std::vector<decltype(*tokensRange.begin())>(tokensRange.begin(), tokensRange.end());

    if (tokens.empty()) {
        return {};
    }
    auto tokenAt = [&tokens](auto&& i) {
        return std::string_view(&*tokens[i].begin(), distance(tokens[i]));
    };

    auto typeInfo = layerInfo_->getTypeInfo(tokenAt(0), false);
    if (!typeInfo || typeInfo->uniqueIdCompositions_.empty())
        return {};

    // Convert the part strings to key-value pairs using the first (primary) ID composition.
    KeyValuePairs kvPairs;
    for (auto withOptionalParts : {true, false}) {
        size_t tokenIndex = 1;
        bool error = false;
        kvPairs.clear();

        for (const auto& part : typeInfo->uniqueIdCompositions_[0]) {
            if (part.isOptional_ && !withOptionalParts)
                continue;

            if (tokenIndex >= tokens.size()) {
                error = true;
                break;
            }

            std::variant<int64_t, std::string> parsedValue = std::string(tokenAt(tokenIndex++));
            if (!part.validate(parsedValue)) {
                error = true;
                break;
            }

            kvPairs.emplace_back(part.idPartLabel_, parsedValue);
        }

        if (tokenIndex < tokens.size()) {
            error = true;
        }

        if (error) {
            if (!withOptionalParts)
                return {};
            // Go on to try without optional parts.
        }
    }

    return find(typeInfo->name_, kvPairs);
}

}
