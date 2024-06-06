#include "featurelayer.h"
#include "mapget/log.h"

#include <map>
#include <shared_mutex>

#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>

#include "simfil/model/bitsery-traits.h"
#include <sstream>

namespace mapget
{

struct TileFeatureLayer::Impl {
    simfil::ModelNodeAddress featureIdPrefix_;

    sfl::segmented_vector<Feature::Data, simfil::detail::ColumnPageSize/4> features_;
    sfl::segmented_vector<Attribute::Data, simfil::detail::ColumnPageSize> attributes_;
    sfl::segmented_vector<FeatureId::Data, simfil::detail::ColumnPageSize/2> featureIds_;
    sfl::segmented_vector<simfil::ArrayIndex, simfil::detail::ColumnPageSize/2> attrLayers_;
    sfl::segmented_vector<simfil::ArrayIndex, simfil::detail::ColumnPageSize/2> attrLayerLists_;
    sfl::segmented_vector<Relation::Data, simfil::detail::ColumnPageSize/2> relations_;

    /**
     * Indexing of features by their id hash. The hash-feature pairs are kept
     * in a vector, which is kept in a sorted state. This allows finding a
     * feature by it's id in O(log(n)) time.
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

    // Simfil execution Environment for this tile's string pool.
    std::unique_ptr<simfil::Environment> simfilEnv_;

    // Compiled simfil expressions, by hash of expression string
    std::map<std::string, simfil::ExprPtr, std::less<>> simfilExpressions_;

    // Mutex to manage access to the expression cache
    std::shared_mutex expressionCacheLock_;

    // (De-)Serialization
    template<typename S>
    void readWrite(S& s) {
        constexpr size_t maxColumnSize = std::numeric_limits<uint32_t>::max();
        s.container(features_, maxColumnSize);
        s.container(attributes_, maxColumnSize);
        s.container(featureIds_, maxColumnSize);
        s.container(attrLayers_, maxColumnSize);
        s.container(attrLayerLists_, maxColumnSize);
        s.object(featureIdPrefix_);
        s.container(relations_, maxColumnSize);
        sortFeatureHashIndex();
        s.container(featureHashIndex_, maxColumnSize);
    }

    explicit Impl(std::shared_ptr<simfil::Fields> fieldDict)
        : simfilEnv_(std::make_unique<simfil::Environment>(std::move(fieldDict)))
    {
    }
};

TileFeatureLayer::TileFeatureLayer(
    TileId tileId,
    std::string const& nodeId,
    std::string const& mapId,
    std::shared_ptr<LayerInfo> const& layerInfo,
    std::shared_ptr<simfil::Fields> const& fields
) :
    simfil::ModelPool(fields),
    impl_(std::make_unique<Impl>(fields)),
    TileLayer(tileId, nodeId, mapId, layerInfo)
{
}

TileFeatureLayer::TileFeatureLayer(
    std::istream& inputStream,
    LayerInfoResolveFun const& layerInfoResolveFun,
    FieldNameResolveFun const& fieldNameResolveFun
) :
    TileLayer(inputStream, layerInfoResolveFun),
    simfil::ModelPool(fieldNameResolveFun(nodeId_)),
    impl_(std::make_unique<Impl>(fieldNameResolveFun(nodeId_)))
{
    bitsery::Deserializer<bitsery::InputStreamAdapter> s(inputStream);
    impl_->readWrite(s);
    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        throw logRuntimeError(fmt::format(
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

/**
 * Create a hash of the given feature-type-name + idParts combination.
 * We do not use std::hash, as it has different implementations on different
 * compilers. This hash must be stable across Emscripten/GCC/MSVC/etc.
 */
uint64_t hashFeatureId(const std::string_view& type, const KeyValueViewPairs& idParts)
{
    // Constants for the FNV-1a hash algorithm
    constexpr uint64_t FNV_prime = 1099511628211ULL;
    constexpr uint64_t offset_basis = 14695981039346656037ULL;

    // Lambda function for FNV-1a hash of a string view
    auto fnv1a_hash = [](std::string_view str) -> uint64_t {
        uint64_t hash = offset_basis;
        for (char c : str) {
            hash ^= static_cast<uint64_t>(c);
            hash *= FNV_prime;
        }
        return hash;
    };

    // Lambda function to hash int64_t values
    auto hash_int64 = [](int64_t value) -> uint64_t {
        uint64_t hash = offset_basis;
        for (size_t i = 0; i < sizeof(int64_t); ++i) {
            hash ^= (value & 0xff);
            hash *= FNV_prime;
            value >>= 8;
        }
        return hash;
    };

    // Begin hashFeatureId implementation
    uint64_t hash = fnv1a_hash(type);

    for (const auto& [key, value] : idParts) {
        // Combine the hash of the key
        hash ^= fnv1a_hash(key);
        hash *= FNV_prime;

        // Combine the hash of the value
        hash ^= std::visit([&](const auto& val) -> uint64_t {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, int64_t>) {
                return hash_int64(val);
            } else {
                return fnv1a_hash(val);
            }
        }, value);
        hash *= FNV_prime;
    }

