#pragma once

#include <string>
#include <mutex>
#include <shared_mutex>

#include "mapget/model/tileid.h"
#include "mapget/model/info.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/stream.h"

namespace mapget
{

/**
 * Abstract class which defines the behavior of a mapget cache,
 * which can store and recover the output of any mapget DataSource
 * for a specific MapTileKey. Any implementation must override the
 * methods (get|put)TileLayer and (get|put)Fields.
 */
class Cache : public TileLayerStream::CachedFieldsProvider, public std::enable_shared_from_this<Cache>
{
    friend class DataSource;

public:
    using Ptr = std::shared_ptr<Cache>;
    // The following methods are already implemented,
    // they forward to the virtual methods on-demand.

    /**
     * Used by DataSource to upsert a cached TileFeatureLayer.
     * Triggers putTileLayerBlob and putFieldsBlob internally.
     */
    void putTileFeatureLayer(TileFeatureLayer::Ptr const& l);

    /** Used by DataSource to retrieve a cached TileFeatureLayer. */
    TileFeatureLayer::Ptr getTileFeatureLayer(MapTileKey const& tileKey, DataSourceInfo const& dataSource);

    /** Override for CachedFieldsProvider::getFieldDict() */
    std::shared_ptr<Fields> getFieldDict(std::string_view const&) override;

    // You need to implement these methods:

    /** Abstract: Retrieve a TileLayer blob for a MapTileKey. */
    virtual std::optional<std::string> getTileLayerBlob(MapTileKey const& k) = 0;

    /** Abstract: Upsert (update or insert) a TileLayer blob. */
    virtual void putTileLayerBlob(MapTileKey const& k, std::string const& v) = 0;

    /** Abstract: Retrieve a fields-dict-blob for a sourceNodeId. */
    virtual std::optional<std::string> getFieldsBlob(std::string_view const& sourceNodeId) = 0;

    /** Abstract: Upsert (update or insert) a fields-dict blob. */
    virtual void putFieldsBlob(std::string_view const& sourceNodeId, std::string const& v) = 0;

    // Override this method if your cache implementation has special stats.

    /**
     * Get diagnostic statistics. The default implementation returns the following:
     * `cache-hits`: Number of fulfilled cache requests.
     * `cache-misses`: Number of cache misses (unfulfilled cache requests).
     * `loaded-field-dicts`: Number of fields-dictionaries currently held in memory.
     */
    virtual nlohmann::json getStatistics() const;


protected:
    // Used by DataSource::cachedFieldsOffset()
    simfil::FieldId cachedFieldsOffset(std::string const& nodeId);

    // Mutex for fieldCacheOffsets_
    std::mutex fieldCacheOffsetMutex_;
    TileLayerStream::FieldOffsetMap fieldCacheOffsets_;

    // Statistics
    int64_t cacheHits_ = 0;
    int64_t cacheMisses_ = 0;
};

}
