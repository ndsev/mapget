#pragma once

#include "info.h"
#include "tileid.h"

#include "nlohmann/json.hpp"
#include "simfil/model/nodes.h"

#include <string>
#include <chrono>
#include <optional>
#include <memory>
#include <tuple>

namespace mapget
{

/**
 * Callback type for a function which returns a layer info pointer for
 * a given (map-name, layer-name) combination.
 */
using LayerInfoResolveFun = std::function<std::shared_ptr<LayerInfo>(std::string_view const&, std::string_view const&)>;

/**
 * Tile Layer base class. Used by TileFeatureLayer class and other
 * tile-specific data containers.
 */
class TileLayer
{
public:
    /**
     * Constructor that takes tileId_, nodeId_, mapId_, layerInfo_,
     * and sets the timestamp_ to the current system time.
     */
    TileLayer(
        const TileId& id,
        std::string nodeId,
        std::string mapId,
        const std::shared_ptr<LayerInfo>& info);

    /**
     * Parse a tile layer from an input stream. Will throw if
     * the resolved major-minor of the TileLayer is not the same
     * as the one read from the stream.
     */
    TileLayer(
        std::istream& inputStream,
        LayerInfoResolveFun const& layerInfoResolveFun);

    /**
     * Getter and setter for layer's tileId. This controls the rough
     * geographic extent of the contained tile data.
     */
    [[nodiscard]] TileId tileId() const;
    void setTileId(const TileId& id);

    /**
     * Getter and setter for layer's nodeId. This is the identifier of
     * the data source process which created this layer.
     */
    [[nodiscard]] std::string nodeId() const;
    void setNodeId(const std::string& id);

    /**
     * Getter and setter for the layer's mapId. This is the identifier
     * of the map which is this tile layer belongs to.
     */
    [[nodiscard]] std::string mapId() const;
    void setMapId(const std::string& id);

    /**
     * Getter and setter for 'layerInfo_' member variable.
     * It holds LayerInfo reference for this TileLayer.
     */
    [[nodiscard]] std::shared_ptr<LayerInfo> layerInfo() const;
    void setLayerInfo(const std::shared_ptr<LayerInfo>& info);

    /**
     * Getter and setter for 'error' member variable.
     * It's used to indicate that an error occurred while the tile was filled.
     */
    [[nodiscard]] std::optional<std::string> error() const;
    void setError(const std::optional<std::string>& err);

    /**
     * Getter and setter for 'timestamp' member variable.
     * It represents when this layer was created.
     */
    [[nodiscard]] std::chrono::time_point<std::chrono::system_clock> timestamp() const;
    void setTimestamp(const std::chrono::time_point<std::chrono::system_clock>& ts);

    /**
     * Getter and setter for 'ttl_' member variable.
     * It represents how long this layer should live.
     */
    [[nodiscard]]std::optional<std::chrono::milliseconds> ttl() const;
    void setTtl(const std::optional<std::chrono::milliseconds>& timeToLive);

    /**
     * Getter and setter for 'mapVersion_' member variable.
     * It represents the protocol version that was used to serialize this layer.
     */
    [[nodiscard]] Version mapVersion() const;
    void setProtocolVersion(Version v);

    /**
     * Getter and setter for 'info' member variable.
     * It's an extra JSON document to store sizes, construction times,
     * and other arbitrary meta-information.
     */
    [[nodiscard]] nlohmann::json info() const;
    void setInfo(std::string const& k, nlohmann::json const& v);

protected:
    Version mapVersion_{0, 0, 0};
    TileId tileId_;
    std::string nodeId_;
    std::string mapId_;
    std::shared_ptr<LayerInfo> layerInfo_;
    std::optional<std::string> error_;
    std::chrono::time_point<std::chrono::system_clock> timestamp_;
    std::optional<std::chrono::milliseconds> ttl_;
    nlohmann::json info_;

    /** Serialization */
    void write(std::ostream& outputStream);
};

}
