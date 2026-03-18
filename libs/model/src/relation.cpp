#include "relation.h"
#include "featurelayer.h"
#include "mapget/log.h"
#include "simfil/model/nodes.h"

namespace mapget
{

namespace
{
simfil::ModelNode::Ptr exposedValidityNode(
    TileFeatureLayer const& model,
    simfil::ModelNodeAddress const& validityCollectionAddress)
{
    auto validities = model.resolve<MultiValidity>(validityCollectionAddress);
    if (validities && validities->size() == 1) {
        if (auto validity = validities->at(0)) {
            return validity;
        }
    }
    return model.resolve(validityCollectionAddress);
}
}

Relation::Relation(Relation::Data* data,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::ProceduralObject<6, Relation, TileFeatureLayer>(std::move(l), a, key),
      data_(data)
{
    fields_.emplace_back(
        StringPool::NameStr,
        [](Relation const& self) {
            return model_ptr<simfil::ValueNode>::make(self.name(), self.model().shared_from_this());
        });
    if (data_->targetFeatureId_)
        fields_.emplace_back(
            StringPool::TargetStr,
            [](Relation const& self) {
                return self.model().resolve(self.data_->targetFeatureId_);
            });
    if (data_->sourceValidity_)
        fields_.emplace_back(
            StringPool::SourceValidityStr,
            [](Relation const& self) {
                return exposedValidityNode(self.model(), self.data_->sourceValidity_);
            });
    if (data_->targetValidity_)
        fields_.emplace_back(
            StringPool::TargetValidityStr,
            [](Relation const& self) {
                return exposedValidityNode(self.model(), self.data_->targetValidity_);
            });
    if (data_->sourceData_)
        fields_.emplace_back(
            StringPool::SourceDataStr,
            [](Relation const& self) {
                return self.model().resolve(self.data_->sourceData_);
            });
}

model_ptr<MultiValidity> Relation::sourceValidity()
{
    if (data_->sourceValidity_) {
        return sourceValidityOrNull();
    }
    auto returnValue = model().newValidityCollection(2);
    data_->sourceValidity_ = returnValue->addr();
    return returnValue;
}

model_ptr<MultiValidity> Relation::sourceValidityOrNull() const
{
    if (!data_->sourceValidity_)
        return {};
    return model().resolve<MultiValidity>(
        *model_ptr<simfil::ModelNode>::make(model_, data_->sourceValidity_));
}

void Relation::setSourceValidity(const model_ptr<MultiValidity>& validityGeom)
{
    data_->sourceValidity_ = validityGeom ? validityGeom->addr() : ModelNodeAddress();
}

model_ptr<MultiValidity> Relation::targetValidity()
{
    if (data_->targetValidity_) {
        return targetValidityOrNull();
    }
    auto returnValue = model().newValidityCollection(2);
    data_->targetValidity_ = returnValue->addr();
    return returnValue;
}


model_ptr<MultiValidity> Relation::targetValidityOrNull() const
{
    if (!data_->targetValidity_)
        return {};
    return model().resolve<MultiValidity>(
        *model_ptr<simfil::ModelNode>::make(model_, data_->targetValidity_));
}

void Relation::setTargetValidity(const model_ptr<MultiValidity>& validityGeom)
{
    data_->targetValidity_ = validityGeom ? validityGeom->addr() : ModelNodeAddress();;
}

std::string_view Relation::name() const
{
    if (auto s = model().strings()->resolve(data_->name_))
        return *s;
    raise("Relation name is not known to string pool.");
}

model_ptr<FeatureId> Relation::target() const
{
    return model().resolve<FeatureId>(
        *model_ptr<simfil::ModelNode>::make(model_, data_->targetFeatureId_));
}

model_ptr<SourceDataReferenceCollection> Relation::sourceDataReferences() const
{
    if (data_->sourceData_)
        return model().resolve<SourceDataReferenceCollection>(
            *model_ptr<simfil::ModelNode>::make(model_, data_->sourceData_));
    return {};
}

void Relation::setSourceDataReferences(simfil::ModelNode::Ptr const& addresses)
{
    data_->sourceData_ = addresses->addr();
}

}
