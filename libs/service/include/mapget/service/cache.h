#pragma once

#include <string>
#include <mutex>
#include <chrono>
#include <optional>
#include <functional>
#include <atomic>

#include "mapget/model/info.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/stream.h"
#include "mapget/model/memory.h"

namespace mapget
{

/**
 * Abstract class which defines the behavior of a mapget cache,
 * which can store and recover the output of any mapget DataSource
 * for a specific MapPartitionKey. Any implementation must override the
 * methods (get|put)TileLayerBlob and (get|put)StringPoolBlob.
 */
class Cache : public TileLayerStream::StringPoolCache, public std::enable_shared_from_this<Cache>
{
    friend class DataSource;

public:
    /** A usable decoded layer, or a miss with the expired layer's deadline when known. */
    struct LookupResult
    {
        PartitionLayer::Ptr tile;
        std::optional<std::chrono::system_clock::time_point> expiredAt;
    };

    using Ptr = std::shared_ptr<Cache>;
    using TileBlobVisitor = std::function<void(const MapPartitionKey&, const std::string&)>;
    // The following methods are already implemented,
    // they forward to the virtual methods on-demand.

    /**
     * Used by DataSource to upsert a cached PartitionLayer.
     * Triggers putTileLayerBlob and putStringPoolBlob internally.
     */
    void putTileLayer(PartitionLayer::Ptr const& l);

    /**
     * Retrieve a cached PartitionLayer for a DataSource, decoding it with the
     * supplied datasource metadata and cached string pool.
     * Missing, unreadable and expired blobs return an empty tile; only expiry
     * sets expiredAt so the service can notify the datasource before reloading.
     */
    LookupResult getTileLayer(MapPartitionKey const& tileKey, DataSourceInfo const& dataSource);

    /**
     * Remove every cached tile for one map.
     *
     * Datasource and add-on lifecycle changes use this boundary because cache
     * keys deliberately do not contain catalog source identity.
     */
    void invalidateMap(std::string_view mapId);

    /** Override TileLayerStream::StringPoolCache::getStringPool() to reuse the source
     * dictionary in memory, loading its persisted blob on first access.
     */
    std::shared_ptr<StringPool> getStringPool(std::string_view const&) override;

    // You need to implement these methods:

    /** Abstract: Retrieve a PartitionLayer blob for a MapPartitionKey. */
    virtual std::optional<std::string> getTileLayerBlob(MapPartitionKey const& k) = 0;

    /** Abstract: Upsert (update or insert) a PartitionLayer blob. */
    virtual void putTileLayerBlob(MapPartitionKey const& k, std::string const& v) = 0;

    /** Abstract: Remove one PartitionLayer blob if present. */
    virtual void eraseTileLayerBlob(MapPartitionKey const& k) = 0;

    /**
     * Visit the stored blobs while the implementation may hold its cache lock.
     * The visitor must not re-enter this cache or retain references to the blob.
     * Collect keys first and mutate afterward, as invalidateMap() does.
     */
    virtual void forEachTileLayerBlob(const TileBlobVisitor& cb) const = 0;

    /** Abstract: Retrieve a string-pool blob for a sourceStringPoolId. */
    virtual std::optional<std::string> getStringPoolBlob(std::string_view const& sourceStringPoolId) = 0;

    /** Abstract: Upsert (update or insert) a string-pool blob. */
    virtual void putStringPoolBlob(std::string_view const& sourceStringPoolId, std::string const& v) = 0;

    // Override this method if your cache implementation has special stats.

    /**
     * Get diagnostic statistics. The default implementation returns the following:
     * `cache-hits`: Number of fulfilled cache requests.
     * `cache-misses`: Number of cache misses (unfulfilled cache requests).
     * `loaded-string-pools`: Number of string pools currently held in memory.
     */
    virtual nlohmann::json getStatistics() const;

protected:
    /** Used by DataSource::cachedStringPoolOffset(): return the highest persisted
     * dictionary ID, or zero when no dictionary was written yet.
     */
    simfil::StringId cachedStringPoolOffset(std::string const& stringPoolId);

    // Mutex for stringPoolOffsets_
    mutable std::mutex stringPoolOffsetMutex_;
    TileLayerStream::StringPoolOffsetMap stringPoolOffsets_;

    // Statistics
    std::atomic<int64_t> cacheHits_{0};
    std::atomic<int64_t> cacheMisses_{0};
};

}
