#include "cache.h"
#include "mapget/log.h"

#include "stx/format.h"

namespace mapget
{

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

TileFeatureLayer::Ptr Cache::getTileFeatureLayer(const MapTileKey& k, DataSourceInfo const& i)
{
    auto tileBlob = getTileLayer(k);
    if (!tileBlob)
        return nullptr;
    TileFeatureLayer::Ptr result;
    TileLayerStream::Reader tileReader(
        [&i, &k](auto&& mapId, auto&& layerId){
            if (i.mapId_ != mapId)
                throw logRuntimeError(stx::format(
                    "Encountered unexpected map id '{}' in cache for tile {:0x}, expected '{}'",
                    mapId, k.tileId_.value_, i.mapId_));
            return i.getLayer(std::string(layerId));
        },
        [&](auto&& parsedLayer){result = parsedLayer;},
        shared_from_this());
    tileReader.read(*tileBlob);
    return result;
}

void Cache::putTileFeatureLayer(TileFeatureLayer::Ptr const& l)
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
    log().debug("Writing tile layer to cache: {}", MapTileKey(*l).toString());
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
