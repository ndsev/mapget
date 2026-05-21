#pragma once

#include "mapget/model/searchresultlayer.h"
#include "mapget/service/service.h"
#include "tiles-request-json.h"

#include "nlohmann/json.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace mapget::detail
{

/** Layer type used for canonical websocket request keys regardless of backend payload subtype. */
inline constexpr LayerType REQUEST_TILE_LAYER_TYPE = LayerType::Features;

/** Chunk metadata carried by large websocket request updates. */
struct ClientRequestChunk
{
    bool chunked = false;
    uint64_t index = 0;
    bool isLast = true;
};

/** Whether a parsed request replaces the current scope or appends another chunk. */
enum class ClientRequestUpdateMode
{
    Replace,
    Append,
};

/** Parse a JSON numeric field into non-negative int64 while handling missing keys. */
[[nodiscard]] int64_t parseNonNegativeInt64(const nlohmann::json& j, std::string_view key);

/** Parse optional chunk metadata from a client request envelope. */
[[nodiscard]] ClientRequestChunk parseClientRequestChunk(const nlohmann::json& j);

/** Build a canonical request key using map/layer/tile while normalizing layer type. */
[[nodiscard]] MapTileKey makeCanonicalRequestedTileKey(
    std::string_view mapId,
    std::string_view layerId,
    TileId tileId,
    uint32_t stage = 0);

/** Normalize an existing map tile key so request matching ignores source layer type. */
[[nodiscard]] MapTileKey makeCanonicalRequestedTileKey(MapTileKey key);

/** Decorate server-internal queue keys so search results do not collide with source tile frames. */
[[nodiscard]] MapTileKey makeSearchRequestedTileKey(MapTileKey key, std::string_view searchRequestKey);

/** Build the outgoing-frame key for either normal feature tiles or a specific search request. */
[[nodiscard]] MapTileKey makeRequestedTileKey(
    MapTileKey key,
    std::optional<std::string_view> searchRequestKey);

/** Extract the search request key attached to TileSearchResultLayer metadata. */
[[nodiscard]] std::optional<std::string> searchRequestKey(TileLayer::Ptr const& layer);

} // namespace mapget::detail
