#pragma once

#include "featureid.h"

namespace mapget
{

/**
 * Represents a feature attribute which belongs to an
 * AttributeLayer, and may have typed `direction` and some
 * `validity` fields in addition to other arbitrary object fields.
 */
class Attribute : public simfil::ProceduralObject<2, Attribute>
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

    /** Actual per-attribute data that is stored in the model's attributes-column. */
    struct Data {
        Direction direction_ = Empty;
        simfil::ModelNodeAddress validity_;
        simfil::ArrayIndex fields_ = -1;
        simfil::FieldId name_ = 0;

        template<typename S>
        void serialize(S& s) {
            s.value1b(direction_);
            s.value4b(validity_.value_);
            s.value4b(fields_);
            s.value2b(name_);
        }
    };

    Attribute(Data& data, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);

    /** Reference to the actual stored data for the attribute. */
    Data& data_;
};

}