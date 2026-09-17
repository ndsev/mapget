#pragma once

#include <compare>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include "tileid.h"

namespace mapget
{
/** Addressing scheme, independent of the kind of payload stored in a partition. */
enum class PartitionKind : uint8_t { Tile, Object };

/** A lossless container identity. Spatial operations require an actual tile. */
class PartitionId
{
public:
    /** Default is the existing tile-zero metadata sentinel, not object zero. */
    PartitionId() = default;
    /** Keep tile-datasource construction convenient without accepting untagged integers. */
    PartitionId(TileId id) : value_(static_cast<uint32_t>(id.value())) {}
    /** Construct a tile address, including the metadata sentinel. */
    static PartitionId tile(TileId id) { return id; }
    /** Construct an object address; all 64 bits, including zero, are identity. */
    static PartitionId object(uint64_t id)
    {
        PartitionId result;
        result.kind_ = PartitionKind::Object;
        result.value_ = id;
        return result;
    }
    /** Inspect the tag without interpreting the numeric identifier. */
    PartitionKind kind() const { return kind_; }
    /** Checked access to spatial identity. Objects never masquerade as tiles. */
    TileId tileId() const
    {
        if (kind_ != PartitionKind::Tile)
            throw std::logic_error("Object partition has no tile ID.");
        auto raw = static_cast<int32_t>(static_cast<uint32_t>(value_));
        return raw == 0 ? TileId{} : TileId::fromValue(raw);
    }
    /** Checked access to unsigned object identity. */
    uint64_t objectId() const
    {
        if (kind_ != PartitionKind::Object)
            throw std::logic_error("Tile partition has no object ID.");
        return value_;
    }
    /** Preserve the familiar tile-only numeric accessor with a checked boundary. */
    int32_t value() const { return tileId().value(); }
    /** Parse a packed tile value; object construction is always explicitly tagged. */
    static PartitionId fromValue(int32_t value) { return TileId::fromValue(value); }
    /** Unsigned decimal for objects, signed decimal for packed tiles. */
    std::string toString() const
    {
        return kind_ == PartitionKind::Object ? std::to_string(value_) : std::to_string(value());
    }
    /** JSON object IDs are strings, never floating-point browser numbers. */
    nlohmann::json toJson() const;
    /** Parse a strict, tagged transport identity. */
    static PartitionId fromJson(nlohmann::json const& json);
    /** Serialize the tag and the native-width identifier, rejecting unknown tags. */
    template <class S>
    void serialize(S& s)
    {
        s.value1b(kind_);
        if (kind_ == PartitionKind::Object) {
            s.value8b(value_);
        }
        else if (kind_ == PartitionKind::Tile) {
            auto raw = static_cast<uint32_t>(value_);
            s.value4b(raw);
            value_ = raw;
            (void)tileId();
        }
        else {
            throw std::invalid_argument("Unknown partition kind.");
        }
    }
    /** Kind is part of identity even when numeric values coincide. */
    auto operator<=>(PartitionId const&) const = default;

private:
    PartitionKind kind_ = PartitionKind::Tile;
    uint64_t value_ = 0;
};

class PartitionLayer;
class PartitionFeatureLayer;
class PartitionFeatureModelLayerBase;
class PartitionSubsetLayer;
class PartitionSourceDataLayer;
struct MapPartitionKey;
// Tile datasources retain their familiar names and includes; there is one model implementation.
using TileLayer = PartitionLayer;
using TileFeatureLayer = PartitionFeatureLayer;
using TileFeatureModelLayerBase = PartitionFeatureModelLayerBase;
using TileSubsetLayer = PartitionSubsetLayer;
using TileSourceDataLayer = PartitionSourceDataLayer;
using MapTileKey = MapPartitionKey;
}  // namespace mapget

template <>
struct std::hash<mapget::PartitionId>
{
    /** Hash both kind and value; equality remains authoritative. */
    size_t operator()(mapget::PartitionId const& id) const noexcept
    {
        auto value = id.kind() == mapget::PartitionKind::Object ?
            id.objectId() :
            static_cast<uint64_t>(static_cast<uint32_t>(id.value()));
        return std::hash<uint64_t>{}(value) ^ (static_cast<size_t>(id.kind()) << 1);
    }
};
