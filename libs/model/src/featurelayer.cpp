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
    simfil::Environment simfilEnv_;

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

    explicit Impl(std::shared_ptr<Fields> fields)
        : simfilEnv_(std::move(fields))
    {
    }
};

TileFeatureLayer::TileFeatureLayer(
    TileId tileId,
    std::string const& nodeId,
    std::string const& mapId,
    std::shared_ptr<LayerInfo> const& layerInfo,
    std::shared_ptr<Fields> const& fields
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
 * Check that starting from a given index, the parts of an id composition
 * match the featureIdParts segment from start for the given length.
 */
bool idPartsMatchComposition(
    std::vector<IdPart> const& candidateComposition,
    uint32_t compositionMatchStartIdx,
    KeyValueViewPairs const& featureIdParts,
    unsigned long matchLength)
{
    auto featureIdIter = featureIdParts.begin();
    auto compositionIter = candidateComposition.begin();

    while (compositionMatchStartIdx > 0) {
        ++compositionIter;
        --compositionMatchStartIdx;
    }

    while (matchLength > 0 && compositionIter != candidateComposition.end()) {
        // Have we exhausted feature ID parts while there's still composition parts?
        if (featureIdIter == featureIdParts.end()) {
            return false;
        }

        auto& [idPartKey, idPartValue] = *featureIdIter;

        // Does this ID part's field name match?
        if (compositionIter->idPartLabel_ != idPartKey) {
            return false;
        }

        // Does the ID part's value match?
        auto& compositionDataType = compositionIter->datatype_;

        if (std::holds_alternative<int64_t>(idPartValue)) {
            auto value = std::get<int64_t>(idPartValue);
            switch (compositionDataType) {
            case IdPartDataType::I32:
                // Value must fit an I32.
                if (value < INT32_MIN || value > INT32_MAX) {
                    return false;
                }
                break;
            case IdPartDataType::U32:
                if (value < 0 || value > UINT32_MAX) {
                    return false;
                }
                break;
            case IdPartDataType::U64:
                if (value < 0 || value > UINT64_MAX) {
                    return false;
                }
                break;
            default:;
            }
        }
        else if (std::holds_alternative<std::string_view>(idPartValue)) {
            auto value = std::get<std::string_view>(idPartValue);
            // UUID128 should be a 128 bit sequence.
            if (compositionDataType == IdPartDataType::UUID128 && value.size() != 16) {
                return false;
            }
        }
        else {
            throw logRuntimeError("Id part data type not supported!");
        }

        ++featureIdIter;
        ++compositionIter;
        --matchLength;
    }

    // Match means we either checked the required length, or all the values.
    return matchLength == 0;
}

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

bool TileFeatureLayer::validFeatureId(
    const std::string_view& typeId,
    KeyValueViewPairs const& featureIdParts,
    bool validateForNewFeature)
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

    for (auto& candidateComposition : typeIt->uniqueIdCompositions_) {
        uint32_t compositionMatchStartIndex = 0;
        if (validateForNewFeature && this->featureIdPrefix()) {
            // Iterate past the prefix in the unique id composition.
            compositionMatchStartIndex = this->featureIdPrefix()->size();
        }

        if (idPartsMatchComposition(
                candidateComposition,
                compositionMatchStartIndex,
                featureIdParts,
                featureIdParts.size()))
        {
            return true;
        }

        // References may use alternative ID compositions,
        // but the feature itself must always use the first one.
        if (validateForNewFeature)
            return false;
    }

    return false;
}

simfil::shared_model_ptr<Feature> TileFeatureLayer::newFeature(
    const std::string_view& typeId,
    const KeyValueViewPairs& featureIdParts)
{
    if (featureIdParts.empty()) {
        throw logRuntimeError("Tried to create an empty feature ID.");
    }

    if (!validFeatureId(typeId, featureIdParts, true)) {
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
    impl_->featureHashIndex_.emplace_back(result.addr(), hash);
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
    if (!validFeatureId(typeId, featureIdParts, false)) {
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

model_ptr<Object> TileFeatureLayer::featureIdPrefix()
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
    return AttributeLayer(
        impl_->attrLayers_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<AttributeLayerList> TileFeatureLayer::resolveAttributeLayerList(simfil::ModelNode const& n) const
{
    return AttributeLayerList(
        impl_->attrLayerLists_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<Attribute> TileFeatureLayer::resolveAttribute(simfil::ModelNode const& n) const
{
    return Attribute(
        &impl_->attributes_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<Feature> TileFeatureLayer::resolveFeature(simfil::ModelNode const& n) const
{
    return Feature(
        impl_->features_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<FeatureId> TileFeatureLayer::resolveFeatureId(simfil::ModelNode const& n) const
{
    return FeatureId(
        impl_->featureIds_[n.addr().index()],
        shared_from_this(),
        n.addr());
}

model_ptr<Relation> TileFeatureLayer::resolveRelation(const simfil::ModelNode& n) const
{
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
    return impl_->simfilEnv_;
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
        simfil::compile(impl_->simfilEnv_, expr, false)
    );
    return newIt->second;
}

void TileFeatureLayer::setPrefix(const KeyValueViewPairs& prefix)
{
    // The prefix must be set, before any feature is added.
    if (impl_->features_.size() > 0)
        throw std::runtime_error("Cannot set feature id prefix after a feature was added.");

    // Check that the prefix is compatible with all primary id composites.
    // The primary id composition is the first one in the list.
    for (auto& featureType : this->layerInfo_->featureTypes_) {
        for (auto& candidateComposition : featureType.uniqueIdCompositions_) {
            if (!idPartsMatchComposition(candidateComposition, 0, prefix, prefix.size())) {
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
    // Convert KeyValuePairs to KeyValuePairsView
    KeyValueViewPairs kvpView;
    for (auto const& [k, v] : queryIdParts) {
        std::visit([&kvpView, &k](auto&& vv){
            if constexpr (std::is_same_v<std::decay_t<decltype(vv)>, std::string>)
                kvpView.emplace_back(k, std::string_view(vv));
            else
                kvpView.emplace_back(k, vv);
        }, v);
    }
    return find(type, kvpView);
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
    auto compositionIt = typeIt->uniqueIdCompositions_.begin();
    if (compositionIt == typeIt->uniqueIdCompositions_.end()) {
        throw logRuntimeError(fmt::format("No composition for feature type {}!", typeId));
    }
    return *compositionIt;
}

}
