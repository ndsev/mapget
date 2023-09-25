#include "featurelayer.h"
#include "mapget/log.h"

#include <map>
#include <shared_mutex>

#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>

#include "simfil/model/bitsery-traits.h"

namespace mapget
{

struct TileFeatureLayer::Impl {
    simfil::ModelNodeAddress featureIdPrefix_;

    sfl::segmented_vector<Feature::Data, simfil::detail::ColumnPageSize/4> features_;
    sfl::segmented_vector<Attribute::Data, simfil::detail::ColumnPageSize> attributes_;
    sfl::segmented_vector<FeatureId::Data, simfil::detail::ColumnPageSize/2> featureIds_;
    sfl::segmented_vector<simfil::ArrayIndex, simfil::detail::ColumnPageSize/2> attrLayers_;
    sfl::segmented_vector<simfil::ArrayIndex, simfil::detail::ColumnPageSize/2> attrLayerLists_;

    // Simfil execution Environment for this tile's string pool.
    simfil::Environment simfilEnv_;

    // Compiled simfil expressions, by hash of expression string
    std::map<std::string, simfil::ExprPtr> simfilExpressions_;

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
        s.value4b(featureIdPrefix_.value_);
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
    const LayerInfoResolveFun& layerInfoResolveFun,
    const FieldNameResolveFun& fieldNameResolveFun
) :
    TileLayer(inputStream, layerInfoResolveFun),
    simfil::ModelPool(fieldNameResolveFun(nodeId_)),
    impl_(std::make_unique<Impl>(fieldNameResolveFun(nodeId_)))
{
    bitsery::Deserializer<bitsery::InputStreamAdapter> s(inputStream);
    impl_->readWrite(s);
    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        throw logRuntimeError(stx::format(
            "Failed to read TileFeatureLayer: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error())));
    }
    ModelPool::read(inputStream);
}

TileFeatureLayer::~TileFeatureLayer() = default;

/*
 * Checks if featureIdParts match one of the uniqueIdComposition specifications.
 * The order of values in KeyValuePairs must be the same as in the composition!
 */
bool TileFeatureLayer::validFeatureId(
    FeatureTypeInfo const& typeId,
    KeyValuePairs const& featureIdParts,
    bool includeTilePrefix) {

    auto featureIdIter = featureIdParts.begin();
    bool idCompositionMatches = true;
    bool prefixSkipped = false;

    if (!this->featureIdPrefix().has_value()) {
        // TODO define what to do in this case.
        // Must a feature ID prefix exist if includeTilePrefix is set to true?
        prefixSkipped = true;
    }

    if (!prefixSkipped && includeTilePrefix) {
        // TODO do featureIdPrefix and candidateComposition have the same structure?
        // Then create separate function for matching these to a list of key value pairs,
        // with option that it's ok to not exhaust key value pairs in case we're matching
        // featureIdPrefix.
        // Also need to pass the index from which to continue matching? Pointer to pointer?

        auto& nextKeyValuePair = *featureIdIter;

        for (auto const& [fieldNameId, fieldValue] :
             this->featureIdPrefix().value()->fields()) {

            if (featureIdIter == featureIdParts.end()) {
                // The feature ID should be longer than the tile prefix.
                return false;
            }

            // Check that the field name matches.
            auto fieldNameStr = *fieldNames()->resolve(fieldNameId);
            if (fieldNameStr != nextKeyValuePair.first) {
                return false;
            }

            // TODO validate value types.
            // Values are variants with one out of the predefined types:
            // check which one via getIndex()/index() call or something like that).
            // Also check that values in fields of type
            // {IdPartDataType::I32, "I32"},
            // {IdPartDataType::U32, "U32"},
            // are not bigger than the specified int size.
            // And that {IdPartDataType::UUID128, "UUID128"} is exactly 16 bytes.

            ++featureIdIter;
        }
    }

    for (auto& candidateComposition : typeId.uniqueIdCompositions_) {
        // TODO validate that it's intentional in FeatureLayer test
        //  that the tile prefix is a part of composite specification.

        for (auto& part : candidateComposition) {

            // Need a new iterator for each candidate composition we're checking.
            auto tempIdIterator = featureIdIter;
            auto& nextKeyValuePair = *tempIdIterator;

            if (tempIdIterator == featureIdParts.end()) {
                idCompositionMatches = false;
                break;
            }

            if (part.idPartLabel_ != nextKeyValuePair.first) {
                idCompositionMatches = false;
                break;
            }
            // TODO validate value types, see above.

            ++tempIdIterator;
        }

        if (idCompositionMatches && featureIdIter == featureIdParts.end()) {
            return true;
        }
    }

    return false;
}

simfil::shared_model_ptr<Feature> TileFeatureLayer::newFeature(
    const std::string_view& typeId,
    const KeyValuePairs& featureIdParts)
{
    if (featureIdParts.empty()) {
        // TODO throw exception and document said exception.
    }

    for (auto& type : this->layerInfo_->featureTypes_) {
        if (type.name_ != typeId) {
            continue;
        }

        if (!validFeatureId(type, featureIdParts, false)) {
            throw logRuntimeError("Could not find a matching ID composition.");
        }
        break;
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

    // Note: Here we rely on the assertion that the root_ collection
    // contains only references to feature nodes, in the order
    // of the feature node column.
    addRoot(simfil::ModelNode::Ptr(result));
    return result;
}

model_ptr<FeatureId>
TileFeatureLayer::newFeatureId(
    const std::string_view& typeId,
    const KeyValuePairs& featureIdParts)
{
    // TODO: Validate ID parts
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

std::optional<model_ptr<Object>> TileFeatureLayer::featureIdPrefix()
{
    if (impl_->featureIdPrefix_.value_)
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
        impl_->attributes_.back(),
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
        impl_->attributes_[n.addr().index()],
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
    std::string exprString{expr};
    auto it = impl_->simfilExpressions_.find(exprString);
    if (it != impl_->simfilExpressions_.end()) {
        return it->second;
    }
    sharedLock.unlock();
    std::unique_lock uniqueLock(impl_->expressionCacheLock_);
    auto [newIt, _] = impl_->simfilExpressions_.emplace(
        std::move(exprString),
        simfil::compile(impl_->simfilEnv_, expr, false)
    );
    return newIt->second;
}

void TileFeatureLayer::setPrefix(const KeyValuePairs& prefix)
{
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

}
