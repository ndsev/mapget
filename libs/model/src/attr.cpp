#include "attr.h"
#include "featurelayer.h"
#include "mapget/log.h"

namespace mapget
{

Attribute::Attribute(Attribute::Data* data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a)
    : simfil::ProceduralObject<2, Attribute>(data->fields_, std::move(l), a), data_(data)
{
    if (data_->validity_)
        fields_.emplace_back(
            StringPool::ValidityStr,
            [](Attribute const& self) {
                return model_ptr<simfil::ModelNode>::make(self.model_, self.data_->validity_);
            });
}

model_ptr<Geometry> Attribute::validity() const
{
    if (!hasValidity())
        throw std::runtime_error("Attempt to access null validity.");
    // TODO: We could remove this cast by passing the ModelPool through ProceduralObject->Object->...
    auto& layer = dynamic_cast<TileFeatureLayer&>(model());
    return layer.resolveGeometry(*simfil::ModelNode::Ptr::make(model_, data_->validity_));
}

bool Attribute::hasValidity() const
{
    return data_->validity_;
}

void Attribute::setValidity(const model_ptr<Geometry>& validityGeom)
{
    data_->validity_ = validityGeom->addr();
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
        auto& layer = dynamic_cast<TileFeatureLayer&>(model());
        return layer.resolveSourceDataReferenceCollection(*model_ptr<simfil::ModelNode>::make(model_, data_->sourceDataRefs_));
    }
    return {};
}

void Attribute::setSourceDataReferences(simfil::ModelNode::Ptr const& node)
{
    data_->sourceDataRefs_ = node->addr();
}

}
