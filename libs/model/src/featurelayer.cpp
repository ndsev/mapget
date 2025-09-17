#include "featurelayer.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>
#include "sfl/segmented_vector.hpp"

#include "simfil/environment.h"
#include "simfil/model/arena.h"
#include "simfil/model/bitsery-traits.h"
#include "simfil/model/nodes.h"
#include "mapget/log.h"
#include "simfil/model/string-pool.h"
#include "simfilutil.h"
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

template <typename S>
void serialize(S& s, mapget::Point& v) {
    s.value8b(v.x);
    s.value8b(v.y);
    s.value8b(v.z);
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
}

namespace mapget
{

struct TileFeatureLayer::Impl {
    simfil::ModelNodeAddress featureIdPrefix_;

    sfl::segmented_vector<Feature::Data, simfil::detail::ColumnPageSize/4> features_;
    sfl::segmented_vector<Attribute::Data, simfil::detail::ColumnPageSize> attributes_;
    sfl::segmented_vector<Validity::Data, simfil::detail::ColumnPageSize> validities_;
    sfl::segmented_vector<FeatureId::Data, simfil::detail::ColumnPageSize/2> featureIds_;
    sfl::segmented_vector<simfil::ArrayIndex, simfil::detail::ColumnPageSize/2> attrLayers_;
    sfl::segmented_vector<simfil::ArrayIndex, simfil::detail::ColumnPageSize/2> attrLayerLists_;
    sfl::segmented_vector<Relation::Data, simfil::detail::ColumnPageSize/2> relations_;
    sfl::segmented_vector<Geometry::Data, simfil::detail::ColumnPageSize/2> geom_;
    sfl::segmented_vector<QualifiedSourceDataReference, simfil::detail::ColumnPageSize/2> sourceDataReferences_;
    Geometry::Storage pointBuffers_;

    /**
     * Indexing of features by their id hash. The hash-feature pairs are kept
     * in a vector, which is kept in a sorted state. This allows finding a
     * feature by its id in O(log(n)) time.
     */
    struct FeatureAddrWithIdHash
    {
        simfil::ModelNodeAddress featureAddr_;
        uint64_t idHash_ = 0;

        template<class S>
        void serialize(S& s) {
            s.object(featureAddr_);
            s.value8b(idHash_);
        }

