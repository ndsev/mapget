#pragma once

#include "service.h"

#include <shared_mutex>

namespace mapget::detail
{

/** Immutable service-owned view of one ready datasource. */
struct RegisteredDataSource
{
    using Ptr = std::shared_ptr<RegisteredDataSource>;

    /** Datasource implementation used for tile and auxiliary API calls. */
    DataSource::Ptr dataSource;

    /** Detached metadata snapshot shared by the catalog, scheduler, and readers. */
    std::shared_ptr<DataSourceInfo const> info;

    /** Catalog identity used to honor an optional request source selector. */
    std::string sourceId;

    /** Capacity-based size of the canonical detached metadata snapshot. */
    simfil::MemoryUsage metadataMemory;
};

/**
 * Thread-safe registry of ready datasource instances.
 *
 * Startup and failure rows remain in Service::Impl because those rows exist
 * before a RegisteredDataSource can be created. The registry owns only the
 * immutable ready-state view shared by all service subsystems.
 */
class DataSourceRegistry
{
public:
    /** Validate, detach, and register one datasource. */
    RegisteredDataSource::Ptr
    add(DataSource::Ptr dataSource, std::optional<std::string> sourceId = {});

    /** Remove a datasource and return its former registration, if present. */
    RegisteredDataSource::Ptr remove(DataSource::Ptr const& dataSource);

    /** Return stable shared handles in registration order. */
    [[nodiscard]] std::vector<RegisteredDataSource::Ptr> snapshot() const;

    /** Return primary sources that match a request's map, layer, and optional source ID. */
    [[nodiscard]] std::vector<RegisteredDataSource::Ptr> matchingPrimarySources(
        std::string_view mapId,
        std::string_view layerId,
        std::optional<std::string> const& sourceId = {}) const;

    /** Return add-on sources in registration order. */
    [[nodiscard]] std::vector<RegisteredDataSource::Ptr> addOnSources() const;

    /** Return a detached authorized metadata list for the legacy info API. */
    [[nodiscard]] std::vector<DataSourceInfo>
    infos(std::optional<AuthHeaders> const& clientHeaders) const;

    /** Return the number of ready sources. */
    [[nodiscard]] size_t size() const;

    /** Return whether no ready source is registered. */
    [[nodiscard]] bool empty() const;

private:
    mutable std::shared_mutex mutex_;
    std::vector<RegisteredDataSource::Ptr> sources_;
    uint64_t nextRuntimeSourceId_ = 0;
};

/** Create a detached LayerInfo copy without datasource-owned schema emitters. */
[[nodiscard]] std::shared_ptr<LayerInfo> cloneLayerInfo(LayerInfo const& info);

/** Create the canonical immutable metadata snapshot for a ready datasource. */
[[nodiscard]] DataSourceInfo cloneDataSourceInfo(DataSourceInfo const& info);

}  // namespace mapget::detail
