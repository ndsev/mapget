#pragma once

#include <cstdint>
#include <stdexcept>

#include "ndsmath/packedtileid.h"

namespace mapget
{

/** Public mapget tile identifier, backed directly by the NDS.Live packed tile ID. */
using TileId = ndsmath::PackedTileId;

/**
 * Return true if a numeric tile ID structurally matches the removed mapget
 * `0xXXXXYYYYZZZZ` layout and can be converted to an NDS.Live packed tile.
 */
inline bool isLegacyTileId(int64_t value)
{
    if (value < 0)
        return false;

    auto const raw = static_cast<uint64_t>(value);
    if ((raw >> 48U) != 0)
        return false;

    auto const level = static_cast<uint32_t>(raw & 0xFFFFU);
    if (level > 15)
        return false;

    auto const x = static_cast<uint32_t>((raw >> 32U) & 0xFFFFU);
    auto const y = static_cast<uint32_t>((raw >> 16U) & 0xFFFFU);
    return x < (uint32_t{1} << (level + 1U)) && y < (uint32_t{1} << level);
}

/**
 * Convert a removed mapget `0xXXXXYYYYZZZZ` tile ID to the canonical packed
 * representation. Legacy columns start at the antimeridian and rows at the
 * north pole, while packed coordinates follow signed NDS coordinates in
 * two's-complement Morton order. This is migration glue for legacy
 * manifests/keys only.
 */
inline TileId legacyTileIdToPacked(int64_t value)
{
    if (!isLegacyTileId(value))
        throw std::out_of_range("Value is not a convertible legacy mapget tile id.");

    auto const raw = static_cast<uint64_t>(value);
    auto const level = static_cast<int>(raw & 0xFFFFU);
    auto const x = static_cast<uint32_t>((raw >> 32U) & 0xFFFFU);
    auto const y = static_cast<uint32_t>((raw >> 16U) & 0xFFFFU);

    auto const columnCount = uint32_t{1} << (level + 1U);
    auto const rowCount = uint32_t{1} << level;
    // Move the legacy antimeridian origin to signed NDS coordinate zero.
    auto const packedX = (x + columnCount / 2U) % columnCount;
    // Flip the north-origin row, then rotate it into signed NDS grid order.
    auto const packedY = (rowCount + rowCount / 2U - 1U - y) % rowCount;
    return TileId::fromTileXY(packedX, packedY, level);
}

} // namespace mapget