    return hash;
}

}  // namespace

simfil::shared_model_ptr<Feature> TileFeatureLayer::newFeature(
    const std::string_view& typeId,
    const KeyValueViewPairs& featureIdParts)
{
    if (featureIdParts.empty()) {
        throw logRuntimeError("Tried to create an empty feature ID.");
    }

    uint32_t idPrefixLength = 0;
    if (auto const idPrefix = getIdPrefix())
        idPrefixLength = idPrefix->size();

    if (!layerInfo_->validFeatureId(typeId, featureIdParts, true, idPrefixLength)) {
        throw logRuntimeError(fmt::format(
            "Could not find a matching ID composition of type {} with parts {}.",
            typeId,
            idPartsToString(featureIdParts)));
    }

    auto featureIdIndex = impl_->featureIds_.size();
    auto featureIdObject = newObject(featureIdParts.size());
    impl_->featureIds_.emplace_back(FeatureId::Data{
        true,
        fieldNames()->emplace(typeId),
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
        simfil::ModelNodeAddress{FeatureIds, (uint32_t)featureIdIndex},
        simfil::ModelNodeAddress{Null, 0},
        simfil::ModelNodeAddress{Null, 0},
        simfil::ModelNodeAddress{Null, 0},
        simfil::ModelNodeAddress{Null, 0},
    });
    auto result = Feature(
        impl_->features_.back(),
        shared_from_this(),
        simfil::ModelNodeAddress{Features, (uint32_t)featureIndex});

    // Add feature hash index entry.
    auto const& primaryIdComposition = getPrimaryIdComposition(typeId);
    auto fullStrippedFeatureId = stripOptionalIdParts(result.id()->keyValuePairs(), primaryIdComposition);
    auto hash = hashFeatureId(typeId, fullStrippedFeatureId);
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
        throw logRuntimeError(fmt::format(
            "Could not find a matching ID composition of type {} with parts {}.",
            typeId,
            idPartsToString(featureIdParts)));
    }

    auto featureIdObject = newObject(featureIdParts.size());
    auto featureIdIndex = impl_->featureIds_.size();
    impl_->featureIds_.emplace_back(FeatureId::Data{
        false,
        fieldNames()->emplace(typeId),
        featureIdObject->addr()
    });
    for (auto const& [k, v] : featureIdParts) {
        auto&& kk = k;
        std::visit([&](auto&& x){
            featureIdObject->addField(kk, x);
        }, v);
    }
    return FeatureId(impl_->featureIds_.back(), shared_from_this(), {FeatureIds, (uint32_t)featureIdIndex});
}

model_ptr<Relation>
TileFeatureLayer::newRelation(const std::string_view& name, const model_ptr<FeatureId>& target)
{
    auto relationIndex = impl_->relations_.size();
    impl_->relations_.emplace_back(Relation::Data{
        fieldNames()->emplace(name),
        target->addr()
    });
    return Relation(&impl_->relations_.back(), shared_from_this(), {Relations, (uint32_t)relationIndex});
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
        Attribute::Empty,
        {Null, 0},
        objectMemberStorage().new_array(initialCapacity),
        fieldNames()->emplace(name)
    });
    return Attribute(
        &impl_->attributes_.back(),
        shared_from_this(),
        {Attributes, (uint32_t)attrIndex});
}

model_ptr<AttributeLayer> TileFeatureLayer::newAttributeLayer(size_t initialCapacity)
{
    auto layerIndex = impl_->attrLayers_.size();
    impl_->attrLayers_.emplace_back(objectMemberStorage().new_array(initialCapacity));
    return AttributeLayer(
        impl_->attrLayers_.back(),
        shared_from_this(),
        {AttributeLayers, (uint32_t)layerIndex});
}

model_ptr<AttributeLayerList> TileFeatureLayer::newAttributeLayers(size_t initialCapacity)
{
    auto listIndex = impl_->attrLayerLists_.size();
    impl_->attrLayerLists_.emplace_back(objectMemberStorage().new_array(initialCapacity));
    return AttributeLayerList(
        impl_->attrLayerLists_.back(),
        shared_from_this(),
        {AttributeLayerLists, (uint32_t)listIndex});
}

