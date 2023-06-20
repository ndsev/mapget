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

/** Struct which represents the id of a unique cached map tile layer.*/
struct MapTileKey
{
    // The tile's data type
    LayerType layer_ = LayerType::Features;

    // The tile's associated map
    std::string mapId_;

    // The tile's associated map layer id
    std::string layerId_;

    // The tile's associated map tile id
    TileId tileId_;

    /** Constructor to parse the key from a string, as returned by toString. */
    explicit MapTileKey(std::string const& str);

    /** Constructor to create the cache key for any TileLayer object. */
    explicit MapTileKey(TileLayer const& data);

    /** Allow default ctor. */
    MapTileKey() = default;

    /** Convert the key to a string. */
    [[nodiscard]] std::string toString() const;

    /** Operator <, allows this struct to be used as an std::map key. */
    bool operator<(MapTileKey const& other) const;
};

/**
 * Abstract class which defines the behavior of a mapget cache,
 * which can store and recover the output of any mapget DataSource
 * for a specific MapTileKey. Any implementation must override the
 * following methods:
 *
 *   optional<string> getTileLayer(MapTileKey const& k)
 *   void putTileLayer(MapTileKey const& k, string const& v)
 *   optional<string> getFields(string const& sourceNodeId)
 *   void putFields(string const& sourceNodeId, std::string v)
 */
class Cache : public TileLayerStream::CachedFieldsProvider
{
    friend class DataSource;

public:
    using Ptr = std::shared_ptr<Cache>;

    // You need to implement these methods:

    /** Abstract: Retrieve a TileLayer blob for a MapTileKey. */
    virtual std::optional<std::string> getTileLayer(MapTileKey const& k) = 0;

    /** Abstract: Upsert (Update or Insert) a TileLayer blob. */
    virtual void putTileLayer(MapTileKey const& k, std::string const& v) = 0;

    /** Abstract: Retrieve a Fields-dict-blob for a sourceNodeId. */
    virtual std::optional<std::string> getFields(std::string const& sourceNodeId) = 0;

    /** Abstract: Upsert (Update or Insert) a Fields-dict blob. */
    virtual void putFields(std::string const& sourceNodeId, std::string const& v) = 0;

    // The following methods are already implemented,
    // they forward to the above methods on-demand.

    /** Used by DataSource to retrieve a cached TileFeatureLayer. */
    std::shared_ptr<TileFeatureLayer> getTileFeatureLayer(MapTileKey const& k, DataSourceInfo const& i);

    /** Used by DataSource to upsert a cached TileFeatureLayer. */
    void putTileFeatureLayer(std::shared_ptr<TileFeatureLayer> const& l);

protected:
    // Override for CachedFieldsProvider::operator()
    std::shared_ptr<Fields> operator() (std::string_view const&) override;

    // Used by DataSource::cachedFieldsOffset()
    simfil::FieldId cachedFieldsOffset(std::string const& nodeId);

    // Mutex for the inherited fieldsPerNodeId_
    std::shared_mutex fieldCacheMutex_;

    // Mutex for fieldCacheOffsets_
    std::mutex fieldCacheOffsetMutex_;
    TileLayerStream::FieldOffsetMap fieldCacheOffsets_;
};

}
