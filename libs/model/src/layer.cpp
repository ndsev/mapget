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
#include <limits>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace mapget
{

namespace
{

/** SourceData layers use tile id 0 as a request-wide metadata/global sentinel. */
bool isSourceDataTileZeroSentinel(LayerType layer)
{
    return layer == LayerType::SourceData;
}

/** Parse packed tile IDs first, then accept the removed mapget layout for stale keys/blobs. */
TileId parseRawTileIdValue(
    int64_t parsedTileId,
    std::string const& context,
    LayerType layer)
{
    if (parsedTileId == 0 && isSourceDataTileZeroSentinel(layer)) {
        // This sentinel is intentionally not a spatial tile and is therefore
        // the only invalid PackedTileId value accepted in persisted layers.
        return TileId();
    }

    if (parsedTileId >= std::numeric_limits<int32_t>::min() &&
        parsedTileId <= std::numeric_limits<int32_t>::max()) {
        try {
            return TileId::fromValue(static_cast<int32_t>(parsedTileId));
        }
        catch (std::out_of_range const&) {
            // Stale map tile keys/blobs may contain small legacy IDs such as
            // level-only `13`; fall through to the legacy-layout recognizer.
        }
    }

    if (isLegacyTileId(parsedTileId))
        return legacyTileIdToPacked(parsedTileId);

    raise(fmt::format("Invalid tile id '{}' in {}", parsedTileId, context));
    return TileId();
}

/** Parse packed tile IDs first, then accept the removed mapget layout for stale keys. */
TileId parseTileIdComponent(
    std::string_view component,
    std::string const& fullKey,
    LayerType layer)
{
    int64_t parsedTileId = 0;
    auto parseTileResult = std::from_chars(
        component.data(),
        component.data() + component.size(),
        parsedTileId,
        10);
    if (parseTileResult.ec != std::errc() ||
        parseTileResult.ptr != component.data() + component.size()) {
        uint64_t parsedHexTileId = 0;
        auto parseHexTileResult = std::from_chars(
            component.data(),
            component.data() + component.size(),
            parsedHexTileId,
            16);
        if (parseHexTileResult.ec != std::errc() ||
            parseHexTileResult.ptr != component.data() + component.size() ||
            parsedHexTileId > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
            !isLegacyTileId(static_cast<int64_t>(parsedHexTileId))) {
            raise(fmt::format("Invalid cache tile id: {}", fullKey));
        }
        return legacyTileIdToPacked(static_cast<int64_t>(parsedHexTileId));
    }

    return parseRawTileIdValue(parsedTileId, fmt::format("cache key '{}'", fullKey), layer);
}

} // namespace

MapPartitionKey::MapPartitionKey(const std::string& str)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (size_t i = 0; i <= str.size(); ++i) {
        if (i == str.size() || str[i] == ':') {
            parts.push_back(std::string_view(str).substr(start, i - start));
            start = i + 1;
        }
    }

    if (parts.size() != 4)
        raise(fmt::format("Invalid cache tile id: {}", str));

    layer_ = nlohmann::json(std::string(parts[0])).get<LayerType>();

    std::string error;
    if (!unescapeIdentifierComponent(parts[1], mapId_, &error) ||
        !unescapeIdentifierComponent(parts[2], layerId_, &error)) {
        raise(fmt::format("Invalid cache tile id '{}': {}", str, error));
    }

    if (parts[3].starts_with("object/"))
        partitionId_ =
            PartitionId::fromJson({{"kind", "object"}, {"id", std::string(parts[3].substr(7))}});
    else
        partitionId_ = parseTileIdComponent(parts[3], str, layer_);
}

MapPartitionKey::MapPartitionKey(
    LayerType layer,
    std::string mapId,
    std::string layerId,
    PartitionId tileId)
    : layer_(layer), mapId_(std::move(mapId)), layerId_(std::move(layerId)), partitionId_(tileId)
{}

MapPartitionKey::MapPartitionKey(const PartitionLayer& data)
{
    layer_ = data.layerInfo()->type_;
    mapId_ = data.mapId();
    layerId_ = data.layerInfo()->layerId_;
    partitionId_ = data.partitionId();
}

std::string MapPartitionKey::toString() const
{
    return fmt::format(
        "{}:{}:{}:{}",
        nlohmann::json(layer_).get<std::string>(),
        escapeIdentifierComponent(mapId_),
        escapeIdentifierComponent(layerId_),
        partitionId_.kind() == PartitionKind::Object ?
            "object/" + partitionId_.toString() :
            partitionId_.toString());
}

bool MapPartitionKey::operator<(const MapPartitionKey& other) const
{
    return std::tie(layer_, mapId_, layerId_, partitionId_) <
        std::tie(other.layer_, other.mapId_, other.layerId_, other.partitionId_);
}

bool MapPartitionKey::operator==(const MapPartitionKey& other) const
{
    return std::tie(layer_, mapId_, layerId_, partitionId_) ==
        std::tie(other.layer_, other.mapId_, other.layerId_, other.partitionId_);
}

bool MapPartitionKey::operator!=(const MapPartitionKey& other) const
{
    return !(*this == other);
}

