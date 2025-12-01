#include "cache.h"
#include "mapget/log.h"

#include "fmt/format.h"

namespace mapget
{

std::shared_ptr<StringPool> Cache::getStringPool(const std::string_view& nodeId)
{
    {
        std::shared_lock stringPoolReadLock(stringPoolCacheMutex_);
        auto it = stringPoolPerNodeId_.find(nodeId);
        if (it != stringPoolPerNodeId_.end())
            return it->second;
    }

    {
        std::unique_lock stringPoolWriteLock(stringPoolCacheMutex_, std::defer_lock);
        std::unique_lock stringPoolOffsetsWriteLock(stringPoolOffsetMutex_, std::defer_lock);
        std::lock(stringPoolWriteLock, stringPoolOffsetsWriteLock);

        // Was the string pool inserted already now?
        auto it = stringPoolPerNodeId_.find(nodeId);
        if (it != stringPoolPerNodeId_.end())
            return it->second;

        // Load/insert the string pool.
        std::shared_ptr<StringPool> stringPool = std::make_shared<StringPool>(nodeId);
        auto cachedStringsBlob = getStringPoolBlob(nodeId);
        if (cachedStringsBlob) {
            // Read the string pool from the stream.
            std::stringstream stream;
            stream << *cachedStringsBlob;

            // First, read the header and the datasource node id.
            // These must match what we expect.
            TileLayerStream::MessageType streamMessageType;
            uint32_t streamMessageSize;
            TileLayerStream::Reader::readMessageHeader(stream, streamMessageType, streamMessageSize);
            auto streamDataSourceNodeId = StringPool::readDataSourceNodeId(stream);
            if (streamMessageType != TileLayerStream::MessageType::StringPool || streamDataSourceNodeId != nodeId) {
                raise("Stream header error while parsing string pool.");
            }

            // Now, actually read the string pool message.
            stringPool->read(stream);
            stringPoolOffsets_.emplace(nodeId, stringPool->highest());
        }
        auto [itNew, _] = stringPoolPerNodeId_.emplace(nodeId, stringPool);
        return itNew->second;
    }
}

nlohmann::json Cache::getStatistics() const {
    return {
        {"cache-hits", cacheHits_},
        {"cache-misses", cacheMisses_},
        {"loaded-string-pools", (int64_t)stringPoolOffsets().size()}
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
    if (result) {
        auto ttl = result->ttl();
        if (ttl && ttl->count() > 0) {
            auto expiresAt = result->timestamp() + *ttl;
            if (std::chrono::system_clock::now() > expiresAt) {
                log().debug("Cache entry expired for {}", tileKey.toString());
                ++cacheMisses_;
                return nullptr;
            }
        }
        ++cacheHits_;
        log().debug("Returned tile from cache: {}", tileKey.tileId_.value_);
    }
    return result;
}

void Cache::putTileLayer(TileLayer::Ptr const& l)
{
    std::unique_lock stringPoolOffsetLock(stringPoolOffsetMutex_);
    TileLayerStream::Writer tileWriter(
        [&l, this](auto&& msg, auto&& msgType)
        {
            if (msgType == TileLayerStream::MessageType::TileFeatureLayer ||
                msgType == TileLayerStream::MessageType::TileSourceDataLayer)
                putTileLayerBlob(MapTileKey(*l), msg);
            else if (msgType == TileLayerStream::MessageType::StringPool)
                putStringPoolBlob(l->nodeId(), msg);
        },
        stringPoolOffsets_,
        /* differentialStringUpdates= */ false);
    log().debug("Writing tile layer to cache: {}", MapTileKey(*l).toString());
    tileWriter.write(l);
}

simfil::StringId Cache::cachedStringPoolOffset(std::string const& nodeId)
{
    if (nodeId.empty()) {
        raise("Tried to query cached string pool offset for empty node ID!");
    }
    std::unique_lock stringPoolOffsetLock(stringPoolOffsetMutex_);
    auto it = stringPoolOffsets_.find(nodeId);
    if (it != stringPoolOffsets_.end()) {
        log().trace("Cached string pool offset for {}: {}", nodeId, it->second);
        return it->second;
    }
    return 0;
}

}
