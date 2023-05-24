#include "tileid.h"
#include <cmath>
#include <algorithm>

namespace mapget
{

TileId::TileId(uint16_t x, uint16_t y, uint16_t z) {
    value_ = ((uint64_t)x << 32) | ((uint64_t)y << 16) | z;
}

TileId::TileId(uint64_t value) : value_(value) {
    // No extra work needed here as we directly assign the given value
}

uint16_t TileId::x() const {
    return (uint16_t) (value_ >> 32);
}

uint16_t TileId::y() const {
    return (uint16_t) ((value_ >> 16) & 0xFFFF);
}

uint16_t TileId::z() const {
    return (uint16_t) (value_ & 0xFFFF);
}

bool TileId::operator== (TileId const& other) const {
    return value_ == other.value_;
}

bool TileId::operator< (TileId const& other) const {
    return value_ < other.value_;
}

TileId TileId::fromWgs84(double longitude, double latitude, uint16_t zoomLevel)
{
    longitude = fmod(longitude, 360.) + 180.;
    if (longitude < 0.)
        longitude += 360.;

    // Calculate the number of subdivisions
    auto numCols = static_cast<uint16_t>(1 << (zoomLevel + 1));
    auto numRows = static_cast<uint16_t>(1 << zoomLevel);

    // Convert to grid coordinates
    auto x = static_cast<int64_t>((longitude / 360.) * numCols);
    auto y = static_cast<int64_t>(((90. - latitude) / 180.) * numRows);

    x %= numCols;
    y %= numRows * 2;
    y = std::abs(y);  // -y: Crossing the North Pole

    // We have crossed the South Pole
    if (y >= numRows)
        y = numRows - (y - numRows) - 1;

    return {(uint16_t)x, (uint16_t)y, zoomLevel};
}

} // namespace mapget