        bool operator< (FeatureAddrWithIdHash const& other) const {
            return std::tie(idHash_, featureAddr_) < std::tie(other.idHash_, other.featureAddr_);
        }
    };
    sfl::segmented_vector<FeatureAddrWithIdHash, simfil::detail::ColumnPageSize/4> featureHashIndex_;
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
        constexpr size_t maxColumnSize = std::numeric_limits<uint32_t>::max();
        s.container(features_, maxColumnSize);
        s.container(attributes_, maxColumnSize);
        s.container(validities_, maxColumnSize);
        s.container(featureIds_, maxColumnSize);
        s.container(attrLayers_, maxColumnSize);
        s.container(attrLayerLists_, maxColumnSize);
        s.object(featureIdPrefix_);
        s.container(relations_, maxColumnSize);
        sortFeatureHashIndex();
        s.container(featureHashIndex_, maxColumnSize);
        s.container(geom_, maxColumnSize);
        s.ext(pointBuffers_, bitsery::ext::ArrayArenaExt{});
        s.container(sourceDataReferences_, maxColumnSize);
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
    std::istream& inputStream,
    LayerInfoResolveFun const& layerInfoResolveFun,
    StringPoolResolveFun const& stringPoolGetter
) :
    TileLayer(inputStream, layerInfoResolveFun),
    ModelPool(stringPoolGetter(nodeId_)),
    impl_(std::make_unique<Impl>(stringPoolGetter(nodeId_)))
{
    bitsery::Deserializer<bitsery::InputStreamAdapter> s(inputStream);
    impl_->readWrite(s);
    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raise(fmt::format(
            "Failed to read TileFeatureLayer: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error())));
    }
    ModelPool::read(inputStream);
}

TileFeatureLayer::~TileFeatureLayer() = default;

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
    impl_->featureIds_.emplace_back(FeatureId::Data{
        true,
        strings()->emplace(typeId),
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
        simfil::ModelNodeAddress{ColumnId::FeatureIds, (uint32_t)featureIdIndex},
        simfil::ModelNodeAddress{Null, 0},
        simfil::ModelNodeAddress{Null, 0},
        simfil::ModelNodeAddress{Null, 0},
        simfil::ModelNodeAddress{Null, 0},
    });
    auto result = Feature(
        impl_->features_.back(),
        shared_from_this(),
        simfil::ModelNodeAddress{ColumnId::Features, (uint32_t)featureIndex});

    // Add feature hash index entry.
    auto const& primaryIdComposition = getPrimaryIdComposition(typeId);
    auto fullStrippedFeatureId = stripOptionalIdParts(result.id()->keyValuePairs(), primaryIdComposition);
    auto hash = Hash().mix(typeId).mix(fullStrippedFeatureId).value();
    impl_->featureHashIndex_.emplace_back(TileFeatureLayer::Impl::FeatureAddrWithIdHash{result.addr(), hash});
    impl_->featureHashIndexNeedsSorting_ = true;

    // Note: Here we rely on the assertion that the root_ collection
    // contains only references to feature nodes, in the order
    // of the feature node column.
    addRoot(simfil::ModelNode::Ptr(result));
    setInfo("num-features", numRoots());
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
    impl_->featureIds_.emplace_back(FeatureId::Data{
        false,
        strings()->emplace(typeId),
        featureIdObject->addr()
    });
    for (auto const& [k, v] : featureIdParts) {
        auto&& kk = k;
        std::visit([&](auto&& x){
            featureIdObject->addField(kk, x);
        }, v);
    }
    return FeatureId(impl_->featureIds_.back(), shared_from_this(), {ColumnId::FeatureIds, (uint32_t)featureIdIndex});
}

model_ptr<Relation>
TileFeatureLayer::newRelation(const std::string_view& name, const model_ptr<FeatureId>& target)
{
    auto relationIndex = impl_->relations_.size();
    impl_->relations_.emplace_back(Relation::Data{
        strings()->emplace(name),
        target->addr()
    });
    return Relation(&impl_->relations_.back(), shared_from_this(), {ColumnId::Relations, (uint32_t)relationIndex});
}

model_ptr<Object> TileFeatureLayer::getIdPrefix()
{
    if (impl_->featureIdPrefix_)
        return resolveObject(simfil::ModelNode::Ptr::make(shared_from_this(), impl_->featureIdPrefix_));
    return {};
}

model_ptr<Attribute>
TileFeatureLayer::newAttribute(const std::string_view& name, size_t initialCapacity)
{
    auto attrIndex = impl_->attributes_.size();
    impl_->attributes_.emplace_back(Attribute::Data{
        {Null, 0},
        objectMemberStorage().new_array(initialCapacity),
        strings()->emplace(name)
    });
    return Attribute(
        &impl_->attributes_.back(),
        shared_from_this(),
        {ColumnId::Attributes, (uint32_t)attrIndex});
}

model_ptr<AttributeLayer> TileFeatureLayer::newAttributeLayer(size_t initialCapacity)
{
    auto layerIndex = impl_->attrLayers_.size();
    impl_->attrLayers_.emplace_back(objectMemberStorage().new_array(initialCapacity));
    return AttributeLayer(
        impl_->attrLayers_.back(),
        shared_from_this(),
        {ColumnId::AttributeLayers, (uint32_t)layerIndex});
}

model_ptr<AttributeLayerList> TileFeatureLayer::newAttributeLayers(size_t initialCapacity)
{
    auto listIndex = impl_->attrLayerLists_.size();
    impl_->attrLayerLists_.emplace_back(objectMemberStorage().new_array(initialCapacity));
    return AttributeLayerList(
        impl_->attrLayerLists_.back(),
        shared_from_this(),
        {ColumnId::AttributeLayerLists, (uint32_t)listIndex});
}

model_ptr<GeometryCollection> TileFeatureLayer::newGeometryCollection(size_t initialCapacity)
{
    auto listIndex = arrayMemberStorage().new_array(initialCapacity);
    return GeometryCollection(
        shared_from_this(),
        {ColumnId::GeometryCollections, (uint32_t)listIndex});
}

model_ptr<Geometry> TileFeatureLayer::newGeometry(GeomType geomType, size_t initialCapacity)
{
    initialCapacity = std::max((size_t)1, initialCapacity);
    impl_->geom_.emplace_back(geomType, initialCapacity);
    return Geometry(
        &impl_->geom_.back(),
        shared_from_this(),
        {ColumnId::Geometries, (uint32_t)impl_->geom_.size() - 1});
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
        {ColumnId::Geometries, (uint32_t)impl_->geom_.size() - 1});
}

