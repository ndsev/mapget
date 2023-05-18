#pragma once

#include <cstdint>

namespace mapget
{

/// Structure to represent the Tile ID
struct TileId
{
    /// Constructor to initialize TileId with x, y, z
    TileId(uint16_t x, uint16_t y, uint16_t z);

    /// Constructor to initialize TileId with value
    TileId(uint64_t const& value);  // NOLINT

    /// Function to get x (column) component of the TileId
    [[nodiscard]] uint16_t x() const;

    /// Function to get y (row) component of the TileId
    [[nodiscard]] uint16_t y() const;

    /// Function to get z (zoom level) component of the TileId (zoom level)
    [[nodiscard]] uint16_t z() const;

    /// Operator overload for equality comparison
    bool operator==(TileId const&) const;

    /// Operator overload for less than comparison
    bool operator<(TileId const&) const;

    /// The value representing the TileId
    uint64_t value_;
};

}