#include "layer.h"
#include "mapget/log.h"

#include <bitsery/bitsery.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>

#include "simfil/model/bitsery-traits.h"

#include <istream>
#include <string_view>
#include <charconv>
#include <vector>

#include "nlohmann/json.hpp"

namespace mapget
{

MapTileKey::MapTileKey(const std::string& str)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (size_t i = 0; i <= str.size(); ++i) {
        if (i == str.size() || str[i] == ':') {
            parts.push_back(std::string_view(str).substr(start, i - start));
            start = i + 1;
        }
    }

    if (parts.size() < 4)
        raise(fmt::format("Invalid cache tile id: {}", str));

    layer_ = nlohmann::json(std::string(parts[0])).get<LayerType>();

    std::string error;
    if (!unescapeIdentifierComponent(parts[1], mapId_, &error) ||
        !unescapeIdentifierComponent(parts[2], layerId_, &error)) {
        raise(fmt::format("Invalid cache tile id '{}': {}", str, error));
    }

    auto parseTileResult = std::from_chars(
        parts[3].data(),
        parts[3].data() + parts[3].size(),
        tileId_.value_,
        16);
    if (parseTileResult.ec != std::errc() ||
        parseTileResult.ptr != parts[3].data() + parts[3].size()) {
        raise(fmt::format("Invalid cache tile id: {}", str));
    }

    if (parts.size() >= 5) {
        uint32_t parsedStage = 0;
        auto parseResult = std::from_chars(
            parts[4].data(),
            parts[4].data() + parts[4].size(),
            parsedStage,
            10);
        if (parseResult.ec == std::errc() && parseResult.ptr == parts[4].data() + parts[4].size()) {
            stage_ = parsedStage;
        }
    }
}

MapTileKey::MapTileKey(LayerType layer, std::string mapId, std::string layerId, TileId tileId, uint32_t stage) :
    layer_(layer), mapId_(std::move(mapId)), layerId_(std::move(layerId)), tileId_(tileId), stage_(stage)
{}

MapTileKey::MapTileKey(const TileLayer& data)
{
    layer_ = data.layerInfo()->type_;
    mapId_ = data.mapId();
    layerId_ = data.layerInfo()->layerId_;
    tileId_ = data.tileId();
    stage_ = data.stage().value_or(UnspecifiedStage);
}

std::string MapTileKey::toString() const
{
    return fmt::format(
        "{}:{}:{}:{:0x}:{}",
        nlohmann::json(layer_).get<std::string>(),
        escapeIdentifierComponent(mapId_),
        escapeIdentifierComponent(layerId_),
        tileId_.value_,
        stage_);
}

bool MapTileKey::operator<(const MapTileKey& other) const
{
    return std::tie(layer_, mapId_, layerId_, tileId_, stage_) <
        std::tie(other.layer_, other.mapId_, other.layerId_, other.tileId_, other.stage_);
}

bool MapTileKey::operator==(const MapTileKey& other) const
{
    return std::tie(layer_, mapId_, layerId_, tileId_, stage_) ==
        std::tie(other.layer_, other.mapId_, other.layerId_, other.tileId_, other.stage_);
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
    const std::vector<uint8_t>& input,
    const LayerInfoResolveFun& layerInfoResolveFun,
    size_t* bytesRead
) : tileId_(0)
{
    using namespace std::chrono;
    using namespace nlohmann;

    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> s(Adapter(input.begin(), input.end()));
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

    bool hasError = false;
    s.value1b(hasError);
    if (hasError) {
        error_ = "";  // Tell the optional that it has a value.
        s.text1b(*error_, std::numeric_limits<uint32_t>::max());
    }

    bool hasErrorCode = false;
    s.value1b(hasErrorCode);
    if (hasErrorCode) {
        errorCode_ = 0;  // Tell the optional that it has a value.
        s.value4b(*errorCode_);
    }

    bool hasLegalInfo = false;
    s.value1b(hasLegalInfo);
    if (hasLegalInfo) {
        legalInfo_ = "";  // Tell the optional that it has a value.
        s.text1b(*legalInfo_, std::numeric_limits<uint32_t>::max());
    }

    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raise(fmt::format(
            "Failed to read TileLayer: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error())));
    }
    if (bytesRead != nullptr) {
        *bytesRead = s.adapter().currentReadPos();
    }
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

std::optional<std::string> TileLayer::legalInfo() const
{
    return legalInfo_;
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

std::optional<int> TileLayer::errorCode() const {
    return errorCode_;
}

void TileLayer::setErrorCode(const std::optional<int>& code) {
    errorCode_ = code;
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

void TileLayer::setLegalInfo(const std::string& legalInfoString)
{
    legalInfo_ = legalInfoString;
}

void TileLayer::setLoadStateCallback(LoadStateCallback cb)
{
    onLoadStateChanged_ = std::move(cb);
}

void TileLayer::setLoadState(LoadState state)
{
    if (onLoadStateChanged_) {
        onLoadStateChanged_(state);
    }
}

std::optional<uint32_t> TileLayer::stage() const
{
    return {};
}

void TileLayer::setStage(std::optional<uint32_t> /*stage*/)
{
    // Base TileLayer does not carry stage information.
}

tl::expected<void, simfil::Error> TileLayer::write(std::ostream& outputStream)
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
    s.value1b(error_.has_value());
    if (error_)
        s.text1b(*error_, std::numeric_limits<uint32_t>::max());
    s.value1b(errorCode_.has_value());
    if (errorCode_)
        s.value4b(*errorCode_);
    s.value1b(legalInfo_.has_value());
    if (legalInfo_.has_value()) {
        s.text1b(legalInfo_.value(), std::numeric_limits<uint32_t>::max());
    }

    return {};
}

MapTileKey TileLayer::id() const
{
    return MapTileKey(*this);
}

nlohmann::json TileLayer::toJson() const
{
    return {};
}

} // namespace mapget