model_ptr<SourceDataReferenceCollection> TileFeatureLayer::newSourceDataReferenceCollection(std::span<QualifiedSourceDataReference> list)
{
    auto& arena = impl_->sourceDataReferences_;
    const auto index = arena.size();
    const auto size = list.size();

    arena.insert(arena.end(), list.begin(), list.end());

    return {
    SourceDataReferenceCollection(index, size, shared_from_this(),
        ModelNodeAddress(ColumnId::SourceDataReferenceCollections, sourceDataAddressListToModelAddress(index, size)))};
}

model_ptr<Validity> TileFeatureLayer::newValidity()
{
    impl_->validities_.emplace_back();
    return Validity(
        &impl_->validities_.back(),
        shared_from_this(),
        {ColumnId::Validities, (uint32_t)impl_->validities_.size() - 1});
}

model_ptr<MultiValidity> TileFeatureLayer::newValidityCollection(size_t initialCapacity)
{
    auto validityArrId = arrayMemberStorage().new_array(initialCapacity);
    return MultiValidity(
        shared_from_this(),
        {ColumnId::ValidityCollections, (uint32_t)validityArrId});
}

model_ptr<AttributeLayer> TileFeatureLayer::resolveAttributeLayer(simfil::ModelNode const& n) const
{
    if (n.addr().column() != ColumnId::AttributeLayers)
        raise("Cannot cast this node to an AttributeLayer.");
    return AttributeLayer(
        impl_->attrLayers_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<AttributeLayerList> TileFeatureLayer::resolveAttributeLayerList(simfil::ModelNode const& n) const
{
    if (n.addr().column() != ColumnId::AttributeLayerLists)
        raise("Cannot cast this node to an AttributeLayerList.");
    return AttributeLayerList(
        impl_->attrLayerLists_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<Attribute> TileFeatureLayer::resolveAttribute(simfil::ModelNode const& n) const
{
    if (n.addr().column() != ColumnId::Attributes)
        raise("Cannot cast this node to an Attribute.");
    return Attribute(
        &impl_->attributes_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<Feature> TileFeatureLayer::resolveFeature(simfil::ModelNode const& n) const
{
    if (n.addr().column() != ColumnId::Features)
        raise("Cannot cast this node to a Feature.");
    return Feature(
        impl_->features_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<FeatureId> TileFeatureLayer::resolveFeatureId(simfil::ModelNode const& n) const
{
    if (n.addr().column() != ColumnId::FeatureIds)
        raise("Cannot cast this node to a FeatureId.");
    return FeatureId(
        impl_->featureIds_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<Relation> TileFeatureLayer::resolveRelation(const simfil::ModelNode& n) const
{
    if (n.addr().column() != ColumnId::Relations)
        raise("Cannot cast this node to a Relation.");
    return Relation(
        &impl_->relations_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<PointNode> TileFeatureLayer::resolvePoint(const simfil::ModelNode& n) const
{
    if (n.addr().column() != ColumnId::Points)
        raise("Cannot cast this node to a Point.");
    return PointNode(
        n, &impl_->geom_.at(n.addr().index()));
}

model_ptr<PointNode> TileFeatureLayer::resolveValidityPoint(const simfil::ModelNode& n) const
{
    if (n.addr().column() != ColumnId::ValidityPoints)
        raise("Cannot cast this node to a ValidityPoint.");
    return PointNode(
        n, &impl_->validities_.at(n.addr().index()));
}

model_ptr<Validity> TileFeatureLayer::resolveValidity(simfil::ModelNode const& n) const
{
    if (n.addr().column() != ColumnId::Validities)
        raise("Cannot cast this node to a Validity.");
    return Validity(
        &impl_->validities_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<MultiValidity> TileFeatureLayer::resolveValidityCollection(const simfil::ModelNode& n) const
{
    if (n.addr().column() != ColumnId::ValidityCollections)
        raise("Cannot cast this node to a ValidityCollection.");
    return MultiValidity(
        shared_from_this(),
        n.addr());
}

model_ptr<PointBufferNode> TileFeatureLayer::resolvePointBuffer(const simfil::ModelNode& n) const
{
    return PointBufferNode(
        &impl_->geom_.at(n.addr().index()),
        shared_from_this(),
        n.addr());
}

model_ptr<PolygonNode> TileFeatureLayer::resolvePolygon(const simfil::ModelNode& n) const
{
    return PolygonNode(
        shared_from_this(),
        n.addr());
}

model_ptr<MeshNode> TileFeatureLayer::resolveMesh(const simfil::ModelNode& n) const
{
    return MeshNode(
        &impl_->geom_.at(n.addr().index()),
        shared_from_this(),
        n.addr());
}

model_ptr<LinearRingNode> TileFeatureLayer::resolveLinearRing(const simfil::ModelNode& n) const
{
    return LinearRingNode(n);
}

model_ptr<Geometry> TileFeatureLayer::resolveGeometry(const simfil::ModelNode& n) const
{
    return Geometry(
        &const_cast<Geometry::Data&>(impl_->geom_.at(n.addr().index())), // FIXME: const_cast?!
        shared_from_this(),
        n.addr());
}

model_ptr<LinearRingNode> TileFeatureLayer::resolveMeshTriangleLinearRing(const simfil::ModelNode& n) const
{
    return LinearRingNode(n, 3);
}

model_ptr<MeshTriangleCollectionNode> TileFeatureLayer::resolveMeshTriangleCollection(const simfil::ModelNode& n) const
{
    return MeshTriangleCollectionNode(n);
}

model_ptr<GeometryCollection>
TileFeatureLayer::resolveGeometryCollection(const simfil::ModelNode& n) const
{
    return GeometryCollection(
        shared_from_this(), n.addr());
}

model_ptr<SourceDataReferenceCollection>
TileFeatureLayer::resolveSourceDataReferenceCollection(const simfil::ModelNode& n) const
{
    if (n.addr().column() != ColumnId::SourceDataReferenceCollections)
        raise("Cannot cast this node to an SourceDataReferenceCollection.");

    auto [index, size] = modelAddressToSourceDataAddressList(n.addr().index());
    const auto& data = impl_->sourceDataReferences_;
    return SourceDataReferenceCollection(index, size, shared_from_this(), n.addr());
}

model_ptr<SourceDataReferenceItem>
TileFeatureLayer::resolveSourceDataReferenceItem(const simfil::ModelNode& n) const
{
    if (n.addr().column() != ColumnId::SourceDataReferences)
        raise("Cannot cast this node to an SourceDataReferenceItem.");

    const auto* data = &impl_->sourceDataReferences_.at(n.addr().index());
    return SourceDataReferenceItem(data, shared_from_this(), n.addr());
}

void TileFeatureLayer::resolve(const simfil::ModelNode& n, const simfil::Model::ResolveFn& cb) const
{
    switch (n.addr().column())
    {
    case ColumnId::Features:
        return cb(*resolveFeature(n));
    case ColumnId::FeatureProperties:
        return cb(Feature::FeaturePropertyView(
            impl_->features_[n.addr().index()],
            shared_from_this(),
            n.addr()
        ));
    case ColumnId::FeatureIds:
        return cb(*resolveFeatureId(n));
    case ColumnId::Attributes:
        return cb(*resolveAttribute(n));
    case ColumnId::AttributeLayers:
        return cb(*resolveAttributeLayer(n));
    case ColumnId::AttributeLayerLists:
        return cb(*resolveAttributeLayerList(n));
    case ColumnId::Relations:
        return cb(*resolveRelation(n));
    case ColumnId::Points:
        return cb(*resolvePoint(n));
    case ColumnId::PointBuffers:
        return cb(*resolvePointBuffer(n));
    case ColumnId::Geometries:
        return cb(*resolveGeometry(n));
    case ColumnId::GeometryCollections:
        return cb(*resolveGeometryCollection(n));
    case ColumnId::Polygon:
        return cb(*resolvePolygon(n));
    case ColumnId::Mesh:
        return cb(*resolveMesh(n));
    case ColumnId::MeshTriangleCollection:
        return cb(*resolveMeshTriangleCollection(n));
    case ColumnId::MeshTriangleLinearRing:
        return cb(*resolveMeshTriangleLinearRing(n));
    case ColumnId::LinearRing:
        return cb(*resolveLinearRing(n));
    case ColumnId::SourceDataReferenceCollections:
        return cb(*resolveSourceDataReferenceCollection(n));
    case ColumnId::SourceDataReferences:
        return cb(*resolveSourceDataReferenceItem(n));
    case ColumnId::Validities:
        return cb(*resolveValidity(n));
    case ColumnId::ValidityPoints:
        return cb(*resolveValidityPoint(n));
    case ColumnId::ValidityCollections:
        return cb(*resolveValidityCollection(n));
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
    return evaluate(query, *root(0), anyMode, autoWildcard);
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

void TileFeatureLayer::write(std::ostream& outputStream)
{
    TileLayer::write(outputStream);
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);
    impl_->readWrite(s);
    ModelPool::write(outputStream);
}

nlohmann::json TileFeatureLayer::toJson() const
{
    auto features = nlohmann::json::array();
    for (auto f : *this)
        features.push_back(f->toJson());
    return nlohmann::json::object({
        {"type", "FeatureCollection"},
        {"features", features}
    });
}

size_t TileFeatureLayer::size() const
{
    return numRoots();
}

model_ptr<Feature> TileFeatureLayer::at(size_t i) const
{
    return resolveFeature(*root(i));
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
        Impl::FeatureAddrWithIdHash{0, hash},
        [](auto&& l, auto&& r) { return l.idHash_ < r.idHash_; });

    // Iterate through potential matches to handle hash collisions.
    while (it != impl_->featureHashIndex_.end() && it->idHash_ == hash)
    {
        auto feature = resolveFeature(*simfil::ModelNode::Ptr::make(shared_from_this(), it->featureAddr_));
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

void TileFeatureLayer::setStrings(std::shared_ptr<simfil::StringPool> const& newDict)
{
    auto oldDict = strings();
    // Reset simfil environment and clear expression cache
    impl_->expressionCache_.reset(makeEnvironment(newDict));
    ModelPool::setStrings(newDict);
    if (!oldDict || *newDict == *oldDict)
        return;

    // Re-map old string IDs to new string IDs
    for (auto& attr : impl_->attributes_) {
        if (auto resolvedName = oldDict->resolve(attr.name_)) {
            attr.name_ = newDict->emplace(*resolvedName);
        }
    }
    for (auto& validity : impl_->validities_) {
        if (auto resolvedName = strings()->resolve(validity.referencedGeomName_)) {
            validity.referencedGeomName_ = newDict->emplace(*resolvedName);
        }
    }
    for (auto& fid : impl_->featureIds_) {
        if (auto resolvedName = oldDict->resolve(fid.typeId_)) {
            fid.typeId_ = newDict->emplace(*resolvedName);
        }
    }
    for (auto& rel : impl_->relations_) {
        if (auto resolvedName = oldDict->resolve(rel.name_)) {
            rel.name_ = newDict->emplace(*resolvedName);
        }
    }
}

simfil::ModelNode::Ptr TileFeatureLayer::clone(
    std::unordered_map<uint32_t, simfil::ModelNode::Ptr>& cache,
    const TileFeatureLayer::Ptr& otherLayer,
    const simfil::ModelNode::Ptr& otherNode)
{
    auto it = cache.find(otherNode->addr().value_);
    if (it != cache.end()) {
        return it->second;
    }

    using namespace simfil;
    ModelNode::Ptr& newCacheNode = cache[otherNode->addr().value_];
    switch (otherNode->addr().column()) {
    case Objects: {
        auto resolved = otherLayer->resolveObject(otherNode);
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
        auto resolved = otherLayer->resolveArray(otherNode);
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
        auto resolved = otherLayer->resolveGeometry(*otherNode);
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
        auto resolved = otherLayer->resolveGeometryCollection(*otherNode);
        auto newNode = newGeometryCollection(resolved->numGeometries());
        newCacheNode = newNode;
        resolved->forEachGeometry(
            [this, &newNode, &cache, &otherLayer](auto&& geom)
            {
                newNode->addGeometry(resolveGeometry(*clone(cache, otherLayer, geom)));
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
    case ColumnId::FeatureIds: {
        auto resolved = otherLayer->resolveFeatureId(*otherNode);
        auto newNode = newFeatureId(resolved->typeId(), resolved->keyValuePairs());
        newCacheNode = newNode;
        break;
    }
    case ColumnId::Attributes: {
        auto resolved = otherLayer->resolveAttribute(*otherNode);
        auto newNode = newAttribute(resolved->name());
        newCacheNode = newNode;
        if (resolved->validityOrNull()) {
            newNode->setValidity(
                resolveValidityCollection(*clone(cache, otherLayer, resolved->validityOrNull())));
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
        auto resolved = otherLayer->resolveValidity(*otherNode);
        auto newNode = newValidity();
        newCacheNode = newNode;
        newNode->setDirection(resolved->direction());
        switch (resolved->geometryDescriptionType()) {
        case Validity::NoGeometry:
            break;
        case Validity::SimpleGeometry:
            newNode->setSimpleGeometry(resolveGeometry(*clone(cache, otherLayer, resolved->simpleGeometry())));
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
        auto resolved = otherLayer->resolveValidityCollection(*otherNode);
        auto newNode = newValidityCollection(resolved->size());
        newCacheNode = newNode;
        for (auto value : *resolved) {
            newNode->append(resolveValidity(*clone(cache, otherLayer, value)));
        }
        break;
    }
    case ColumnId::AttributeLayers: {
        auto resolved = otherLayer->resolveAttributeLayer(*otherNode);
        auto newNode = newAttributeLayer(resolved->size());
        newCacheNode = newNode;
        for (auto [key, value] : resolved->fields()) {
            if (auto keyStr = otherLayer->strings()->resolve(key)) {
                newNode->addField(*keyStr, clone(cache, otherLayer, value));
            }
        }
        break;
    }
    case ColumnId::AttributeLayerLists: {
        auto resolved = otherLayer->resolveAttributeLayerList(*otherNode);
        auto newNode = newAttributeLayers(resolved->size());
        newCacheNode = newNode;
        for (auto [key, value] : resolved->fields()) {
            if (auto keyStr = otherLayer->strings()->resolve(key)) {
                newNode->addField(*keyStr, clone(cache, otherLayer, value));
            }
        }
        break;
    }
    case ColumnId::Relations: {
        auto resolved = otherLayer->resolveRelation(*otherNode);
        auto newNode = newRelation(
            resolved->name(),
            resolveFeatureId(*clone(cache, otherLayer, resolved->target())));
        if (resolved->sourceValidityOrNull()) {
            newNode->setSourceValidity(resolveValidityCollection(
                *clone(cache, otherLayer, resolved->sourceValidityOrNull())));
        }
        if (resolved->targetValidityOrNull()) {
            newNode->setTargetValidity(resolveValidityCollection(
                *clone(cache, otherLayer, resolved->targetValidityOrNull())));
        }
        newCacheNode = newNode;
        break;
    }
    case ColumnId::SourceDataReferenceCollections: {
        auto resolved = otherLayer->resolveSourceDataReferenceCollection(*otherNode);
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
        newCacheNode = ModelNode::Ptr::make(shared_from_this(), otherNode->addr());
    }
    }
    cache.insert({otherNode->addr().value_, newCacheNode});
    return newCacheNode;
}

void TileFeatureLayer::clone(
    std::unordered_map<uint32_t, simfil::ModelNode::Ptr>& clonedModelNodes,
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
        [&](simfil::ModelNode::Ptr const& n) -> simfil::ModelNode::Ptr
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
                baseAttrLayers->addField(*keyStr, lookupOrClone(value));
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
                    resolveGeometry(*lookupOrClone(geomElement)));
                return true;
            });
    }

    // Adopt relations
    if (otherFeature.numRelations()) {
        otherFeature.forEachRelation(
            [this, &cloneTarget, &lookupOrClone](auto&& rel)
            {
                auto newRel = resolveRelation(*lookupOrClone(rel));
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
