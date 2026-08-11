#pragma once

#include "service-scheduler.h"

namespace mapget::detail
{

/** Datasource-affine source-tile load executable by any service worker. */
class TileLoadJob final : public ServiceJob
{
public:
    /** Bind one selected source permit to one coalesced tile-load state. */
    TileLoadJob(
        ServiceScheduler& scheduler,
        std::shared_ptr<SourceConcurrency> source,
        std::shared_ptr<TileLoadState> state);

    /** Load, compose add-ons, apply TTL, and publish the tile. */
    void run() noexcept override;

    /** No-op because tile jobs are materialized directly onto a worker. */
    void discard() noexcept override;

    /** Materialized tile jobs remain executable even if all waiters detach. */
    [[nodiscard]] bool cancelled() const override;

    /** Return the per-source permit held until this job finishes. */
    [[nodiscard]] std::shared_ptr<SourceConcurrency> sourceAffinity() const override;

    /** Return the loaded map for queued-work invalidation. */
    [[nodiscard]] std::string_view mapId() const override;

    /** Classify this as source-affine tile work. */
    [[nodiscard]] ServiceJobKind kind() const override;

private:
    ServiceScheduler& scheduler_;
    std::shared_ptr<SourceConcurrency> source_;
    std::shared_ptr<TileLoadState> state_;
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
    Cache::Ptr& cache);

}  // namespace mapget::detail
