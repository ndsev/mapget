#include "tiles-ws-request.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace mapget::detail
{

/** Read a numeric JSON field as non-negative int64, clamping unsigned overflow. */
int64_t parseNonNegativeInt64(const nlohmann::json& j, std::string_view key)
{
    const auto keyString = std::string(key);
    const auto it = j.find(keyString);
    if (it == j.end()) {
        return 0;
    }
    if (it->is_number_unsigned()) {
        const auto raw = it->get<uint64_t>();
        const auto max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        return static_cast<int64_t>(std::min(raw, max));
    }
    if (it->is_number_integer()) {
        const auto raw = it->get<int64_t>();
        return std::max<int64_t>(0, raw);
    }
    return 0;
}

/** Parse optional WS request chunk metadata and validate chunk sequencing fields. */
ClientRequestChunk parseClientRequestChunk(const nlohmann::json& j)
{
    auto chunkIt = j.find("chunk");
    if (chunkIt == j.end()) {
        return {};
    }
    if (!chunkIt->is_object()) {
        throw std::runtime_error("chunk must be an object");
    }

    auto indexIt = chunkIt->find("index");
    if (indexIt == chunkIt->end()
        || !(indexIt->is_number_integer() || indexIt->is_number_unsigned())) {
        throw std::runtime_error("chunk.index must be a non-negative integer");
    }
    if (indexIt->is_number_integer() && indexIt->get<int64_t>() < 0) {
        throw std::runtime_error("chunk.index must be a non-negative integer");
    }

    auto isLastIt = chunkIt->find("isLast");
    if (isLastIt == chunkIt->end() || !isLastIt->is_boolean()) {
        throw std::runtime_error("chunk.isLast must be a boolean");
    }

    return ClientRequestChunk{
        .chunked = true,
        .index = static_cast<uint64_t>(parseNonNegativeInt64(*chunkIt, "index")),
        .isLast = isLastIt->get<bool>(),
    };
}

/** Build the canonical key used for matching client tile requests to backend frames. */
MapTileKey makeCanonicalRequestedTileKey(
    std::string_view mapId,
    std::string_view layerId,
    TileId tileId,
    uint32_t stage)
{
    return MapTileKey(
        REQUEST_TILE_LAYER_TYPE,
        std::string(mapId),
        std::string(layerId),
        tileId,
        stage);
}

/** Normalize an existing backend tile key into the request-key namespace. */
MapTileKey makeCanonicalRequestedTileKey(MapTileKey key)
{
    key.layer_ = REQUEST_TILE_LAYER_TYPE;
    return key;
}

/** Decorate a canonical tile key so concurrent search results do not collide. */
MapTileKey makeSearchRequestedTileKey(MapTileKey key, std::string_view searchRequestKey)
{
    key = makeCanonicalRequestedTileKey(std::move(key));
    key.layerId_.append("#search:");
    key.layerId_.append(searchRequestKey);
    return key;
}

/** Build either a normal tile key or the search-specific key for the same source tile. */
MapTileKey makeRequestedTileKey(
    MapTileKey key,
    std::optional<std::string_view> searchRequestKey)
{
    if (searchRequestKey && !searchRequestKey->empty()) {
        return makeSearchRequestedTileKey(std::move(key), *searchRequestKey);
    }
    return makeCanonicalRequestedTileKey(std::move(key));
}

/** Extract the search request key carried by backend-produced search-result layers. */
std::optional<std::string> searchRequestKey(TileLayer::Ptr const& layer)
{
    if (!std::dynamic_pointer_cast<TileSearchResultLayer>(layer)) {
        return std::nullopt;
    }
    auto info = layer->info();
    auto it = info.find("searchRequestKey");
    if (it == info.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

} // namespace mapget::detail
