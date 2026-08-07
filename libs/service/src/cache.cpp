#include "cache.h"
#include "mapget/log.h"

#include "fmt/format.h"

#include <exception>

namespace mapget
{

std::shared_ptr<StringPool> Cache::getStringPool(const std::string_view& stringPoolId)
{
    {
        std::shared_lock stringPoolReadLock(stringPoolCacheMutex_);
        auto it = stringPoolPerStringPoolId_.find(stringPoolId);
        if (it != stringPoolPerStringPoolId_.end())
            return it->second;
    }

    {
        std::unique_lock stringPoolWriteLock(stringPoolCacheMutex_, std::defer_lock);
        std::unique_lock stringPoolOffsetsWriteLock(stringPoolOffsetMutex_, std::defer_lock);
        std::lock(stringPoolWriteLock, stringPoolOffsetsWriteLock);

        // Was the string pool inserted already now?
        auto it = stringPoolPerStringPoolId_.find(stringPoolId);
        if (it != stringPoolPerStringPoolId_.end())
            return it->second;

        // Load/insert the string pool.
        std::shared_ptr<StringPool> stringPool = std::make_shared<StringPool>(stringPoolId);
        auto cachedStringsBlob = getStringPoolBlob(stringPoolId);
        if (cachedStringsBlob) {
            std::vector<uint8_t> bytes(cachedStringsBlob->begin(), cachedStringsBlob->end());

            // First, read the header and the datasource node id.
            // These must match what we expect.
            TileLayerStream::MessageType streamMessageType;
            uint32_t streamMessageSize;
            size_t headerBytesRead = 0;
            if (!TileLayerStream::Reader::readMessageHeader(
                    std::span<const uint8_t>(bytes),
                    streamMessageType,
                    streamMessageSize,
                    &headerBytesRead)) {
                raise("Stream header error while parsing string pool.");
            }
            if (headerBytesRead + streamMessageSize > bytes.size()) {
                raise("Invalid StringPool message size while parsing cache blob.");
            }

            std::vector<uint8_t> payload(
                bytes.begin() + static_cast<std::ptrdiff_t>(headerBytesRead),
                bytes.begin() + static_cast<std::ptrdiff_t>(headerBytesRead + streamMessageSize));
            size_t stringPoolIdBytesRead = 0;
            auto streamDataSourceStringPoolId = StringPool::readDataSourceStringPoolId(payload, 0, &stringPoolIdBytesRead);
            if (streamMessageType != TileLayerStream::MessageType::StringPool || streamDataSourceStringPoolId != stringPoolId) {
                raise("Stream header error while parsing string pool.");
            }

            // Now, actually read the string pool message.
            auto readResult = stringPool->read(payload, stringPoolIdBytesRead);
            if (!readResult) {
                raise(readResult.error().message);
            }
            stringPoolOffsets_.emplace(stringPoolId, stringPool->highest());
        }
        auto [itNew, _] = stringPoolPerStringPoolId_.emplace(stringPoolId, stringPool);
        return itNew->second;
    }
}

nlohmann::json Cache::getStatistics() const {
    int64_t loadedStringPools = 0;
    int64_t stringPoolEntries = 0;
    int64_t stringPoolPayloadBytes = 0;
    MemoryUsageBreakdown memory;
    {
        std::shared_lock lock(stringPoolCacheMutex_);
        loadedStringPools =
            static_cast<int64_t>(stringPoolPerStringPoolId_.size());
        memory.add("loaded-string-pool-index", {
            stringPoolPerStringPoolId_.size() *
                sizeof(decltype(stringPoolPerStringPoolId_)::value_type),
            stringPoolPerStringPoolId_.size() *
                (sizeof(decltype(stringPoolPerStringPoolId_)::value_type) + 3 * sizeof(void*)),
        });
        for (auto const& [id, pool] : stringPoolPerStringPoolId_) {
            memory.add("loaded-string-pool-ids", stringMemoryUsage(id));
            if (!pool) {
                continue;
            }
            memory.add("loaded-string-pool-objects", {
                sizeof(simfil::StringPool),
                sizeof(simfil::StringPool),
            });
            stringPoolEntries +=
                static_cast<int64_t>(pool->size());
            stringPoolPayloadBytes +=
                static_cast<int64_t>(pool->bytes());
            memory.add("loaded-string-pools", pool->memoryUsage());
        }
    }
    {
        std::lock_guard lock(stringPoolOffsetMutex_);
        memory.add("string-pool-offset-index", {
            stringPoolOffsets_.size() * sizeof(decltype(stringPoolOffsets_)::value_type),
            stringPoolOffsets_.bucket_count() * sizeof(void*) +
                stringPoolOffsets_.size() *
                    (sizeof(decltype(stringPoolOffsets_)::value_type) + 2 * sizeof(void*)),
        });
        for (auto const& [id, _] : stringPoolOffsets_) {
            memory.add("string-pool-offset-ids", stringMemoryUsage(id));
        }
    }
    return {
        {"cache-hits", cacheHits_.load()},
        {"cache-misses", cacheMisses_.load()},
        {"loaded-string-pools", loadedStringPools},
        {"string-pool-entries", stringPoolEntries},
        {"string-pool-payload-bytes", stringPoolPayloadBytes},
        {"memory", memory.toJson()}
    };
}

Cache::LookupResult Cache::getTileLayer(const MapTileKey& tileKey, DataSourceInfo const& dataSource)
{
    LookupResult result;
    auto tileBlob = getTileLayerBlob(tileKey);
    if (!tileBlob) {
        cacheMisses_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    TileLayer::Ptr tile;
    TileLayerStream::Reader tileReader(
        [&dataSource, &tileKey](auto&& mapId, auto&& layerId) {
            if (dataSource.mapId_ != mapId) {
                raiseFmt(
                    "Encountered unexpected map id '{}' in cache for tile {:0x}, expected '{}'",
                    mapId,
                    tileKey.tileId_.value(),
                    dataSource.mapId_);
            }
            return dataSource.getLayer(std::string(layerId));
        },
        [&](auto&& parsedLayer){tile = parsedLayer;},
        shared_from_this());

    try {
        tileReader.read(*tileBlob);
    }
    catch (std::exception const& e) {
        log().warn(
            "Ignoring unreadable cache entry for {}: {}",
            tileKey.toString(),
            e.what());
        cacheMisses_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    if (tile) {
        auto ttl = tile->ttl();
        if (ttl && ttl->count() > 0) {
            auto expiresAt = tile->timestamp() + *ttl;
            if (std::chrono::system_clock::now() > expiresAt) {
                log().debug("Cache entry expired for {}", tileKey.toString());
                cacheMisses_.fetch_add(1, std::memory_order_relaxed);
                result.expiredAt = expiresAt;
                return result;
            }
        }
        cacheHits_.fetch_add(1, std::memory_order_relaxed);
        log().debug("Returned tile from cache: {}", tileKey.tileId_.value());
        result.tile = tile;
    }
    return result;
}

void Cache::invalidateMap(std::string_view mapId)
{
    std::vector<MapTileKey> keys;
    forEachTileLayerBlob(
        [&](MapTileKey const& key, std::string const&) {
            if (key.mapId_ == mapId) {
                keys.push_back(key);
            }
        });
    for (auto const& key : keys) {
        eraseTileLayerBlob(key);
    }
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
                putStringPoolBlob(l->stringPoolId(), msg);
        },
        stringPoolOffsets_,
        /* differentialStringUpdates= */ false);
    log().debug("Writing tile layer to cache: {}", MapTileKey(*l).toString());
    tileWriter.write(l);
}

simfil::StringId Cache::cachedStringPoolOffset(std::string const& stringPoolId)
{
    if (stringPoolId.empty()) {
        raise("Tried to query cached string pool offset for empty node ID!");
    }
    std::unique_lock stringPoolOffsetLock(stringPoolOffsetMutex_);
    auto it = stringPoolOffsets_.find(stringPoolId);
    if (it != stringPoolOffsets_.end()) {
        log().trace("Cached string pool offset for {}: {}", stringPoolId, it->second);
        return it->second;
    }
    return 0;
}

}
