#include "attr.h"
#include "featurelayer.h"

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
                    self.pool_);
            });
    if (data_.validity_.value_)
        fields_.emplace_back(
            Fields::ValidityStr,
            [](Attribute const& self) {
                return model_ptr<simfil::ModelNode>::make(self.pool_, self.data_.validity_);
            });
}

model_ptr<Geometry> Attribute::validity()
{
    return pool().resolveGeometry(model_ptr<simfil::ModelNode>::make(pool_, data_.validity_));
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
    if (auto s = pool().fieldNames()->resolve(data_.name_))
        return *s;
    throw std::runtime_error("Attribute name is not known to string pool.");
}

}