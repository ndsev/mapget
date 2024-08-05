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

model_ptr<Geometry> Relation::sourceValidity() const
{
    if (!hasSourceValidity())
        throw std::runtime_error("Attempt to access null validity.");
    return model().resolveGeometry(*model_ptr<simfil::ModelNode>::make(model_, data_->sourceValidity_));
}

bool Relation::hasSourceValidity() const
{
    return data_->sourceValidity_;
}

void Relation::setSourceValidity(const model_ptr<Geometry>& validityGeom)
{
    data_->sourceValidity_ = validityGeom->addr();
}

model_ptr<Geometry> Relation::targetValidity() const
{
    if (!hasTargetValidity())
        throw std::runtime_error("Attempt to access null validity.");
    return model().resolveGeometry(*model_ptr<simfil::ModelNode>::make(model_, data_->targetValidity_));
}

bool Relation::hasTargetValidity() const
{
    return data_->targetValidity_;
}

void Relation::setTargetValidity(const model_ptr<Geometry>& validityGeom)
{
    data_->targetValidity_ = validityGeom->addr();
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
    return model().resolveSourceDataReferenceCollection(*model_ptr<simfil::ModelNode>::make(model_, data_->sourceData_));
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
    return 2 + (data_->sourceValidity_ || data_->targetValidity_ ? 2 : 0);
}

simfil::ModelNode::Ptr Relation::get(const simfil::StringId& f) const
{
    switch (f) {
    case StringPool::NameStr: // name
        return model_ptr<simfil::ValueNode>::make(name(), model().shared_from_this());
    case StringPool::TargetStr: // target
        return ModelNode::Ptr::make(model().shared_from_this(), data_->targetFeatureId_);
    case StringPool::SourceValidityStr: // source validity
        return ModelNode::Ptr::make(model().shared_from_this(), data_->sourceValidity_);
    case StringPool::TargetValidityStr: // target validity
        return ModelNode::Ptr::make(model().shared_from_this(), data_->targetValidity_);
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
    case 2: return StringPool::SourceValidityStr;
    case 3: return StringPool::TargetValidityStr;
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
