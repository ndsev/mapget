#include "attr.h"
#include "featurelayer.h"
#include "mapget/log.h"

namespace mapget
{

namespace {
std::string_view directionToString(Attribute::Direction const& d) {
    switch (d) {
    case Attribute::Empty: return "EMPTY";
    case Attribute::Positive: return "POSITIVE";
    case Attribute::Negative: return "NEGATIVE";
    case Attribute::Both: return "BOTH";
    case Attribute::None: return "NONE";
    }
    return "?";
}
}

Attribute::Attribute(Attribute::Data* data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a)
    : simfil::ProceduralObject<2, Attribute>(data->fields_, std::move(l), a), data_(data)
{
    if (data_->direction_)
        fields_.emplace_back(
            Fields::DirectionStr,
            [](Attribute const& self) {
                return model_ptr<simfil::ValueNode>::make(
                    directionToString(self.data_->direction_),
                    self.model_);
            });
    if (data_->validity_)
        fields_.emplace_back(
            Fields::ValidityStr,
            [](Attribute const& self) {
                return model_ptr<simfil::ModelNode>::make(self.model_, self.data_->validity_);
            });
}

model_ptr<Geometry> Attribute::validity() const
{
    if (!hasValidity())
        throw std::runtime_error("Attempt to access null validity.");
    return model().resolveGeometry(model_ptr<simfil::ModelNode>::make(model_, data_->validity_));
}

bool Attribute::hasValidity() const
{
    return data_->validity_;
}

void Attribute::setValidity(const model_ptr<Geometry>& validityGeom)
{
    data_->validity_ = validityGeom->addr();
}

void Attribute::setDirection(const Attribute::Direction& v)
{
    data_->direction_ = v;
}

Attribute::Direction Attribute::direction() const
{
    return data_->direction_;
}

std::string_view Attribute::name() const
{
    if (auto s = model().fieldNames()->resolve(data_->name_))
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

        if (auto ks = model().fieldNames()->resolve(key)) {
            if (!cb(*ks, value))
                return false;
        }
    }
    return true;
}

}