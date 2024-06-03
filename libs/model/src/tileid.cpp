#include "tileid.h"
#include <cmath>
#include <algorithm>

namespace mapget
{

// Constants for the range of latitude and longitude
constexpr auto MIN_LON = -180.0;
constexpr auto MAX_LON = 180.0;
constexpr auto MIN_LAT = -90.0;
constexpr auto MAX_LAT = 90.0;
constexpr auto LON_EXTENT = MAX_LON - MIN_LON;
constexpr auto LAT_EXTENT = MAX_LAT - MIN_LAT;

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
    zoomLevel = std::min((uint16_t)62, zoomLevel);

    longitude = fmod(longitude, LON_EXTENT) + MAX_LON;
    if (longitude < 0.)
        longitude += LON_EXTENT;

    // Calculate the number of subdivisions
    auto numCols = static_cast<int64_t>(1ull << (zoomLevel + 1));
    auto numRows = static_cast<int64_t>(1ull << zoomLevel);

    // Convert to grid coordinates
    auto x = static_cast<int64_t>((longitude / LON_EXTENT) * static_cast<double>(numCols));
    auto y = static_cast<int64_t>(((MAX_LAT - latitude) / LAT_EXTENT) * static_cast<double>(numRows));

    x %= numCols;
    y %= numRows * 2;
    y = std::abs(y);  // -y: Crossing the North Pole

    // We have crossed the South Pole
    if (y >= numRows)
        y = numRows - (y - numRows) - 1;

    return {(uint16_t)x, (uint16_t)y, zoomLevel};
}

Point<> TileId::center() const {
    auto extent = size();
    auto lon = MIN_LON + (static_cast<double>(x()) + 0.5) * extent.x;
    // Subtract from MAX_LAT because y=0 is the North Pole
    auto lat = MAX_LAT - (static_cast<double>(y()) + 0.5) * extent.y;
    return {lon, lat};
}

Point<> TileId::sw() const {
    auto extent = size();
    double lon = MIN_LON + static_cast<double>(x()) * extent.x;
    // Subtract from MAX_LAT because y=0 is the North Pole
    double lat = MAX_LAT - static_cast<double>(y() + 1) * extent.y;
    return {lon, lat};
}

Point<> TileId::ne() const {
    auto extent = size();
    double lon = MIN_LON + static_cast<double>(x() + 1) * extent.x;
    // Subtract from MAX_LAT because y=0 is the North Pole
    double lat = MAX_LAT - static_cast<double>(y()) * extent.y;
    return {lon, lat};
}

Point<> TileId::size() const {
    return {LON_EXTENT / (1 << (z() + 1)), LAT_EXTENT / (1 << z())};
}

} // namespace mapget
