#include "attr.h"
#include "featurelayer.h"
#include "mapget/log.h"

namespace mapget
{

model_ptr<simfil::OverlayNode> Attribute::queryContext(
    model_ptr<Feature> const& feature,
    std::string_view layerName,
    uint32_t attributeIndex,
    uint32_t validityIndex,
    uint32_t validityCount) const
{
    if (!feature || &feature->model() != &model())
        throw std::invalid_argument(
            "Attribute query context requires a feature from the same layer.");
    auto result = model_ptr<simfil::OverlayNode>::make(simfil::Value::field(*this));
    result->set(StringPool::OverlayNameStr, simfil::Value::make(std::string(name())));
    result->set(StringPool::OverlayFeatureStr, simfil::Value::field(*feature));
    result->set(StringPool::OverlayLayerStr, simfil::Value::make(std::string(layerName)));
    result->set(StringPool::OverlayAttributeIndexStr, simfil::Value::make(int64_t(attributeIndex)));
    result->set(StringPool::OverlayValidityIndexStr, simfil::Value::make(int64_t(validityIndex)));
    // An attribute without explicit validity still has one whole-feature evaluation context.
    result->set(
        StringPool::OverlayValidityCountStr,
        simfil::Value::make(int64_t(std::max(1u, validityCount))));
    result->set(StringPool::OverlayHasValidityStr, simfil::Value::make(validityCount != 0));
    return result;
}

namespace
{
simfil::ModelNode::Ptr exposedValidityNode(
    PartitionFeatureLayer const& model,
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

Attribute::Attribute(
    Attribute::Data* data,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::ProceduralObject<2, Attribute, PartitionFeatureLayer>(
          data->fields_,
          std::move(l),
          a,
          key),
      data_(data)
{
    if (data_->validities_)
        fields_.emplace_back(
            StringPool::ValidityStr,
            [](Attribute const& self) {
                return exposedValidityNode(self.model(), self.data_->validities_);
            });
}

std::string_view Attribute::name() const
{
    if (auto s = model().strings()->resolve(data_->name_))
        return *s;
    raise("Attribute name is not known to string pool.");
}

bool Attribute::forEachField(
    std::function<bool(std::string_view const& k, simfil::ModelNode::Ptr const& val)> const& cb)
    const
{
    if (!cb)
        return false;
    auto numExtraFields = fields_.size();
    for (auto const& [key, value] : fields()) {
        if (numExtraFields) {
            // Skip the procedural fields.
            --numExtraFields;
            continue;
        }

        if (auto ks = model().strings()->resolve(key)) {
            if (!cb(*ks, value))
                return false;
        }
    }
    return true;
}

model_ptr<SourceDataReferenceCollection> Attribute::sourceDataReferences() const
{
    if (data_->sourceDataRefs_) {
        return model().resolve<SourceDataReferenceCollection>(
            *model_ptr<simfil::ModelNode>::make(model_, data_->sourceDataRefs_));
    }
    return {};
}

void Attribute::setSourceDataReferences(simfil::ModelNode::Ptr const& node)
{
    data_->sourceDataRefs_ = node->addr();
}

model_ptr<MultiValidity> Attribute::validity()
{
    if (auto returnValue = validityOrNull()) {
        return returnValue;
    }
    auto returnValue = model().newValidityCollection(2);
    data_->validities_ = returnValue->addr();
    return returnValue;
}

model_ptr<MultiValidity> Attribute::validityOrNull() const
{
    if (!data_->validities_) {
        return {};
    }
    return model().resolve<MultiValidity>(data_->validities_);
}

void Attribute::setValidity(const model_ptr<MultiValidity>& validities) const
{
    data_->validities_ = validities ? validities->addr() : ModelNodeAddress();;
}

}
