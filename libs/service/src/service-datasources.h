#pragma once

#include "mapget/service/service.h"

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

    /** Original provider binding; datasource conversion must never use composed domain IDs. */
    std::shared_ptr<DataSourceInfo const> nativeInfo;

    /** Exact add-on generations included in info; a running job never consults a newer registry. */
    std::vector<Ptr> addOns;

    /** Catalog identity used to honor an optional request source selector. */
    std::string sourceId;

    /** Capacity-based size of the canonical detached metadata snapshot. */
    simfil::MemoryUsage metadataMemory;

    /** A composed map is accessible only when every contributing provider permits access. */
    [[nodiscard]] bool authorized(std::optional<ClientAuthorization> const& headers) const;
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
    infos(std::optional<ClientAuthorization> const& clientAuthorization) const;

    /** Resolve an exact authorized schema binding without copying metadata or doing I/O. */
    [[nodiscard]] std::shared_ptr<MapSchema const> getSchema(
        std::string_view sourceId,
        std::string_view schemaId,
        std::optional<ClientAuthorization> const& clientAuthorization) const;

    /** Group authorized immutable owners; callbacks run outside locks and may abort traversal.
     * Recheck the pinned registration snapshot as one batch before publishing, rejecting a
     * concurrent replacement/removal with resource_unavailable_try_again instead of stale rows.
     */
    [[nodiscard]] nlohmann::json schemaInventory(
        std::optional<ClientAuthorization> const& clientAuthorization,
        std::function<bool(MapSchema const&)> const& include = {},
        std::function<void()> const& checkpoint = {},
        std::function<bool(std::string_view sourceId, std::string_view mapId)> const&
            includeSource = {}) const;

    /** Account unique current and retired pinned graphs; retain them only for this measurement. */
    [[nodiscard]] MemoryUsageBreakdown schemaMemoryUsage() const;

    /** Return the number of ready sources. */
    [[nodiscard]] size_t size() const;

    /** Return whether no ready source is registered. */
    [[nodiscard]] bool empty() const;

private:
    /** Rebuild changed primary snapshots transactionally before replacing the ready registry. */
    void compose(std::vector<RegisteredDataSource::Ptr>& sources) const;

    /** Called under mutex_ after publication; retain weak observations, never the graphs. */
    void rememberSchemas();
    mutable std::shared_mutex mutex_;
    std::vector<RegisteredDataSource::Ptr> sources_;
    mutable std::vector<std::weak_ptr<MapSchema const>> knownSchemas_;
    uint64_t nextRuntimeSourceId_ = 0;
};

/** Create a detached LayerInfo copy without datasource-owned schema emitters. */
[[nodiscard]] std::shared_ptr<LayerInfo> cloneLayerInfo(LayerInfo const& info);

/** Create the canonical immutable metadata snapshot for a ready datasource. */
[[nodiscard]] DataSourceInfo cloneDataSourceInfo(DataSourceInfo const& info);

}  // namespace mapget::detail
