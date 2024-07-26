#include "cache.h"
#include "mapget/log.h"

#include "fmt/format.h"

namespace mapget
{

std::shared_ptr<StringPool> Cache::getFieldDict(const std::string_view& nodeId)
{
    {
        std::shared_lock fieldCacheReadLock(fieldCacheMutex_);
        auto it = fieldsPerNodeId_.find(nodeId);
        if (it != fieldsPerNodeId_.end())
            return it->second;
    }

    {
        std::unique_lock fieldCacheWriteLock(fieldCacheMutex_, std::defer_lock);
        std::unique_lock fieldCacheOffsetsWriteLock(fieldCacheOffsetMutex_, std::defer_lock);
        std::lock(fieldCacheWriteLock, fieldCacheOffsetsWriteLock);

        // Was the Fields dict inserted already now?
        auto it = fieldsPerNodeId_.find(nodeId);
        if (it != fieldsPerNodeId_.end())
            return it->second;

        // Load/insert the Fields dict.
        std::shared_ptr<StringPool> cachedFields = std::make_shared<StringPool>(nodeId);
        auto cachedFieldsBlob = getFieldsBlob(nodeId);
        if (cachedFieldsBlob) {
            // Read the fields from the stream.
            std::stringstream stream;
            stream << *cachedFieldsBlob;

            // First, read the header and the datasource node id.
            // These must match what we expect.
            TileLayerStream::MessageType streamMessageType;
            uint32_t streamMessageSize;
            TileLayerStream::Reader::readMessageHeader(stream, streamMessageType, streamMessageSize);
            auto streamDataSourceNodeId = StringPool::readDataSourceNodeId(stream);
            if (streamMessageType != TileLayerStream::MessageType::Fields || streamDataSourceNodeId != nodeId) {
                raise("Stream header error while parsing fields from cache.");
            }

            // Now, actually read the fields message.
            cachedFields->read(stream);
            fieldCacheOffsets_.emplace(nodeId, cachedFields->highest());
        }
        auto [itNew, _] = fieldsPerNodeId_.emplace(nodeId, cachedFields);
        return itNew->second;
    }
}

nlohmann::json Cache::getStatistics() const {
    return {
        {"cache-hits", cacheHits_},
        {"cache-misses", cacheMisses_},
        {"loaded-field-dicts", (int64_t)fieldDictOffsets().size()}
    };
}

TileLayer::Ptr Cache::getTileLayer(const MapTileKey& tileKey, DataSourceInfo const& dataSource)
{
    auto tileBlob = getTileLayerBlob(tileKey);
    if (!tileBlob) {
        ++cacheMisses_;
        return nullptr;
    }
    TileLayer::Ptr result;
    TileLayerStream::Reader tileReader(
        [&dataSource, &tileKey](auto&& mapId, auto&& layerId) {
            if (dataSource.mapId_ != mapId) {
                raiseFmt(
                    "Encountered unexpected map id '{}' in cache for tile {:0x}, expected '{}'",
                    mapId,
                    tileKey.tileId_.value_,
                    dataSource.mapId_);
            }
            return dataSource.getLayer(std::string(layerId));
        },
        [&](auto&& parsedLayer){result = parsedLayer;},
        shared_from_this());

    tileReader.read(*tileBlob);
    ++cacheHits_;
    log().debug("Returned tile from cache: {}", tileKey.tileId_.value_);
    return result;
}

void Cache::putTileLayer(TileLayer::Ptr const& l)
{
    std::unique_lock fieldsOffsetLock(fieldCacheOffsetMutex_);
    TileLayerStream::Writer tileWriter(
        [&l, this](auto&& msg, auto&& msgType)
        {
            if (msgType == TileLayerStream::MessageType::TileFeatureLayer ||
                msgType == TileLayerStream::MessageType::TileSourceDataLayer)
                putTileLayerBlob(MapTileKey(*l), msg);
            else if (msgType == TileLayerStream::MessageType::Fields)
                putFieldsBlob(l->nodeId(), msg);
        },
        fieldCacheOffsets_,
        /* differentialFieldUpdates = */ false);
    log().debug("Writing tile layer to cache: {}", MapTileKey(*l).toString());
    tileWriter.write(l);
}

simfil::StringId Cache::cachedFieldsOffset(std::string const& nodeId)
{
    if (nodeId.empty()) {
        raise("Tried to query cached fields offset for empty node ID!");
    }
    std::unique_lock fieldsOffsetLock(fieldCacheOffsetMutex_);
    auto it = fieldCacheOffsets_.find(nodeId);
    if (it != fieldCacheOffsets_.end()) {
        log().trace("Cached fields offset for {}: {}", nodeId, it->second);
        return it->second;
    }
    return 0;
}

}
