#include "attr.h"
#include "featurelayer.h"
#include "mapget/log.h"

namespace mapget
{

Attribute::Attribute(Attribute::Data* data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a)
    : simfil::ProceduralObject<2, Attribute, TileFeatureLayer>(data->fields_, std::move(l), a), data_(data)
{
    if (data_->validities_)
        fields_.emplace_back(
            StringPool::ValidityStr,
            [](Attribute const& self) {
                return model_ptr<simfil::ModelNode>::make(self.model_, self.data_->validities_);
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
        return model().resolveSourceDataReferenceCollection(*model_ptr<simfil::ModelNode>::make(model_, data_->sourceDataRefs_));
    }
    return {};
}

void Attribute::setSourceDataReferences(simfil::ModelNode::Ptr const& node)
{
    data_->sourceDataRefs_ = node->addr();
}

model_ptr<ValidityCollection> Attribute::validities(bool createIfMissing)
{
    if (auto returnValue = validities()) {
        return returnValue;
    }
    if (createIfMissing) {
        auto returnValue = model().newValidityCollection(1);
        data_->validities_ = returnValue->addr();
        return returnValue;
    }
    return {};
}

model_ptr<ValidityCollection> Attribute::validities() const
{
    if (!data_->validities_) {
        return {};
    }
    return model().resolveValidityCollection(*ModelNode::Ptr::make(model_, data_->validities_));
}

void Attribute::setValidities(const model_ptr<ValidityCollection>& validities) const
{
    data_->validities_ = validities->addr();
}

}
