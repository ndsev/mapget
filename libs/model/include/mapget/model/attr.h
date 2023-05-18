#pragma once

#include "featureid.h"

namespace mapget
{

class Attribute : public simfil::ProceduralObject<2>
{
    friend class TileFeatureLayer;

public:
    /// Attribute direction values - may be used as flags.
    enum Direction : uint8_t {
        Empty = 0x0,
        Positive = 0x1,
        Negative = 0x2,
        Both = 0x3,
        None = 0x3,
    };

    /// Attribute direction accessors
    [[nodiscard]] Direction direction() const;
    void setDirection(Direction const& v);

    /// Attribute validity accessor
    model_ptr<Geometry> validity();

protected:

    struct Data {
        Direction direction_ = Empty;
        simfil::ModelNodeAddress validity_;
        simfil::ModelNodeAddress fields_;
    };

    Attribute(Data& data, FeatureLayerConstPtr l, simfil::ModelNodeAddress a);

    Data& data_;
};

}