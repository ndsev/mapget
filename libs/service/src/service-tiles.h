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
        PartitionLayer::Ptr cachedLayer);

    /** Resolve the tile and process every coalesced consumer on this worker. */
    void run() noexcept;

private:
    ServiceScheduler& scheduler_;
    std::shared_ptr<SourceConcurrency> source_;
    RegisteredDataSource::Ptr registration_;
    std::shared_ptr<TileLoadState> state_;
    PartitionLayer::Ptr cachedLayer_;
};

/**
 * Merge all matching add-on datasource tiles into one freshly loaded base tile.
 *
 * Add-ons reuse the worker executing the base tile and therefore do not consume
 * independent datasource permits. Missing, errored or incompatible contributions
 * add warnings and apply their source's error lifetime to the partial result.
 * The earliest finite contributor expiry wins; zero never cancels another
 * contributor's deadline. Exceptions from loading/cloning, including cancellation,
 * propagate instead and fail the enclosing tile job.
 */
void loadAddOnTiles(
    PartitionFeatureLayer::Ptr const& baseTile,
    RegisteredDataSource const& baseSource,
    Cache::Ptr& cache,
    std::optional<std::chrono::milliseconds> const& defaultTtl,
    std::chrono::milliseconds emptyTileTtl,
    std::chrono::milliseconds errorTileTtl,
    TileCancellationCheck const& isCancelled = {});

}  // namespace mapget::detail
