#include "tileid.h"

namespace mapget
{

// Constructor to initialize TileId with x, y, z
TileId::TileId(uint16_t x, uint16_t y, uint16_t z) {
    value_ = ((uint64_t)x << 32) | ((uint64_t)y << 16) | z;
}

// Constructor to initialize TileId with value
TileId::TileId(uint64_t const& value) : value_(value) {
    // No extra work needed here as we directly assign the given value
}

// Function to get x component of the TileId
uint16_t TileId::x() const {
    return (uint16_t) (value_ >> 32);
}

// Function to get y component of the TileId
uint16_t TileId::y() const {
    return (uint16_t) ((value_ >> 16) & 0xFFFF);
}

// Function to get z component of the TileId (zoom level)
uint16_t TileId::z() const {
    return (uint16_t) (value_ & 0xFFFF);
}

// Operator overload for equality comparison
bool TileId::operator== (TileId const& other) const {
    return value_ == other.value_;
}

// Operator overload for less than comparison
bool TileId::operator< (TileId const& other) const {
    return value_ < other.value_;
}

} // namespace mapget
