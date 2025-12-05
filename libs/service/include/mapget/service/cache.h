#pragma once

#include <string>
#include <mutex>
#include <chrono>
#include <optional>

#include "mapget/model/info.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/stream.h"

namespace mapget
{

/**
 * Abstract class which defines the behavior of a mapget cache,
 * which can store and recover the output of any mapget DataSource
 * for a specific MapTileKey. Any implementation must override the
 * methods (get|put)TileLayerBlob and (get|put)StringPoolBlob.
 */
class Cache : public TileLayerStream::StringPoolCache, public std::enable_shared_from_this<Cache>
{
    friend class DataSource;

public:
    struct LookupResult {
        TileLayer::Ptr tile;
        std::optional<std::chrono::system_clock::time_point> expiredAt;
    };

    using Ptr = std::shared_ptr<Cache>;
    // The following methods are already implemented,
    // they forward to the virtual methods on-demand.

    /**
     * Used by DataSource to upsert a cached TileLayer.
     * Triggers putTileLayerBlob and putStringPoolBlob internally.
     */
    void putTileLayer(TileLayer::Ptr const& l);

    /** Used by DataSource to retrieve a cached TileLayer. */
    LookupResult getTileLayer(MapTileKey const& tileKey, DataSourceInfo const& dataSource);

    /** Override for CachedStringPoolCache::getStringPool() */
    std::shared_ptr<StringPool> getStringPool(std::string_view const&) override;

    // You need to implement these methods:

    /** Abstract: Retrieve a TileLayer blob for a MapTileKey. */
    virtual std::optional<std::string> getTileLayerBlob(MapTileKey const& k) = 0;

    /** Abstract: Upsert (update or insert) a TileLayer blob. */
    virtual void putTileLayerBlob(MapTileKey const& k, std::string const& v) = 0;

    /** Abstract: Retrieve a string-pool blob for a sourceNodeId. */
    virtual std::optional<std::string> getStringPoolBlob(std::string_view const& sourceNodeId) = 0;

    /** Abstract: Upsert (update or insert) a string-pool blob. */
    virtual void putStringPoolBlob(std::string_view const& sourceNodeId, std::string const& v) = 0;

    // Override this method if your cache implementation has special stats.

    /**
     * Get diagnostic statistics. The default implementation returns the following:
     * `cache-hits`: Number of fulfilled cache requests.
     * `cache-misses`: Number of cache misses (unfulfilled cache requests).
     * `loaded-string-pools`: Number of string pools currently held in memory.
     */
    virtual nlohmann::json getStatistics() const;


protected:
    // Used by DataSource::cachedStringPoolOffset()
    simfil::StringId cachedStringPoolOffset(std::string const& nodeId);

    // Mutex for stringPoolOffsets_
    std::mutex stringPoolOffsetMutex_;
    TileLayerStream::StringPoolOffsetMap stringPoolOffsets_;

    // Statistics
    int64_t cacheHits_ = 0;
    int64_t cacheMisses_ = 0;
};

}
