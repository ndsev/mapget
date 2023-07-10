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
 * following methods:
 *
 *   optional<string> getTileLayer(MapTileKey const& k)
 *   void putTileLayer(MapTileKey const& k, string const& v)
 *   optional<string> getFields(string const& sourceNodeId)
 *   void putFields(string const& sourceNodeId, std::string v)
 */
class Cache : public TileLayerStream::CachedFieldsProvider, public std::enable_shared_from_this<Cache>
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
    TileFeatureLayer::Ptr getTileFeatureLayer(MapTileKey const& k, DataSourceInfo const& i);

    /** Used by DataSource to upsert a cached TileFeatureLayer. */
    void putTileFeatureLayer(TileFeatureLayer::Ptr const& l);

protected:
    // Override for CachedFieldsProvider::operator()
    std::shared_ptr<Fields> operator() (std::string_view const&) override;

    // Used by DataSource::cachedFieldsOffset()
    simfil::FieldId cachedFieldsOffset(std::string const& nodeId);

    // Mutex for fieldCacheOffsets_
    std::mutex fieldCacheOffsetMutex_;
    TileLayerStream::FieldOffsetMap fieldCacheOffsets_;
};

}
