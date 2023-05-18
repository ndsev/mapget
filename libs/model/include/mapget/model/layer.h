#pragma once

#include "info.h"
#include "tileid.h"

#include "nlohmann/json.hpp"

#include <string>
#include <chrono>
#include <optional>
#include <memory>

namespace mapget
{

/**
 * Tile Layer base class. Used by TileFeatureLayer class and other
 * tile-specific data containers.
 */
class TileLayer
{
public:
    /// Constructor that takes tileId_, nodeId_, mapId_, layerInfo_,
    /// and sets the timestamp_ to the current system time.
    TileLayer(
        const TileId& id,
        const std::string& node_id,
        const std::string& map_id,
        const std::shared_ptr<LayerInfo>& info);

    /// Getter and setter for layer's tileId. This controls the rough
    /// geographic extent of the contained tile data.
    [[nodiscard]] TileId tileId() const;
    void setTileId(const TileId& id);

    /// Getter and setter for layer's nodeId. This is the identifier of
    /// the data source process which created this layer.
    [[nodiscard]] std::string nodeId() const;
    void setNodeId(const std::string& id);

    /// Getter and setter for the layer's mapId. This is the identifier
    /// of the map which is this tile layer belongs to.
    [[nodiscard]] std::string mapId() const;
    void setMapId(const std::string& id);

    /// Getter and setter for 'layerInfo_' member variable.
    /// It holds LayerInfo reference for this TileLayer.
    [[nodiscard]] std::shared_ptr<LayerInfo> layerInfo() const;
    void setLayerInfo(const std::shared_ptr<LayerInfo>& info);

    /// Getter and setter for 'error' member variable.
    /// It's used to indicate that an error occurred while the tile was filled.
    [[nodiscard]] std::optional<std::string> error() const;
    void setError(const std::optional<std::string>& err);

    /// Getter and setter for 'timestamp' member variable.
    /// It represents when this layer was created.
    [[nodiscard]] std::chrono::time_point<std::chrono::steady_clock> timestamp() const;
    void setTimestamp(const std::chrono::time_point<std::chrono::steady_clock>& ts);

    /// Getter and setter for 'ttl_' member variable.
    /// It represents how long this layer should live.
    [[nodiscard]]std::optional<std::chrono::seconds> ttl() const;
    void setTtl(const std::optional<std::chrono::seconds>& timeToLive);

    /// Getter and setter for 'protocolVersion_' member variable.
    /// It represents the protocol version that was used to serialize this layer.
    [[nodiscard]] int protocolVersion() const;
    void setProtocolVersion(int version);

    /// Getter and setter for 'info' member variable.
    /// It's an extra JSON document to store sizes, construction times,
    /// and other arbitrary meta-information.
    [[nodiscard]] nlohmann::json info() const;
    void setInfo(const nlohmann::json& info);

protected:
    TileId tileId_;
    std::string nodeId_;
    std::string mapId_;
    std::shared_ptr<LayerInfo> layerInfo_;
    std::optional<std::string> error_;
    std::chrono::time_point<std::chrono::steady_clock> timestamp_;
    std::optional<std::chrono::seconds> ttl_;
    int protocolVersion_ = 0;
    nlohmann::json info_;
};

}
