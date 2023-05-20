#pragma once

#include <cstdint>

namespace mapget
{


/**
 * TileId class - represents a rectangular area on the globe, according to
 * the following tiling scheme.
 *
 * Each tile is identified by a zoom level `z` and two grid
 * coordinates `x` and `y`. *mapget* uses a binary tiling scheme for
 * the earths surface: The zoom level `z` controls the number of subdivisions for
 * the WGS84 longitudinal `[-180,180]` axis (columns) and latitudinal `[-90,90]` axis (rows).
 * The tile `x` coordinate indicates the column, and the `y` coordinate indicates the row.
 * On level zero, there are two columns and one row. In general, the number of rows is `2^z`,
 * and the number of columns is `2^(z+1)`.
 */
struct TileId
{
    /**
     * Constructor to initialize TileId with x, y, z
     */
    TileId(uint16_t x, uint16_t y, uint16_t z);

    /**
     * Constructor to initialize TileId with value
     */
    TileId(uint64_t const& value);  // NOLINT

    /**
     * Function to create a TileId from WGS84 longitude, latitude, and zoom level.
     */
    static TileId fromWgs84(double longitude, double latitude, uint16_t zoomLevel);

    /**
     * Function to get x (column) component of the TileId
     */
    [[nodiscard]] uint16_t x() const;

    /**
     * Function to get y (row) component of the TileId
     */
    [[nodiscard]] uint16_t y() const;

    /**
     * Function to get z (zoom level) component of the TileId (zoom level)
     */
    [[nodiscard]] uint16_t z() const;

    /**
     * Operator overload for equality comparison
     */
    bool operator==(TileId const&) const;

    /**
     * Operator overload for less than comparison
     */
    bool operator<(TileId const&) const;

    /**
     * The value representing the TileId, in 0x00xxyyzz format.
     */
    uint64_t value_;
};

}