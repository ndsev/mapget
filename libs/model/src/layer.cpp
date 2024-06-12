#include "layer.h"
#include "mapget/log.h"

#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>

#include "simfil/model/bitsery-traits.h"

#include <istream>
#include <ranges>
#include <string_view>
#include <charconv>

#include "nlohmann/json.hpp"

namespace mapget
{

MapTileKey::MapTileKey(const std::string& str)
{
    // This will get simpler with C++ 23. Then we can just use ranges::to<std::vector>,
    // and also the verbose conversion from a char range to a string_view
    // will not be necessary anymore.
    using namespace std::ranges;
    auto parts = str | views::split(':');
    auto partsVec = std::vector<decltype(*parts.begin())>(parts.begin(), parts.end());

    if (partsVec.size() < 4)
        raise(fmt::format("Invalid cache tile id: {}", str));
    layer_ = nlohmann::json(std::string_view(&*partsVec[1].begin(), distance(partsVec[1]))).get<LayerType>();
    mapId_ = std::string_view(&*partsVec[1].begin(), distance(partsVec[1]));
    layerId_ = std::string_view(&*partsVec[2].begin(), distance(partsVec[2]));
    std::from_chars(&*partsVec[3].begin(), &*partsVec[3].begin() + distance(partsVec[3]), tileId_.value_, 16);
}

MapTileKey::MapTileKey(const TileLayer& data)
{
    layer_ = data.layerInfo()->type_;
    mapId_ = data.mapId();
    layerId_ = data.layerInfo()->layerId_;
    tileId_ = data.tileId();
}

std::string MapTileKey::toString() const
{
    return fmt::format(
        "{}:{}:{}:{:0x}",
        nlohmann::json(layer_).get<std::string>(),
        mapId_,
        layerId_,
        tileId_.value_);
}

bool MapTileKey::operator<(const MapTileKey& other) const
{
    return std::tie(layer_, mapId_, layerId_, tileId_) <
        std::tie(other.layer_, other.mapId_, other.layerId_, other.tileId_);
}

bool MapTileKey::operator==(const MapTileKey& other) const
{
    return std::tie(layer_, mapId_, layerId_, tileId_) ==
        std::tie(other.layer_, other.mapId_, other.layerId_, other.tileId_);
}

bool MapTileKey::operator!=(const MapTileKey& other) const
{
    return !(*this == other);
}

TileLayer::TileLayer(
    const TileId& id,
    std::string nodeId,
    std::string mapId,
    const std::shared_ptr<LayerInfo>& info
)
    : tileId_(id),
      nodeId_(std::move(nodeId)),
      mapId_(std::move(mapId)),
      layerInfo_(info),
      timestamp_(std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch()))
{
}

TileLayer::TileLayer(
    std::istream& inputStream,
    const LayerInfoResolveFun& layerInfoResolveFun
) : tileId_(0)
{
    using namespace std::chrono;
    using namespace nlohmann;

    bitsery::Deserializer<bitsery::InputStreamAdapter> s(inputStream);
    s.text1b(mapId_, std::numeric_limits<uint32_t>::max());
    std::string layerName;
    s.text1b(layerName, std::numeric_limits<uint32_t>::max());
    layerInfo_ = layerInfoResolveFun(mapId_, layerName);

    s.object(mapVersion_);
    if (!mapVersion_.isCompatible(layerInfo_->version_)) {
        raise(fmt::format(
            "Read map layer '{}' version {} "
            "is incompatible with present version {}.",
            layerName,
            mapVersion_.toString(),
            layerInfo_->version_.toString()));
    }

    s.value8b(tileId_.value_);
    s.text1b(nodeId_, std::numeric_limits<uint32_t>::max());

    int64_t timestamp = 0;
    s.value8b(timestamp);
    timestamp_ = time_point<system_clock>(microseconds(timestamp));

    bool hasTtl = false;
    s.value1b(hasTtl);
    if (hasTtl) {
        int64_t ttl = 0;
        s.value8b(ttl);
        ttl_ = milliseconds(ttl);
    }

    std::string infoJsonString;
    s.text1b(infoJsonString, std::numeric_limits<uint32_t>::max());
    info_ = json::parse(infoJsonString);
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

std::chrono::time_point<std::chrono::system_clock> TileLayer::timestamp() const {
    return timestamp_;
}

std::optional<std::chrono::milliseconds> TileLayer::ttl() const {
    return ttl_;
}

Version TileLayer::mapVersion() const {
    return mapVersion_;
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

void TileLayer::setTimestamp(const std::chrono::time_point<std::chrono::system_clock>& ts) {
    timestamp_ = ts;
}

void TileLayer::setTtl(const std::optional<std::chrono::milliseconds>& timeToLive) {
    ttl_ = timeToLive;
}

void TileLayer::setMapVersion(Version v) {
    mapVersion_ = v;
}

void TileLayer::setInfo(std::string const& k, nlohmann::json const& v) {
    info_[k] = v;
}

void TileLayer::write(std::ostream& outputStream)
{
    using namespace std::chrono;
    using namespace nlohmann;

    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);
    s.text1b(mapId_, std::numeric_limits<uint32_t>::max());
    s.text1b(layerInfo_->layerId_, std::numeric_limits<uint32_t>::max());
    s.object(mapVersion_);
    s.value8b(tileId_.value_);
    s.text1b(nodeId_, std::numeric_limits<uint32_t>::max());
    s.value8b(duration_cast<microseconds>(timestamp_.time_since_epoch()).count());
    s.value1b(ttl_.has_value());
    if (ttl_)
        s.value8b(ttl_->count());
    s.text1b(info_.dump(), std::numeric_limits<uint32_t>::max());
}

MapTileKey TileLayer::id() const
{
    return MapTileKey(*this);
}

} // namespace mapget
