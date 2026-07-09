#pragma once

#include "info.h"
#include "tileid.h"

#include "nlohmann/json.hpp"
#include "simfil/error.h"

#include <string>
#include <chrono>
#include <optional>
#include <memory>
#include <functional>
#include <vector>
#include <limits>
#include <tl/expected.hpp>

namespace simfil { struct StringPool; }

namespace mapget
{

constexpr uint32_t UnspecifiedStage = std::numeric_limits<uint32_t>::max();

/**
 * Callback type for a function which returns a string pool instance
 * for a given node identifier.
 */
using StringPoolResolveFun = std::function<std::shared_ptr<simfil::StringPool>(std::string_view const&)>;

/**
 * Callback type for a function which returns a layer info pointer for
 * a given (map-name, layer-name) combination.
 */
using LayerInfoResolveFun = std::function<std::shared_ptr<LayerInfo>(std::string_view const&, std::string_view const&)>;

class TileLayer;

/** Struct which represents the unique id of a tile layer.*/
struct MapTileKey
{
    // The tile's data type
    LayerType layer_ = LayerType::Features;

    // The tile's associated map
    std::string mapId_;

    // The tile's associated map layer id
    std::string layerId_;

    // The tile's associated map tile id
    TileId tileId_;

    // Staged-loading index for this tile/layer request (0-based).
    uint32_t stage_ = 0;

    /** Constructor to parse the key from a string, as returned by toString. */
    explicit MapTileKey(std::string const& str);

    /** Constructor to create the cache key for any TileLayer object. */
    explicit MapTileKey(TileLayer const& data);

    /** Constructor to create the cache key from raw components. */
    explicit MapTileKey(LayerType layer, std::string mapId, std::string layerId, TileId tileId, uint32_t stage = 0);

    /** Allow default ctor. */
    MapTileKey() = default;

    /** Convert the key to a string. The string will be in the form of
     *  "(0):(1):(2):(3):(4)", with
     *   (0) being the layer type enum name,
     *   (1) being the percent-escaped map id,
     *   (2) being the percent-escaped layer id,
     *   (3) being the decimal packed tile id,
     *   (4) being the decimal stage index.
     */
    [[nodiscard]] std::string toString() const;

    /** Operator <, allows this struct to be used as an std::map key. */
    bool operator<(MapTileKey const& other) const;

    /** Operator ==, compares all components. */
    bool operator==(MapTileKey const& other) const;

    /** Operator ==, compares all components. */
    bool operator!=(MapTileKey const& other) const;
};

/**
 * Tile Layer base class. Used by TileFeatureLayer class and other
 * tile-specific data containers.
 */
class TileLayer
{
public:
    using Ptr = std::shared_ptr<TileLayer>;
    enum class LoadState : uint8_t {
        LoadingQueued = 0,
        BackendFetching = 1,
        BackendConverting = 2
    };
    using LoadStateCallback = std::function<void(LoadState)>;

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
     * Parse a tile layer from a binary byte buffer. Will throw if
     * the resolved major-minor version of the TileLayer is not the same
     * as the one read from the input.
     */
    TileLayer(
        const std::vector<uint8_t>& input,
        LayerInfoResolveFun const& layerInfoResolveFun,
        size_t* bytesRead = nullptr);

    virtual ~TileLayer() = default;

    /** Get a global identifier for this tile layer. */
    [[nodiscard]] MapTileKey id() const;

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
     * Getter and setter for 'errorCode' member variable.
     * It's used to provide a numeric error code (e.g., HTTP status code,
     * SQLite error code) when an error occurred while filling the tile.
     */
    [[nodiscard]] std::optional<int> errorCode() const;
    void setErrorCode(const std::optional<int>& code);

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
    [[nodiscard]] std::optional<std::chrono::milliseconds> ttl() const;
    void setTtl(const std::optional<std::chrono::milliseconds>& timeToLive);

    /**
     * Getter and setter for 'mapVersion_' member variable.
     * It represents the map layer version that was used to serialize this layer.
     */
    [[nodiscard]] Version mapVersion() const;
    void setMapVersion(Version v);

    /**
     * Getter and setter for 'info' member variable.
     * It's an extra JSON document to store sizes, construction times,
     * and other arbitrary meta-information.
     */
    [[nodiscard]] nlohmann::json info() const;
    void setInfo(std::string const& k, nlohmann::json const& v);

    /**
     * Getter and setter for this tile's copyright information.
     */
    [[nodiscard]] std::optional<std::string> legalInfo() const;
    void setLegalInfo(const std::string& legalInfoString);

    /** Serialization */
    virtual tl::expected<void, simfil::Error> write(std::ostream& outputStream);
    virtual nlohmann::json toJson() const;

    /**
     * Set a load-state callback. Used by the service to forward state changes.
     * Not serialized with the tile.
     */
    void setLoadStateCallback(LoadStateCallback cb);

    /** Emit a load-state change (if a callback is registered). */
    void setLoadState(LoadState state);

    /**
     * Optional staged-loading index for feature tiles.
     * Base TileLayer implementation has no stage.
     */
    [[nodiscard]] virtual std::optional<uint32_t> stage() const;
    virtual void setStage(std::optional<uint32_t> stage);

protected:
    size_t deserializationOffsetBytes_ = 0;
    Version mapVersion_{0, 0, 0};
    TileId tileId_;
    std::string nodeId_; // Identifier of the string-pool/datasource instance
    std::string mapId_;
    std::shared_ptr<LayerInfo> layerInfo_;
    std::optional<std::string> error_;
    std::optional<int> errorCode_;
    std::chrono::time_point<std::chrono::system_clock> timestamp_;
    std::optional<std::chrono::milliseconds> ttl_;
    nlohmann::json info_;
    std::optional<std::string> legalInfo_; // Copyright-related information
    LoadStateCallback onLoadStateChanged_;
};

}
