#pragma once

#include "featureid.h"

namespace mapget
{

/**
 * Represents a feature attribute which belongs to an
 * AttributeLayer, and may have typed `direction` and some
 * `validity` fields in addition to other arbitrary object fields.
 */
class Attribute : public simfil::ProceduralObject<2>
{
    friend class TileFeatureLayer;

public:
    /**
     * Attribute direction values - may be used as flags.
     */
    enum Direction : uint8_t {
        Empty = 0x0,    // No set direction
        Positive = 0x1, // Positive (digitization) direction
        Negative = 0x2, // Negative (against digitization) direction
        Both = 0x3,     // Both positive and negative direction
        None = 0x4,     // Not in any direction
    };

    /**
     * Attribute direction accessors.
     */
    [[nodiscard]] Direction direction() const;
    void setDirection(Direction const& v);

    /**
     * Attribute validity accessor.
     */
    model_ptr<Geometry> validity();

    /**
     * Read-only attribute name accessor.
     */
    std::string_view name();

protected:

    struct Data {
        Direction direction_ = Empty;
        simfil::ModelNodeAddress validity_;
        simfil::ArrayIndex fields_ = -1;
        simfil::FieldId name_ = 0;
    };

    Attribute(Data& data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);

    Data& data_;
};

}