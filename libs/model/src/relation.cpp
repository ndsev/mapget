#include "relation.h"
#include "featurelayer.h"
#include "mapget/log.h"

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
    return model().resolveGeometry(model_ptr<simfil::ModelNode>::make(model_, data_->sourceValidity_));
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
    return model().resolveGeometry(model_ptr<simfil::ModelNode>::make(model_, data_->targetValidity_));
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
    if (auto s = model().fieldNames()->resolve(data_->name_))
        return *s;
    throw logRuntimeError("Relation name is not known to string pool.");
}

model_ptr<FeatureId> Relation::target() const
{
    return model().resolveFeatureId(*model_ptr<simfil::ModelNode>::make(model_, data_->targetFeatureId_));
}

simfil::ValueType Relation::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr Relation::at(int64_t i) const
{
    switch (i) {
    case 0: // name
        return model_ptr<simfil::ValueNode>::make(name(), model().shared_from_this());
    case 1: // target
        return ModelNode::Ptr::make(model().shared_from_this(), data_->targetFeatureId_);
    case 2: // source validity
        return ModelNode::Ptr::make(model().shared_from_this(), data_->sourceValidity_);
    case 3: // target validity
        return ModelNode::Ptr::make(model().shared_from_this(), data_->targetValidity_);
    default:
        return {};
    }
}

uint32_t Relation::size() const
{
    return 2 + (data_->sourceValidity_ || data_->targetValidity_ ? 2 : 0);
}

simfil::ModelNode::Ptr Relation::get(const simfil::FieldId& f) const
{
    switch (f) {
    case Fields::NameStr: // name
        return model_ptr<simfil::ValueNode>::make(name(), model().shared_from_this());
    case Fields::TargetStr: // target
        return ModelNode::Ptr::make(model().shared_from_this(), data_->targetFeatureId_);
    case Fields::SourceValidityStr: // source validity
        return ModelNode::Ptr::make(model().shared_from_this(), data_->sourceValidity_);
    case Fields::TargetValidityStr: // target validity
        return ModelNode::Ptr::make(model().shared_from_this(), data_->targetValidity_);
    default:
        return {};
    }
}

simfil::FieldId Relation::keyAt(int64_t i) const
{
    switch (i) {
    case 0: return Fields::NameStr;
    case 1: return Fields::TargetStr;
    case 2: return Fields::SourceValidityStr;
    case 3: return Fields::TargetValidityStr;
    default:
        return {};
    }
}

bool Relation::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    return std::all_of(begin(), end(), [&cb](auto&& child){return cb(*child);});
}

}