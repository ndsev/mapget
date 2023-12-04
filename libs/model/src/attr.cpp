#include "attr.h"
#include "featurelayer.h"
#include "mapget/log.h"

namespace mapget
{

Attribute::Attribute(Attribute::Data& data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a)
    : simfil::ProceduralObject<2, Attribute>(data.fields_, std::move(l), a), data_(data)
{
    if (data_.direction_)
        fields_.emplace_back(
            Fields::DirectionStr,
            [](Attribute const& self) {
                return model_ptr<simfil::ValueNode>::make(
                    (int64_t)self.data_.direction_,
                    self.model_);
            });
    if (data_.validity_.value_)
        fields_.emplace_back(
            Fields::ValidityStr,
            [](Attribute const& self) {
                return model_ptr<simfil::ModelNode>::make(self.model_, self.data_.validity_);
            });
}

model_ptr<Geometry> Attribute::validity() const
{
    if (!hasValidity())
        throw std::runtime_error("Attempt to access null validity.");
    return model().resolveGeometry(model_ptr<simfil::ModelNode>::make(model_, data_.validity_));
}

bool Attribute::hasValidity() const
{
    return data_.validity_.value_ != 0;
}

void Attribute::setValidity(const model_ptr<Geometry>& validityGeom)
{
    data_.validity_ = validityGeom->addr();
}

void Attribute::setDirection(const Attribute::Direction& v)
{
    data_.direction_ = v;
}

Attribute::Direction Attribute::direction() const
{
    return data_.direction_;
}

std::string_view Attribute::name()
{
    if (auto s = model().fieldNames()->resolve(data_.name_))
        return *s;
    throw logRuntimeError("Attribute name is not known to string pool.");
}

}