model_ptr<AttributeLayer> TileFeatureLayer::resolveAttributeLayer(simfil::ModelNode const& n) const
{
    if (n.addr().column() != AttributeLayers)
        throw logRuntimeError("Cannot cast this node to an AttributeLayer.");
    return AttributeLayer(
        impl_->attrLayers_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<AttributeLayerList> TileFeatureLayer::resolveAttributeLayerList(simfil::ModelNode const& n) const
{
    if (n.addr().column() != AttributeLayerLists)
        throw logRuntimeError("Cannot cast this node to an AttributeLayerList.");
    return AttributeLayerList(
        impl_->attrLayerLists_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<Attribute> TileFeatureLayer::resolveAttribute(simfil::ModelNode const& n) const
{
    if (n.addr().column() != Attributes)
        throw logRuntimeError("Cannot cast this node to an Attribute.");
    return Attribute(
        &impl_->attributes_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<Feature> TileFeatureLayer::resolveFeature(simfil::ModelNode const& n) const
{
    if (n.addr().column() != Features)
        throw logRuntimeError("Cannot cast this node to a Feature.");
    return Feature(
        impl_->features_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<FeatureId> TileFeatureLayer::resolveFeatureId(simfil::ModelNode const& n) const
{
    if (n.addr().column() != FeatureIds)
        throw logRuntimeError("Cannot cast this node to a FeatureId.");
    return FeatureId(
        impl_->featureIds_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<Relation> TileFeatureLayer::resolveRelation(const simfil::ModelNode& n) const
{
    if (n.addr().column() != Relations)
        throw logRuntimeError("Cannot cast this node to a Relation.");
    return Relation(
        &impl_->relations_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

void TileFeatureLayer::resolve(const simfil::ModelNode& n, const simfil::Model::ResolveFn& cb) const
{
    switch (n.addr().column())
    {
    case Features: {
        cb(*resolveFeature(n));
        return;
    }
    case FeatureProperties: {
        cb(Feature::FeaturePropertyView(
            impl_->features_[n.addr().index()],
            shared_from_this(),
            n.addr()
        ));
        return;
    }
    case FeatureIds: {
        cb(*resolveFeatureId(n));
        return;
    }
    case Attributes: {
        cb(*resolveAttribute(n));
        return;
    }
    case AttributeLayers: {
        cb(*resolveAttributeLayer(n));
        return;
    }
    case AttributeLayerLists: {
        cb(*resolveAttributeLayerList(n));
        return;
    }
    case Relations: {
        cb(*resolveRelation(n));
        return;
    }
    }

    simfil::ModelPool::resolve(n, cb);
}

simfil::Environment& TileFeatureLayer::evaluationEnvironment()
{
    return *impl_->simfilEnv_;
}

simfil::ExprPtr const& TileFeatureLayer::compiledExpression(const std::string_view& expr)
{
    std::shared_lock sharedLock(impl_->expressionCacheLock_);
    auto it = impl_->simfilExpressions_.find(expr);
    if (it != impl_->simfilExpressions_.end()) {
        return it->second;
    }
    sharedLock.unlock();
    std::unique_lock uniqueLock(impl_->expressionCacheLock_);
    auto [newIt, _] = impl_->simfilExpressions_.emplace(
        std::string(expr),
        simfil::compile(*impl_->simfilEnv_, expr, false)
    );
    return newIt->second;
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
            if (!IdPart::idPartsMatchComposition(candidateComposition, 0, prefix, prefix.size(), false)) {
                throw logRuntimeError(fmt::format(
                    "Prefix not compatible with an id composite in type: {}",
                    featureType.name_));
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

nlohmann::json TileFeatureLayer::toGeoJson() const
{
    auto features = nlohmann::json::array();
    for (auto f : *this)
        features.push_back(f->toGeoJson());
    return nlohmann::json::object({
        {"type", "FeatureCollection"},
        {"features", features},
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
    auto hash = hashFeatureId(type, queryIdPartsStripped);

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
        throw logRuntimeError(fmt::format("Could not find feature type {}", typeId));
    }
    if (typeIt->uniqueIdCompositions_.empty()) {
        throw logRuntimeError(fmt::format("No composition for feature type {}!", typeId));
    }
    return typeIt->uniqueIdCompositions_.front();
}

void TileFeatureLayer::setFieldNames(std::shared_ptr<simfil::Fields> const& newDict)
{
    // Re-map old field IDs to new field IDs
    for (auto& attr : impl_->attributes_) {
        if (auto resolvedName = fieldNames()->resolve(attr.name_)) {
            attr.name_ = newDict->emplace(*resolvedName);
        }
    }
    for (auto& fid : impl_->featureIds_) {
        if (auto resolvedName = fieldNames()->resolve(fid.typeId_)) {
            fid.typeId_ = newDict->emplace(*resolvedName);
        }
    }
    for (auto& rel : impl_->relations_) {
        if (auto resolvedName = fieldNames()->resolve(rel.name_)) {
            rel.name_ = newDict->emplace(*resolvedName);
        }
    }

    // Reset simfil environment and clear expression cache
    {
        std::unique_lock lock(impl_->expressionCacheLock_);
        impl_->simfilExpressions_.clear();
        impl_->simfilEnv_ = std::make_unique<simfil::Environment>(newDict);
    }

    ModelPool::setFieldNames(newDict);
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
            if (auto keyStr = otherLayer->fieldNames()->resolve(key)) {
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
    case Geometries:
    case Points:
    case PointBuffers: {
        auto resolved = otherLayer->resolveGeometry(otherNode);
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
    case GeometryCollections: {
        auto resolved = otherLayer->resolveGeometryCollection(otherNode);
        auto newNode = newGeometryCollection(resolved->numGeometries());
        newCacheNode = newNode;
        resolved->forEachGeometry(
            [this, &newNode, &cache, &otherLayer](auto&& geom)
            {
                newNode->addGeometry(resolveGeometry(clone(cache, otherLayer, geom)));
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
    case Features:
    case FeatureProperties: {
        throw logRuntimeError("Cannot clone entire feature yet.");
    }
    case FeatureIds: {
        auto resolved = otherLayer->resolveFeatureId(*otherNode);
        auto newNode = newFeatureId(resolved->typeId(), resolved->keyValuePairs());
        newCacheNode = newNode;
        break;
    }
    case Attributes: {
        auto resolved = otherLayer->resolveAttribute(*otherNode);
        auto newNode = newAttribute(resolved->name());
        newCacheNode = newNode;
        if (resolved->hasValidity()) {
            newNode->setValidity(resolveGeometry(clone(cache, otherLayer, resolved->validity())));
        }
        newNode->setDirection(resolved->direction());
        resolved->forEachField(
            [this, &newNode, &cache, &otherLayer](auto&& key, auto&& value)
            {
                newNode->addField(key, clone(cache, otherLayer, value));
                return true;
            });
        break;
    }
    case AttributeLayers: {
        auto resolved = otherLayer->resolveAttributeLayer(*otherNode);
        auto newNode = newAttributeLayer(resolved->size());
        newCacheNode = newNode;
        for (auto [key, value] : resolved->fields()) {
            if (auto keyStr = otherLayer->fieldNames()->resolve(key)) {
                newNode->addField(*keyStr, clone(cache, otherLayer, value));
            }
        }
        break;
    }
    case AttributeLayerLists: {
        auto resolved = otherLayer->resolveAttributeLayerList(*otherNode);
        auto newNode = newAttributeLayers(resolved->size());
        newCacheNode = newNode;
        for (auto [key, value] : resolved->fields()) {
            if (auto keyStr = otherLayer->fieldNames()->resolve(key)) {
                newNode->addField(*keyStr, clone(cache, otherLayer, value));
            }
        }
        break;
    }
    case Relations: {
        auto resolved = otherLayer->resolveRelation(*otherNode);
        auto newNode = newRelation(
            resolved->name(),
            resolveFeatureId(*clone(cache, otherLayer, resolved->target())));
        newCacheNode = newNode;
        break;
    }
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
        if (getIdPrefix() && idParts.size() >= getIdPrefix()->size()) {
            idParts = KeyValueViewPairs(
                idParts.begin()+getIdPrefix()->size(), idParts.end());
        }
        cloneTarget = newFeature(type, idParts);
    }

    auto lookupOrClone =
        [&](simfil::ModelNode::Ptr const& n) -> simfil::ModelNode::Ptr
    {
        return clone(clonedModelNodes, otherLayer, n);
    };

    // Adopt attributes
    if (auto attrs = otherFeature.attributes()) {
        auto baseAttrs = cloneTarget->attributes();
        for (auto const& [key, value] : attrs->fields()) {
            if (auto keyStr = otherLayer->fieldNames()->resolve(key)) {
                baseAttrs->addField(*keyStr, lookupOrClone(value));
            }
        }
    }

    // Adopt attribute layers
    if (auto attrLayers = otherFeature.attributeLayers()) {
        auto baseAttrLayers = cloneTarget->attributeLayers();
        for (auto const& [key, value] : attrLayers->fields()) {
            if (auto keyStr = otherLayer->fieldNames()->resolve(key)) {
                baseAttrLayers->addField(*keyStr, lookupOrClone(value));
            }
        }
    }

    // Adopt geometries
    if (auto geom = otherFeature.geom()) {
        auto baseGeom = cloneTarget->geom();
        geom->forEachGeometry(
            [this, &baseGeom, &lookupOrClone](auto&& geomElement)
            {
                baseGeom->addGeometry(
                    resolveGeometry(lookupOrClone(geomElement)));
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

}
