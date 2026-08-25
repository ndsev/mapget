#pragma once

#include "service-scheduler.h"

namespace mapget::detail
{

/** One cache- or datasource-backed source tile executable by any service worker. */
class TileLoadJob final
{
public:
    /** Bind one selected source permit to one coalesced tile-load state. */
    TileLoadJob(
        ServiceScheduler& scheduler,
        std::shared_ptr<SourceConcurrency> source,
        std::shared_ptr<TileLoadState> state);

    /** Bind one cached source model to all requests coalesced for that key. */
    TileLoadJob(
        ServiceScheduler& scheduler,
        std::shared_ptr<TileLoadState> state,
        TileLayer::Ptr cachedLayer);

    /** Resolve the tile and process every coalesced consumer on this worker. */
    void run() noexcept;

private:
    ServiceScheduler& scheduler_;
    std::shared_ptr<SourceConcurrency> source_;
    std::shared_ptr<TileLoadState> state_;
    TileLayer::Ptr cachedLayer_;
};

/**
 * Merge all matching add-on datasource tiles into one freshly loaded base tile.
 *
 * Add-ons reuse the worker executing the base tile and therefore do not consume
 * independent datasource permits.
 */
void loadAddOnTiles(
    TileFeatureLayer::Ptr const& baseTile,
    RegisteredDataSource const& baseSource,
    DataSourceRegistry const& dataSources,
    Cache::Ptr& cache,
    std::optional<std::chrono::milliseconds> const& defaultTtl);

}  // namespace mapget::detail
