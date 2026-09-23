#include "mapget/model/partitionid.h"
#include <charconv>
#include <limits>

namespace mapget
{
nlohmann::json PartitionId::toJson() const
{
    if (kind() == PartitionKind::Object)
        return {{"kind", "object"}, {"id", toString()}};
    return {{"kind", "tile"}, {"id", value()}};
}

PartitionId PartitionId::fromJson(nlohmann::json const& json)
{
    auto const kind = json.at("kind").get<std::string>();
    if (kind == "object") {
        auto const text = json.at("id").get<std::string>();
        uint64_t value = 0;
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || text.empty())
            throw std::invalid_argument("Object ID must be an unsigned 64-bit decimal string.");
        return object(value);
    }
    if (kind != "tile" || !json.at("id").is_number_integer())
        throw std::invalid_argument(
            "Expected a tile partition with an integer ID or an object partition with a decimal "
            "string ID.");
    if (json.at("id").is_number_unsigned() && json.at("id").get<uint64_t>() > INT32_MAX)
        throw std::invalid_argument("Packed tile ID is outside the signed 32-bit range.");
    auto const value = json.at("id").get<int64_t>();
    if (value < INT32_MIN || value > INT32_MAX)
        throw std::invalid_argument("Packed tile ID is outside the signed 32-bit range.");
    return value == 0 ? tile(TileId{}) : fromValue(static_cast<int32_t>(value));
}
}  // namespace mapget
