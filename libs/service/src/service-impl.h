#pragma once

#include "service-scheduler.h"

#include <algorithm>
#include <atomic>
#include <shared_mutex>

namespace mapget
{

/** Private composition root; subsystem methods are implemented in service-*.cpp files. */
struct Service::Impl
{
    /** Ready datasource state shared by request admission and worker jobs. */
    detail::DataSourceRegistry dataSources_;

    /** Homogeneous global worker pool and all request/in-flight scheduling state. */
    detail::ServiceScheduler scheduler_;

    /** Protects config-generation and catalog rows, including initializing sources. */
    mutable std::shared_mutex catalogMutex_;
    std::unique_ptr<DataSourceConfigService::Subscription> configSubscription_;
    std::vector<DataSource::Ptr> dataSourcesFromConfig_;

    /** Constructor-input hashes indexed by YAML row, including disabled rows. */
    std::vector<std::string> dataSourceConfigFingerprints_;
    size_t dataSourceConstructionFailed_ = 0;
    std::vector<DataSourceCatalogEntry> sourceCatalog_;
    /** Stable task identities survive unrelated watched edits and row reordering. */
    std::vector<uint64_t> dataSourceConstructionIds_;
    std::unordered_map<uint64_t, uint32_t> constructionRows_;
    uint64_t nextConstructionId_ = 0;
    uint64_t sourceCatalogRevision_ = 0;
    std::string sourceConfigStatus_ = "ok";
    std::string sourceConfigStatusMessage_;
    mutable std::condition_variable_any sourceCatalogReadyCv_;

    /** Joinable config-construction task with explicit portable cancellation. */
    struct ConstructionThread
    {
        uint64_t generation;
        /** Joinable constructor task retained until completion or shutdown. */
        std::thread thread;

        /** Completion flag used to prune finished threads without blocking. */
        std::shared_ptr<std::atomic_bool> done;

        /** Cooperative cancellation shared with DataSourceInitContext. */
        std::shared_ptr<std::atomic_bool> stopRequested;
    };

    std::vector<ConstructionThread> dataSourceConstructionThreads_;
    std::mutex constructionSlotMutex_;
    std::condition_variable_any constructionSlotCv_;
    size_t activeDataSourceConstructions_ = 0;
    size_t maxConcurrentDataSourceConstructions_ = std::max<size_t>(
        1,
        std::min<size_t>(4, std::max<unsigned>(1, std::thread::hardware_concurrency())));
    std::atomic_bool shuttingDown_{false};

    mutable std::mutex sourceCatalogCallbacksMutex_;
    std::map<uint64_t, Service::DataSourceCatalogCallback> sourceCatalogCallbacks_;
    uint64_t nextSourceCatalogCallbackId_ = 1;

    /** Compose the ready-source registry, scheduler, and optional config subscription. */
    Impl(
        Cache::Ptr cache,
        bool useDataSourceConfig,
        std::optional<std::chrono::milliseconds> defaultTtl,
        size_t workerCount,
        std::chrono::milliseconds errorTileTtl,
        std::chrono::milliseconds emptyTileTtl);

    /** Stop source construction before draining and joining homogeneous workers. */
    ~Impl();

    Impl(Impl const&) = delete;

    Impl& operator=(Impl const&) = delete;

    /** Register a ready datasource and publish it to the scheduler when primary. */
    detail::RegisteredDataSource::Ptr addDataSource(
        DataSource::Ptr const& dataSource,
        bool publishCatalogChange = true,
        std::optional<std::string> sourceId = {});

    /** Remove a ready datasource, invalidate its map, and prevent new work. */
    void removeDataSource(DataSource::Ptr const& dataSource, bool publishCatalogChange = true);

    /** Publish recomposed metadata to catalog/scheduler under catalogMutex_; defer callbacks. */
    [[nodiscard]] std::function<void()> synchronizeDataSourcesLocked();

    /** Admit and enqueue an atomic bundle of ordinary tile requests. */
    bool requestTiles(
        std::vector<LayerTilesRequest::Ptr> const& requests,
        std::optional<ClientAuthorization> const& clientAuthorization,
        detail::RegisteredDataSource::Ptr const& expectedSource = {});

    /** Start one coordinated feature-filter execution. */
    bool requestFilter(
        FeatureLayerFilterTilesRequest::Ptr const& request,
        std::optional<ClientAuthorization> const& clientAuthorization);

    /** Start one non-blocking attachment execution. */
    void requestAttachment(
        AttachmentRequest request,
        std::function<void(AttachmentResult)> callback,
        std::optional<ClientAuthorization> const& clientAuthorization);

    /** Resolve a locate request through candidates and ordinary tile loading. */
    std::vector<LocateResponse> locate(
        LocateRequest const& request, std::optional<ClientAuthorization> const& authorization);

