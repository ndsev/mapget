#include "layer.h"

namespace mapget
{

TileLayer::TileLayer(
    const TileId& id,
    std::string node_id,
    std::string map_id,
    const std::shared_ptr<LayerInfo>& info
)
    : tileId_(id),
      nodeId_(std::move(node_id)),
      mapId_(std::move(map_id)),
      layerInfo_(info),
      timestamp_(std::chrono::steady_clock::now())
{
    // constructor body
}

TileId TileLayer::tileId() const {
    return tileId_;
}

std::string TileLayer::nodeId() const {
    return nodeId_;
}

std::string TileLayer::mapId() const {
    return mapId_;
}

std::shared_ptr<LayerInfo> TileLayer::layerInfo() const {
    return layerInfo_;
}

std::optional<std::string> TileLayer::error() const {
    return error_;
}

std::chrono::time_point<std::chrono::steady_clock> TileLayer::timestamp() const {
    return timestamp_;
}

std::optional<std::chrono::seconds> TileLayer::ttl() const {
    return ttl_;
}

int TileLayer::protocolVersion() const {
    return protocolVersion_;
}

nlohmann::json TileLayer::info() const {
    return info_;
}

void TileLayer::setTileId(const TileId& id) {
    tileId_ = id;
}

void TileLayer::setNodeId(const std::string& id) {
    nodeId_ = id;
}

void TileLayer::setMapId(const std::string& id) {
    mapId_ = id;
}

void TileLayer::setLayerInfo(const std::shared_ptr<LayerInfo>& info) {
    layerInfo_ = info;
}

void TileLayer::setError(const std::optional<std::string>& err) {
    error_ = err;
}

void TileLayer::setTimestamp(const std::chrono::time_point<std::chrono::steady_clock>& ts) {
    timestamp_ = ts;
}

void TileLayer::setTtl(const std::optional<std::chrono::seconds>& timeToLive) {
    ttl_ = timeToLive;
}

void TileLayer::setProtocolVersion(int version) {
    protocolVersion_ = version;
}

void TileLayer::setInfo(const nlohmann::json& info) {
    this->info_ = info;
}

} // namespace mapget
