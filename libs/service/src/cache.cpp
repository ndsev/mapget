#include "cache.h"

#include "stx/format.h"
#include "stx/string.h"

namespace mapget
{

MapTileKey::MapTileKey(const std::string& str)
{
    auto parts = stx::split(str, ":", false);
    if (parts.size() < 4)
        throw std::runtime_error(stx::format("Invalid cache tile id: {}", str));
    layer_ = nlohmann::json(parts[0]).get<LayerType>();
    mapId_ = parts[1];
    layerId_ = parts[2];
    tileId_ = TileId(std::stoull(str, nullptr, 16));
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
    return stx::format(
        "{}:{}:{}:{:x}",
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

std::shared_ptr<Fields> Cache::operator()(const std::string_view& nodeId)
{
    std::string nodeIdStr{nodeId};

    {
        std::shared_lock fieldCacheReadLock(fieldCacheMutex_);
        auto it = fieldsPerNodeId_.find(nodeIdStr);
        if (it != fieldsPerNodeId_.end())
            return it->second;
    }

    {
        std::unique_lock fieldCacheWriteLock(fieldCacheMutex_, std::defer_lock);
        std::unique_lock fieldCacheOffsetsWriteLock(fieldCacheOffsetMutex_, std::defer_lock);
        std::lock(fieldCacheWriteLock, fieldCacheOffsetsWriteLock);

        // Was it inserted already now?
        auto it = fieldsPerNodeId_.find(nodeIdStr);
        if (it != fieldsPerNodeId_.end())
            return it->second;

        // Load/Insert it
        std::shared_ptr<Fields> cachedFields = std::make_shared<Fields>(nodeIdStr);
        auto cachedFieldsBlob = getFields(nodeIdStr);
        if (cachedFieldsBlob) {
            std::stringstream stream;
            stream << *cachedFieldsBlob;
            Fields::readDataSourceNodeId(stream);
            cachedFields->read(stream);
            fieldCacheOffsets_.emplace(nodeIdStr, cachedFields->highest());
        }
        auto [itNew, _] = fieldsPerNodeId_.emplace(nodeIdStr, cachedFields);
        return itNew->second;
    }
}

std::shared_ptr<TileFeatureLayer> Cache::getTileFeatureLayer(const MapTileKey& k, DataSourceInfo const& i)
{
    auto tileBlob = getTileLayer(k);
    if (!tileBlob)
        return nullptr;
    std::stringstream inputStream;
    inputStream << *tileBlob;
    return std::make_shared<TileFeatureLayer>(
        inputStream,
        [&i, &k](auto&& mapId, auto&& layerId){
            if (i.mapId_ != mapId)
                throw std::runtime_error(stx::format(
                    "Encountered unexpected map id '{}' in cache for tile {:x}, expected '{}'",
                    mapId, k.tileId_.value_, i.mapId_));
            return i.getLayer(std::string(layerId));
        },
        [&](auto&& nodeId){return (*this)(nodeId);});
}

void Cache::putTileFeatureLayer(std::shared_ptr<TileFeatureLayer> const& l)
{
    std::unique_lock fieldsOffsetLock(fieldCacheOffsetMutex_);
    TileLayerStream::Writer tileWriter(
        [&l, this](auto&& msg, auto&& msgType)
        {
            if (msgType == TileLayerStream::MessageType::TileFeatureLayer)
                putTileLayer(MapTileKey(*l), msg);
            else if (msgType == TileLayerStream::MessageType::TileFeatureLayer)
                putFields(l->nodeId(), msg);
        },
        fieldCacheOffsets_);
    tileWriter.write(l);
}

simfil::FieldId Cache::cachedFieldsOffset(std::string const& nodeId)
{
    std::unique_lock fieldsOffsetLock(fieldCacheOffsetMutex_);
    auto it = fieldCacheOffsets_.find(nodeId);
    if (it != fieldCacheOffsets_.end())
        return it->second;
    return 0;
}

}