    /** Resolve admission and optionally pin the authorized generation for later scheduling. */
    [[nodiscard]] LayerRequestContext resolveLayerRequest(
        std::string const& mapId,
        std::string const& layerId,
        std::optional<ClientAuthorization> const& clientAuthorization,
        std::optional<std::string> const& sourceId = {},
        detail::RegisteredDataSource::Ptr* selected = nullptr) const;

    /** Invalidate one ready primary map after applying optional datasource authorization. */
    [[nodiscard]] bool resetMapCache(
        std::string const& mapId,
        std::optional<ClientAuthorization> const& clientAuthorization);

    /** Assemble lightweight and optional expensive service statistics. */
    [[nodiscard]] nlohmann::json
    statistics(bool includeCachedFeatureTreeBytes, bool includeTileSizeDistribution) const;

    /** Assemble capacity-based service and cooperative datasource memory ownership. */
    [[nodiscard]] nlohmann::json memoryStatistics();

    /** Advance the revision and optionally capture an immediately serializable row delta. */
    [[nodiscard]] DataSourceCatalogChange markSourceCatalogChangedLocked(
        std::string reason,
        DataSourceCatalogEntry const* sourceUpdate = nullptr);

    /** Return whether no source in the current config generation is still initializing. */
    [[nodiscard]] bool sourceCatalogReloadDoneLocked() const;

    /** Wake blocking catalog readers and invoke change subscribers outside catalog locks. */
    void notifySourceCatalogChanged(DataSourceCatalogChange const& change);

    /** Join and erase source-construction threads that already reported completion. */
    void pruneCompletedConstructionThreadsLocked();

    /** Request cooperative cancellation of every retained constructor task. */
    void requestStopForConstructionThreadsLocked();

    /** Wait for a bounded datasource-construction slot unless cancelled. */
    bool acquireConstructionSlot(std::shared_ptr<std::atomic_bool> const& stopRequested);

    /** Return a datasource-construction slot and wake one waiter. */
    void releaseConstructionSlot();

    /** Reject late constructor results after reload or shutdown. */
    [[nodiscard]] bool isCurrentConstruction(uint64_t generation) const;

    /** Resolve a retained task to its current YAML row; caller holds catalogMutex_. */
    DataSourceCatalogEntry* constructionEntryLocked(uint64_t generation);

    /** Publish a constructor status-message delta for the current generation. */
    void updateCatalogStatusMessage(uint64_t generation, std::string message);

    /** Publish normalized optional constructor progress for the current generation. */
    void
    updateCatalogProgress(uint64_t generation, std::optional<float> progress);

    /** Mark one current-generation catalog row failed and preserve diagnostics. */
    void
    markCatalogConstructionFailed(uint64_t generation, std::string message, bool retrying = false);

    /** Atomically register a constructed source and attach it to its current catalog row. */
    bool markCatalogConstructionReady(
        uint64_t generation,
        DataSource::Ptr const& dataSource,
        std::optional<std::string> sourceId);

    /** Start one generation-guarded asynchronous datasource constructor. */
    void launchDataSourceConstruction(
        uint64_t generation,
        YAML::Node configNode,
        DataSourceDescriptor descriptor);

    /** Reconcile unchanged source lifetimes; explicit loads may retry failed entries. */
    void applyDataSourceConfig(std::vector<YAML::Node> const& dataSourceConfigNodes, bool retryFailedSources);

    /** Return detached authorized metadata for the legacy Service::info API. */
    [[nodiscard]] std::vector<DataSourceInfo>
    getDataSourceInfos(std::optional<ClientAuthorization> const& clientAuthorization) const;

    /** Return one authorized ordered catalog snapshot and its atomic revision. */
    [[nodiscard]] DataSourceCatalogSnapshot
    getSourceCatalog(std::optional<ClientAuthorization> const& clientAuthorization, bool waitUntilReloadDone)
        const;

    /** Return the current monotonic catalog revision without cloning rows. */
    [[nodiscard]] uint64_t getSourceCatalogRevision() const;

    /** Test whether an optional row delta is authorized for one subscriber. */
    [[nodiscard]] bool isSourceCatalogChangeVisible(
        DataSourceCatalogChange const& change,
        std::optional<ClientAuthorization> const& clientAuthorization) const;

    /** Register a catalog callback and return its removable subscription ID. */
    uint64_t addSourceCatalogCallback(Service::DataSourceCatalogCallback callback);

    /** Remove a previously registered catalog callback.
     * An already copied subscription snapshot may still invoke it.
     */
    void removeSourceCatalogCallback(uint64_t id);
};

}  // namespace mapget
