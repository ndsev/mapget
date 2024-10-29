#include "relation.h"
#include "featurelayer.h"
#include "mapget/log.h"
#include "simfil/model/nodes.h"

namespace mapget
{

Relation::Relation(Relation::Data* data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(std::move(l), a), data_(data)
{
}

model_ptr<ValidityCollection> Relation::sourceValidities(bool createIfMissing)
{
    if (data_->sourceValidities_) {
        return sourceValidities();
    }
    if (createIfMissing) {
        auto returnValue = model().newValidityCollection(1);
        data_->sourceValidities_ = returnValue->addr();
        return returnValue;
    }
    return {};
}

model_ptr<ValidityCollection> Relation::sourceValidities() const
{
    if (!data_->sourceValidities_)
        return {};
    return model().resolveValidityCollection(*model_ptr<simfil::ModelNode>::make(model_, data_->sourceValidities_));
}

void Relation::setSourceValidities(const model_ptr<ValidityCollection>& validityColl)
{
    data_->sourceValidities_ = validityColl->addr();
}

model_ptr<ValidityCollection> Relation::targetValidities(bool createIfMissing)
{
    if (data_->targetValidities_) {
        return targetValidities();
    }
    if (createIfMissing) {
        auto returnValue = model().newValidityCollection(1);
        data_->targetValidities_ = returnValue->addr();
        return returnValue;
    }
    return {};
}


model_ptr<ValidityCollection> Relation::targetValidities() const
{
    if (!data_->targetValidities_)
        return {};
    return model().resolveValidityCollection(*model_ptr<simfil::ModelNode>::make(model_, data_->targetValidities_));
}

void Relation::setTargetValidities(const model_ptr<ValidityCollection>& validityColl)
{
    data_->targetValidities_ = validityColl->addr();
}

std::string_view Relation::name() const
{
    if (auto s = model().strings()->resolve(data_->name_))
        return *s;
    raise("Relation name is not known to string pool.");
}

model_ptr<FeatureId> Relation::target() const
{
    return model().resolveFeatureId(*model_ptr<simfil::ModelNode>::make(model_, data_->targetFeatureId_));
}

model_ptr<SourceDataReferenceCollection> Relation::sourceDataReferences() const
{
    if (data_->sourceData_)
        return model().resolveSourceDataReferenceCollection(*model_ptr<simfil::ModelNode>::make(model_, data_->sourceData_));
    return {};
}

void Relation::setSourceDataReferences(simfil::ModelNode::Ptr const& addresses)
{
    data_->sourceData_ = addresses->addr();
}

simfil::ValueType Relation::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr Relation::at(int64_t i) const
{
    return get(keyAt(i));
}

uint32_t Relation::size() const
{
    return 2 + (data_->sourceValidities_ || data_->targetValidities_ ? 2 : 0);
}

simfil::ModelNode::Ptr Relation::get(const simfil::StringId& f) const
{
    switch (f) {
    case StringPool::NameStr: // name
        return model_ptr<simfil::ValueNode>::make(name(), model().shared_from_this());
    case StringPool::TargetStr: // target
        return ModelNode::Ptr::make(model().shared_from_this(), data_->targetFeatureId_);
    case StringPool::SourceValiditiesStr: // source validity
        return ModelNode::Ptr::make(model().shared_from_this(), data_->sourceValidities_);
    case StringPool::TargetValiditiesStr: // target validity
        return ModelNode::Ptr::make(model().shared_from_this(), data_->targetValidities_);
    case StringPool::SourceDataStr: // source data
        return ModelNode::Ptr::make(model().shared_from_this(), data_->sourceData_);
    default:
        return {};
    }
}

simfil::StringId Relation::keyAt(int64_t i) const
{
    switch (i) {
    case 0: return StringPool::NameStr;
    case 1: return StringPool::TargetStr;
    case 2: return StringPool::SourceValiditiesStr;
    case 3: return StringPool::TargetValiditiesStr;
    case 4: return StringPool::SourceDataStr;
    default:
        return {};
    }
}

bool Relation::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    return std::all_of(begin(), end(), [&cb](auto&& child){return cb(*child);});
}

}