MemoryUsageBreakdown PartitionLayer::memoryUsage() const
{
    MemoryUsageBreakdown result;
    result.add("object", {sizeof(PartitionLayer), sizeof(PartitionLayer)});
    result.add("string-pool-id", stringMemoryUsage(stringPoolId_));
    result.add("map-id", stringMemoryUsage(mapId_));
    if (error_) {
        result.add("error", stringMemoryUsage(*error_));
    }
    result.add("info-json", jsonMemoryUsage(info_));
    if (legalInfo_) {
        result.add("legal-info", stringMemoryUsage(*legalInfo_));
    }
    return result;
}

PartitionLayer::PartitionLayer(
    const PartitionId& id,
    std::string stringPoolId,
    std::string mapId,
    const std::shared_ptr<LayerInfo>& info)
    : partitionId_(id),
      stringPoolId_(std::move(stringPoolId)),
      mapId_(std::move(mapId)),
      layerInfo_(info),
      timestamp_(std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch()))
{
    if (id.kind() != info->partitionKind_)
        throw std::invalid_argument("Partition kind does not match layer metadata.");
}

PartitionLayer::PartitionLayer(
    const std::vector<uint8_t>& input,
    const LayerInfoResolveFun& layerInfoResolveFun,
    size_t* bytesRead)
    : partitionId_()
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

    s.object(partitionId_);
    if (partitionId_.kind() != layerInfo_->partitionKind_)
        throw std::invalid_argument("Partition kind does not match layer metadata.");
    s.text1b(stringPoolId_, std::numeric_limits<uint32_t>::max());

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
            "Failed to read PartitionLayer: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error())));
    }
    if (bytesRead != nullptr) {
        *bytesRead = s.adapter().currentReadPos();
    }
}

TileId PartitionLayer::tileId() const
{
    return partitionId_.tileId();
}

std::string PartitionLayer::stringPoolId() const
{
    return stringPoolId_;
}

std::string PartitionLayer::mapId() const
{
    return mapId_;
}

std::shared_ptr<LayerInfo> PartitionLayer::layerInfo() const
{
    return layerInfo_;
}

std::optional<std::string> PartitionLayer::error() const
{
    return error_;
}

std::chrono::time_point<std::chrono::system_clock> PartitionLayer::timestamp() const
{
    return timestamp_;
}

std::optional<std::chrono::milliseconds> PartitionLayer::ttl() const
{
    return ttl_;
}

Version PartitionLayer::mapVersion() const
{
    return mapVersion_;
}

nlohmann::json PartitionLayer::info() const
{
    return info_;
}

void PartitionLayer::setInfo(nlohmann::json const& info)
{
    info_ = info;
}

std::optional<std::string> PartitionLayer::legalInfo() const
{
    return legalInfo_;
}

void PartitionLayer::setTileId(const TileId& id)
{
    if (layerInfo_->partitionKind_ != PartitionKind::Tile)
        throw std::logic_error("Cannot set a tile ID on an object layer.");
    partitionId_ = id;
}

void PartitionLayer::setStringPoolId(const std::string& id)
{
    stringPoolId_ = id;
}

void PartitionLayer::setMapId(const std::string& id)
{
    mapId_ = id;
}

void PartitionLayer::setLayerInfo(const std::shared_ptr<LayerInfo>& info)
{
    layerInfo_ = info;
}

void PartitionLayer::setError(const std::optional<std::string>& err)
{
    error_ = err;
}

std::optional<int> PartitionLayer::errorCode() const
{
    return errorCode_;
}

void PartitionLayer::setErrorCode(const std::optional<int>& code)
{
    errorCode_ = code;
}

void PartitionLayer::setTimestamp(const std::chrono::time_point<std::chrono::system_clock>& ts)
{
    timestamp_ = ts;
}

void PartitionLayer::setTtl(const std::optional<std::chrono::milliseconds>& timeToLive)
{
    ttl_ = timeToLive;
}

void PartitionLayer::setMapVersion(Version v)
{
    mapVersion_ = v;
}

void PartitionLayer::setInfo(std::string const& k, nlohmann::json const& v)
{
    info_[k] = v;
}

void PartitionLayer::setLegalInfo(std::optional<std::string> legalInfoString)
{
    legalInfo_ = std::move(legalInfoString);
}

void PartitionLayer::setLoadStateCallback(LoadStateCallback cb)
{
    onLoadStateChanged_ = std::move(cb);
}

void PartitionLayer::setLoadState(LoadState state)
{
    if (onLoadStateChanged_) {
        onLoadStateChanged_(state);
    }
}

tl::expected<void, simfil::Error> PartitionLayer::write(std::ostream& outputStream)
{
    using namespace std::chrono;
    using namespace nlohmann;

    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);
    s.text1b(mapId_, std::numeric_limits<uint32_t>::max());
    s.text1b(layerInfo_->layerId_, std::numeric_limits<uint32_t>::max());
    s.object(mapVersion_);
    s.object(partitionId_);
    s.text1b(stringPoolId_, std::numeric_limits<uint32_t>::max());
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

MapPartitionKey PartitionLayer::id() const
{
    return MapPartitionKey(*this);
}

nlohmann::json PartitionLayer::toJson() const
{
    return {};
}

} // namespace mapget
