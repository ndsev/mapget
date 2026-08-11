#include "service-datasources.h"

#include "fmt/format.h"
#include "mapget/log.h"
#include "mapget/model/stream.h"
#include "service-impl.h"

#include <algorithm>
#include <cmath>
#include <regex>

namespace mapget
{
namespace
{

/** Read the optional cheap config enable flag without rejecting legacy rows. */
bool isDataSourceDescriptorEnabled(YAML::Node const& descriptor)
{
    if (!descriptor["enabled"].IsDefined()) {
        return true;
    }
    try {
        return descriptor["enabled"].as<bool>(true);
    }
    catch (...) {
        return true;
    }
}

/** Apply descriptor-only auth rules before a datasource instance exists. */
bool isDescriptorAuthorized(
    DataSourceDescriptor const& descriptor,
    std::optional<AuthHeaders> const& clientHeaders)
{
    if (!clientHeaders || descriptor.authHeaderAlternatives.empty()) {
        return true;
    }
    for (auto const& [header, value] : *clientHeaders) {
        auto normalizedHeader = header;
        std::ranges::transform(
            normalizedHeader,
            normalizedHeader.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        auto const pattern = descriptor.authHeaderAlternatives.find(normalizedHeader);
        if (pattern != descriptor.authHeaderAlternatives.end() &&
            std::regex_match(value, pattern->second)) {
            return true;
        }
    }
    return false;
}

/** Drop non-finite progress and clamp reported percentages to the wire range. */
std::optional<float> normalizeProgressPercentage(std::optional<float> progress)
{
    if (!progress || !std::isfinite(*progress)) {
        return std::nullopt;
    }
    return std::clamp(*progress, 0.0f, 100.0f);
}

/** Copy the lightweight fields carried by an interactive catalog delta. */
DataSourceCatalogSourceUpdate makeSourceCatalogSourceUpdate(DataSourceCatalogEntry const& entry)
{
    return DataSourceCatalogSourceUpdate{
        .descriptor = entry.descriptor,
        .status = entry.status,
        .statusMessage = entry.statusMessage,
        .progress = entry.progress,
        .dataSource = entry.dataSource,
    };
}

}  // namespace
}  // namespace mapget

namespace mapget::detail
{

std::shared_ptr<LayerInfo> cloneLayerInfo(LayerInfo const& info)
{
    auto result = std::make_shared<LayerInfo>();
    result->layerId_ = info.layerId_;
    result->type_ = info.type_;
    result->featureTypes_ = info.featureTypes_;
    result->zoomLevels_ = info.zoomLevels_;
    result->coverage_ = info.coverage_;
    result->canRead_ = info.canRead_;
    result->canWrite_ = info.canWrite_;
    result->version_ = info.version_;
    result->featureModelSchema_ = info.featureModelSchema_ ?
        info.featureModelSchema_->detachedCopy() :
        nullptr;
    return result;
}

DataSourceInfo cloneDataSourceInfo(DataSourceInfo const& info)
{
    info.validateIdentifiers();

    auto result = info;
    if (result.protocolVersion_ == Version{}) {
        result.protocolVersion_ = TileLayerStream::CurrentProtocolVersion;
    }
    result.layers_.clear();
    result.layers_.reserve(info.layers_.size());
    for (auto const& [layerId, layerInfo] : info.layers_) {
        if (!layerInfo) {
            raise(fmt::format(
                "Datasource '{}' has null LayerInfo for layer '{}'.",
                info.stringPoolId_,
                layerId));
        }
        result.layers_.try_emplace(layerId, cloneLayerInfo(*layerInfo));
    }
    return result;
}

RegisteredDataSource::Ptr
DataSourceRegistry::add(DataSource::Ptr dataSource, std::optional<std::string> sourceId)
{
    if (!dataSource) {
        raise("Tried to add a null data source.");
    }

    auto info = std::make_shared<DataSourceInfo const>(cloneDataSourceInfo(dataSource->info()));
    if (info->stringPoolId_.empty()) {
        raise("Tried to register a datasource with an empty string-pool ID.");
    }
    if (info->maxParallelJobs_ <= 0) {
        raise("Datasource maxParallelJobs must be greater than zero.");
    }

    std::unique_lock lock(mutex_);
    for (auto const& existing : sources_) {
        if (existing->dataSource == dataSource) {
            raise("Tried to register the same datasource instance twice.");
        }
        if (existing->info->stringPoolId_ == info->stringPoolId_) {
            raise(fmt::format(
                "Data source with node ID '{}' already registered!",
                info->stringPoolId_));
        }
        if (!info->isAddOn_ && !existing->info->isAddOn_ && existing->info->mapId_ == info->mapId_)
        {
            raise(fmt::format(
                "Primary data source for map '{}' is already registered.",
                info->mapId_));
        }
    }

    auto effectiveSourceId = sourceId ?
        std::move(*sourceId) :
        fmt::format("runtime-source-{}", nextRuntimeSourceId_++);
    if (effectiveSourceId.empty()) {
        raise("Data source catalog sourceId must not be empty.");
    }
    if (std::ranges::any_of(
            sources_,
            [&](auto const& existing) { return existing->sourceId == effectiveSourceId; }))
    {
        raise(fmt::format(
            "Data source catalog sourceId '{}' is already registered.",
            effectiveSourceId));
    }

    auto registered = std::make_shared<RegisteredDataSource>(RegisteredDataSource{
        .dataSource = std::move(dataSource),
        .info = std::move(info),
        .sourceId = std::move(effectiveSourceId),
    });
    registered->metadataMemory = registered->info->memoryUsage().total();
    sources_.push_back(registered);
    return registered;
}

RegisteredDataSource::Ptr DataSourceRegistry::remove(DataSource::Ptr const& dataSource)
{
    std::unique_lock lock(mutex_);
    auto const found = std::ranges::find_if(
        sources_,
        [&](auto const& source) { return source->dataSource == dataSource; });
    if (found == sources_.end()) {
        return {};
    }
    auto removed = *found;
    sources_.erase(found);
    return removed;
}

std::vector<RegisteredDataSource::Ptr> DataSourceRegistry::snapshot() const
{
    std::shared_lock lock(mutex_);
    return sources_;
}

std::vector<RegisteredDataSource::Ptr> DataSourceRegistry::matchingPrimarySources(
    std::string_view mapId,
    std::string_view layerId,
    std::optional<std::string> const& sourceId) const
{
    std::vector<RegisteredDataSource::Ptr> result;
    std::shared_lock lock(mutex_);
    for (auto const& source : sources_) {
        if (source->info->isAddOn_ || source->info->mapId_ != mapId ||
            !source->info->layers_.contains(std::string(layerId)))
        {
            continue;
        }
        if (sourceId && source->sourceId != *sourceId) {
            continue;
        }
        result.push_back(source);
    }
    return result;
}

std::vector<RegisteredDataSource::Ptr> DataSourceRegistry::addOnSources() const
{
    std::vector<RegisteredDataSource::Ptr> result;
    std::shared_lock lock(mutex_);
    for (auto const& source : sources_) {
        if (source->info->isAddOn_) {
            result.push_back(source);
        }
    }
    return result;
}

std::vector<DataSourceInfo>
DataSourceRegistry::infos(std::optional<AuthHeaders> const& clientHeaders) const
{
    auto sources = snapshot();
    std::vector<DataSourceInfo> result;
    result.reserve(sources.size());
    for (auto const& source : sources) {
        if (!clientHeaders || source->dataSource->isDataSourceAuthorized(*clientHeaders)) {
            result.push_back(cloneDataSourceInfo(*source->info));
        }
    }
    return result;
}

size_t DataSourceRegistry::size() const
{
    std::shared_lock lock(mutex_);
    return sources_.size();
}

bool DataSourceRegistry::empty() const
{
    std::shared_lock lock(mutex_);
    return sources_.empty();
}

}  // namespace mapget::detail

namespace mapget
{

Service::Impl::Impl(
    Cache::Ptr cache,
    bool useDataSourceConfig,
    std::optional<std::chrono::milliseconds> defaultTtl,
    size_t workerCount)
    : scheduler_(dataSources_, std::move(cache), defaultTtl, workerCount)
{
    if (!useDataSourceConfig) {
        return;
    }
    configSubscription_ = DataSourceConfigService::get().subscribe(
        [this](auto&& configNodes) { applyDataSourceConfig(configNodes); },
        [this](std::string const& error)
        {
            DataSourceCatalogChange change;
            {
                std::unique_lock lock(catalogMutex_);
                sourceConfigStatus_ = "error";
                sourceConfigStatusMessage_ = error;
                change = markSourceCatalogChangedLocked("config-error");
            }
            notifySourceCatalogChanged(change);
        });
}

Service::Impl::~Impl()
{
    // Stop constructors first so no source can be published while the worker
    // pool drains request-owned closures.
    shuttingDown_ = true;
    sourceCatalogReadyCv_.notify_all();
    configSubscription_.reset();

    std::vector<ConstructionThread> constructionThreads;
    {
        std::unique_lock lock(catalogMutex_);
        requestStopForConstructionThreadsLocked();
        constructionThreads.swap(dataSourceConstructionThreads_);
    }
    constructionSlotCv_.notify_all();
    for (auto& construction : constructionThreads) {
        if (construction.thread.joinable()) {
            construction.thread.join();
        }
    }

    scheduler_.stop();
    dataSourcesFromConfig_.clear();
}

DataSourceCatalogChange Service::Impl::markSourceCatalogChangedLocked(
    std::string reason,
    DataSourceCatalogEntry const* sourceUpdate)
{
    auto change = DataSourceCatalogChange{
        .revision = ++sourceCatalogRevision_,
        .reason = std::move(reason),
    };
    if (sourceUpdate) {
        change.sourceUpdate = makeSourceCatalogSourceUpdate(*sourceUpdate);
    }
    return change;
}

bool Service::Impl::sourceCatalogReloadDoneLocked() const
{
    return shuttingDown_.load(std::memory_order_relaxed) ||
        std::ranges::none_of(
               sourceCatalog_,
               [](auto const& entry)
               { return entry.status == DataSourceCatalogStatus::Initializing; });
}

void Service::Impl::notifySourceCatalogChanged(DataSourceCatalogChange const& change)
{
    sourceCatalogReadyCv_.notify_all();
    std::vector<Service::DataSourceCatalogCallback> callbacks;
    {
        std::lock_guard lock(sourceCatalogCallbacksMutex_);
        callbacks.reserve(sourceCatalogCallbacks_.size());
        for (auto const& [_, callback] : sourceCatalogCallbacks_) {
            callbacks.push_back(callback);
        }
    }
    for (auto const& callback : callbacks) {
        try {
            callback(change);
        }
        catch (std::exception const& error) {
            log().warn("Datasource catalog callback failed: {}", error.what());
        }
        catch (...) {
            log().warn("Datasource catalog callback failed with a non-standard exception.");
        }
    }
}

void Service::Impl::pruneCompletedConstructionThreadsLocked()
{
    std::erase_if(
        dataSourceConstructionThreads_,
        [](ConstructionThread& construction)
        {
            if (!construction.done || !construction.done->load(std::memory_order_acquire)) {
                return false;
            }
            if (construction.thread.joinable()) {
                construction.thread.join();
            }
            return true;
        });
}

void Service::Impl::requestStopForConstructionThreadsLocked()
{
    for (auto& construction : dataSourceConstructionThreads_) {
        if (construction.stopRequested) {
            construction.stopRequested->store(true, std::memory_order_release);
        }
    }
    constructionSlotCv_.notify_all();
}

bool Service::Impl::acquireConstructionSlot(std::shared_ptr<std::atomic_bool> const& stopRequested)
{
    std::unique_lock lock(constructionSlotMutex_);
    constructionSlotCv_.wait(
        lock,
        [&]
        {
            return shuttingDown_.load(std::memory_order_relaxed) ||
                (stopRequested && stopRequested->load(std::memory_order_acquire)) ||
                activeDataSourceConstructions_ < maxConcurrentDataSourceConstructions_;
        });
    if (shuttingDown_.load(std::memory_order_relaxed) ||
        (stopRequested && stopRequested->load(std::memory_order_acquire)))
    {
        return false;
    }
    ++activeDataSourceConstructions_;
    return true;
}

void Service::Impl::releaseConstructionSlot()
{
    {
        std::lock_guard lock(constructionSlotMutex_);
        if (activeDataSourceConstructions_ > 0) {
            --activeDataSourceConstructions_;
        }
    }
    constructionSlotCv_.notify_all();
}

bool Service::Impl::isCurrentCatalogGeneration(uint64_t generation) const
{
    std::shared_lock lock(catalogMutex_);
    return generation == sourceCatalogGeneration_ && !shuttingDown_.load(std::memory_order_relaxed);
}

void Service::Impl::updateCatalogStatusMessage(
    uint64_t generation,
    uint32_t configIndex,
    std::string message)
{
    std::optional<DataSourceCatalogChange> change;
    {
        std::unique_lock lock(catalogMutex_);
        if (generation != sourceCatalogGeneration_) {
            return;
        }
        auto entry = std::ranges::find_if(
            sourceCatalog_,
            [&](auto const& candidate) { return candidate.descriptor.configIndex == configIndex; });
        if (entry == sourceCatalog_.end() || entry->statusMessage == message) {
            return;
        }
        entry->statusMessage = std::move(message);
        change = markSourceCatalogChangedLocked("status-message", &*entry);
    }
    notifySourceCatalogChanged(*change);
}

void Service::Impl::updateCatalogProgress(
    uint64_t generation,
    uint32_t configIndex,
    std::optional<float> progress)
{
    progress = normalizeProgressPercentage(progress);
    std::optional<DataSourceCatalogChange> change;
    {
        std::unique_lock lock(catalogMutex_);
        if (generation != sourceCatalogGeneration_) {
            return;
        }
        auto entry = std::ranges::find_if(
            sourceCatalog_,
            [&](auto const& candidate) { return candidate.descriptor.configIndex == configIndex; });
        if (entry == sourceCatalog_.end() || entry->progress == progress) {
            return;
        }
        entry->progress = progress;
        change = markSourceCatalogChangedLocked("progress", &*entry);
    }
    notifySourceCatalogChanged(*change);
}

void Service::Impl::markCatalogConstructionFailed(
    uint64_t generation,
    uint32_t configIndex,
    std::string message)
{
    std::optional<DataSourceCatalogChange> change;
    {
        std::unique_lock lock(catalogMutex_);
        if (generation != sourceCatalogGeneration_) {
            return;
        }
        auto entry = std::ranges::find_if(
            sourceCatalog_,
            [&](auto const& candidate) { return candidate.descriptor.configIndex == configIndex; });
        if (entry == sourceCatalog_.end()) {
            return;
        }
        entry->status = DataSourceCatalogStatus::Failed;
        entry->statusMessage = std::move(message);
        entry->progress.reset();
        ++dataSourceConstructionFailed_;
        change = markSourceCatalogChangedLocked("status", &*entry);
    }
    notifySourceCatalogChanged(*change);
}

bool Service::Impl::markCatalogConstructionReady(
    uint64_t generation,
    uint32_t configIndex,
    DataSource::Ptr const& dataSource,
    std::optional<std::string> sourceId)
{
    std::optional<DataSourceCatalogChange> change;
    {
        std::unique_lock lock(catalogMutex_);
        if (generation != sourceCatalogGeneration_) {
            return false;
        }
        auto entry = std::ranges::find_if(
            sourceCatalog_,
            [&](auto const& candidate) { return candidate.descriptor.configIndex == configIndex; });
        if (entry == sourceCatalog_.end()) {
            return false;
        }
        // Registration and the Ready transition share the catalog lock. A
        // superseding reload can therefore never leave a late source visible
        // in the ready registry without a matching current-generation row.
        auto source = dataSources_.add(dataSource, std::move(sourceId));
        scheduler_.registerDataSource(source);
        entry->status = DataSourceCatalogStatus::Ready;
        entry->statusMessage.clear();
        entry->progress.reset();
        entry->dataSource = source->dataSource;
        entry->info = source->info;
        dataSourcesFromConfig_.push_back(source->dataSource);
        change = markSourceCatalogChangedLocked("status", &*entry);
    }
    notifySourceCatalogChanged(*change);
    return true;
}

void Service::Impl::launchDataSourceConstruction(
    uint64_t generation,
    YAML::Node configNode,
    DataSourceDescriptor descriptor)
{
    auto done = std::make_shared<std::atomic_bool>(false);
    auto stopRequested = std::make_shared<std::atomic_bool>(false);
    auto const configIndex = descriptor.configIndex;
    auto sourceId = descriptor.sourceId;
    dataSourceConstructionThreads_.push_back(ConstructionThread{
        .thread = std::thread(
            [this,
             generation,
             configNode = std::move(configNode),
             configIndex,
             sourceId = std::move(sourceId),
             done,
             stopRequested]() mutable
            {
                auto slotAcquired = false;
                auto finish = [&]
                {
                    if (slotAcquired) {
                        releaseConstructionSlot();
                    }
                    done->store(true, std::memory_order_release);
                };
                try {
                    slotAcquired = acquireConstructionSlot(stopRequested);
                    if (!slotAcquired || !isCurrentCatalogGeneration(generation)) {
                        finish();
                        return;
                    }

                    std::string lastStatusMessage;
                    DataSourceInitContext context{
                        .setStatusMessage =
                            [this, generation, configIndex, &lastStatusMessage](std::string message)
                        {
                            lastStatusMessage = message;
                            updateCatalogStatusMessage(generation, configIndex, std::move(message));
                        },
                        .setProgress =
                            [this, generation, configIndex](std::optional<float> progress)
                        { updateCatalogProgress(generation, configIndex, progress); },
                        .isCancelled =
                            [this, generation, stopRequested]
                        {
                            return stopRequested->load(std::memory_order_acquire) ||
                                !isCurrentCatalogGeneration(generation);
                        },
                    };
                    auto dataSource =
                        DataSourceConfigService::get().makeDataSource(configNode, context);
                    if (!dataSource) {
                        if (isCurrentCatalogGeneration(generation)) {
                            if (lastStatusMessage.empty()) {
                                lastStatusMessage = fmt::format(
                                    "Failed to make datasource at index {}.",
                                    configIndex);
                            }
                            markCatalogConstructionFailed(
                                generation,
                                configIndex,
                                std::move(lastStatusMessage));
                        }
                        finish();
                        return;
                    }
                    if (!isCurrentCatalogGeneration(generation)) {
                        finish();
                        return;
                    }

                    markCatalogConstructionReady(
                        generation,
                        configIndex,
                        dataSource,
                        std::move(sourceId));
                }
                catch (std::exception const& error) {
                    if (isCurrentCatalogGeneration(generation)) {
                        markCatalogConstructionFailed(
                            generation,
                            configIndex,
                            fmt::format(
                                "Exception while making datasource at index {}: {}",
                                configIndex,
                                error.what()));
                    }
                }
                catch (...) {
                    if (isCurrentCatalogGeneration(generation)) {
                        markCatalogConstructionFailed(
                            generation,
                            configIndex,
                            fmt::format(
                                "Unknown exception while making datasource at index {}.",
                                configIndex));
                    }
                }
                finish();
            }),
        .done = std::move(done),
        .stopRequested = std::move(stopRequested),
    });
}

void Service::Impl::applyDataSourceConfig(std::vector<YAML::Node> const& configNodes)
{
    std::vector<DataSource::Ptr> previousDataSources;
    std::vector<std::pair<YAML::Node, DataSourceDescriptor>> constructionInputs;
    uint64_t generation = 0;
    DataSourceCatalogChange change;
    {
        std::unique_lock lock(catalogMutex_);
        pruneCompletedConstructionThreadsLocked();
        requestStopForConstructionThreadsLocked();
        previousDataSources.swap(dataSourcesFromConfig_);
        dataSourceConstructionFailed_ = 0;
        sourceCatalog_.clear();
        sourceConfigStatus_ = "ok";
        sourceConfigStatusMessage_.clear();
        generation = ++sourceCatalogGeneration_;

        auto index = uint32_t{0};
        for (auto const& configNode : configNodes) {
            if (!isDataSourceDescriptorEnabled(configNode)) {
                ++index;
                continue;
            }
            auto descriptor = DataSourceConfigService::get().describeDataSource(configNode, index);
            sourceCatalog_.push_back(DataSourceCatalogEntry{
                .descriptor = descriptor,
                .status = DataSourceCatalogStatus::Initializing,
                .statusMessage = "Initializing datasource.",
            });
            constructionInputs.emplace_back(configNode, std::move(descriptor));
            ++index;
        }
        change = markSourceCatalogChangedLocked("reload");
    }

    log().info("Config changed. Removing previous datasources.");
    for (auto const& dataSource : previousDataSources) {
        removeDataSource(dataSource, false);
    }
    notifySourceCatalogChanged(change);

    std::unique_lock lock(catalogMutex_);
    for (auto& [configNode, descriptor] : constructionInputs) {
        launchDataSourceConstruction(generation, std::move(configNode), std::move(descriptor));
    }
}

detail::RegisteredDataSource::Ptr Service::Impl::addDataSource(
    DataSource::Ptr const& dataSource,
    bool publishCatalogChange,
    std::optional<std::string> sourceId)
{
    detail::RegisteredDataSource::Ptr registered;
    std::optional<DataSourceCatalogChange> change;
    {
        // Catalog snapshots acquire this lock before reading the ready-source
        // registry, keeping the returned revision and source list atomic.
        std::unique_lock lock(catalogMutex_);
        registered = dataSources_.add(dataSource, std::move(sourceId));
        scheduler_.registerDataSource(registered);
        if (publishCatalogChange) {
            change = markSourceCatalogChangedLocked("added");
        }
    }
    if (change) {
        notifySourceCatalogChanged(*change);
    }
    return registered;
}

void Service::Impl::removeDataSource(DataSource::Ptr const& dataSource, bool publishCatalogChange)
{
    detail::RegisteredDataSource::Ptr registered;
    std::optional<DataSourceCatalogChange> change;
    {
        // Preserve the catalog -> registry -> scheduler lock order used by
        // registration and snapshot assembly.
        std::unique_lock lock(catalogMutex_);
        registered = dataSources_.remove(dataSource);
        if (!registered) {
            return;
        }
        scheduler_.unregisterDataSource(registered);
        if (publishCatalogChange) {
            change = markSourceCatalogChangedLocked("removed");
        }
    }
    scheduler_.invalidateMap(registered->info->mapId_);
    if (change) {
        notifySourceCatalogChanged(*change);
    }
}

std::vector<DataSourceInfo>
Service::Impl::getDataSourceInfos(std::optional<AuthHeaders> const& clientHeaders) const
{
    return dataSources_.infos(clientHeaders);
}

DataSourceCatalogSnapshot Service::Impl::getSourceCatalog(
    std::optional<AuthHeaders> const& clientHeaders,
    bool waitUntilReloadDone) const
{
    DataSourceCatalogSnapshot snapshot;
    std::shared_lock lock(catalogMutex_);
    if (waitUntilReloadDone) {
        sourceCatalogReadyCv_.wait(lock, [this] { return sourceCatalogReloadDoneLocked(); });
    }
    snapshot.revision = sourceCatalogRevision_;
    snapshot.configStatus = sourceConfigStatus_;
    snapshot.configStatusMessage = sourceConfigStatusMessage_;
    if (!sourceCatalog_.empty()) {
        snapshot.sources.reserve(sourceCatalog_.size());
        for (auto const& entry : sourceCatalog_) {
            auto const authorized = entry.dataSource ?
                (!clientHeaders || entry.dataSource->isDataSourceAuthorized(*clientHeaders)) :
                isDescriptorAuthorized(entry.descriptor, clientHeaders);
            if (authorized) {
                snapshot.sources.push_back(entry);
            }
        }
        return snapshot;
    }
    // Runtime-only catalogs use the ready registry as their source list. Keep
    // the catalog lock while taking that snapshot so its revision describes
    // exactly the returned rows.
    auto sources = dataSources_.snapshot();
    snapshot.sources.reserve(sources.size());
    auto configIndex = uint32_t{0};
    for (auto const& source : sources) {
        auto const currentIndex = configIndex++;
        if (clientHeaders && !source->dataSource->isDataSourceAuthorized(*clientHeaders)) {
            continue;
        }
        snapshot.sources.push_back(DataSourceCatalogEntry{
            .descriptor =
                DataSourceDescriptor{
                    .configIndex = currentIndex,
                    .sourceId = source->sourceId,
                    .type = "",
                    .displayName =
                        fmt::format("datasource-{}-{}", currentIndex, source->info->mapId_),
                    .addOn = source->info->isAddOn_,
                },
            .status = DataSourceCatalogStatus::Ready,
            .dataSource = source->dataSource,
            .info = source->info,
        });
    }
    return snapshot;
}

uint64_t Service::Impl::getSourceCatalogRevision() const
{
    std::shared_lock lock(catalogMutex_);
    return sourceCatalogRevision_;
}

bool Service::Impl::isSourceCatalogChangeVisible(
    DataSourceCatalogChange const& change,
    std::optional<AuthHeaders> const& clientHeaders) const
{
    if (!change.sourceUpdate) {
        return true;
    }
    auto const& source = *change.sourceUpdate;
    if (source.dataSource) {
        return !clientHeaders || source.dataSource->isDataSourceAuthorized(*clientHeaders);
    }
    return isDescriptorAuthorized(source.descriptor, clientHeaders);
}

uint64_t Service::Impl::addSourceCatalogCallback(Service::DataSourceCatalogCallback callback)
{
    if (!callback) {
        return 0;
    }
    std::lock_guard lock(sourceCatalogCallbacksMutex_);
    auto const id = nextSourceCatalogCallbackId_++;
    sourceCatalogCallbacks_[id] = std::move(callback);
    return id;
}

void Service::Impl::removeSourceCatalogCallback(uint64_t id)
{
    if (!id) {
        return;
    }
    std::lock_guard lock(sourceCatalogCallbacksMutex_);
    sourceCatalogCallbacks_.erase(id);
}

LayerRequestContext Service::Impl::resolveLayerRequest(
    std::string const& mapId,
    std::string const& layerId,
    std::optional<AuthHeaders> const& clientHeaders,
    std::optional<std::string> const& sourceId) const
{
    LayerRequestContext result;
    auto const matches = dataSources_.matchingPrimarySources(mapId, layerId, sourceId);
    auto unauthorized = false;
    auto foundAuthorized = false;
    for (auto const& source : matches) {
        if (clientHeaders && !source->dataSource->isDataSourceAuthorized(*clientHeaders)) {
            unauthorized = true;
            continue;
        }
        auto const type = source->info->layers_.at(layerId)->type_;
        if (!foundAuthorized) {
            result.status_ = RequestStatus::Success;
            result.layerType_ = type;
            foundAuthorized = true;
        }
        else if (result.layerType_ != type) {
            log().warn(
                "Conflicting layer types for {}::{} across data sources ({} vs {}).",
                mapId,
                layerId,
                static_cast<int>(result.layerType_),
                static_cast<int>(type));
        }
    }
    if (foundAuthorized) {
        return result;
    }
    if (!matches.empty() && unauthorized) {
        result.status_ = RequestStatus::Unauthorized;
        result.noDataSourceReason_ = NoDataSourceReason::None;
        return result;
    }

    result.status_ = RequestStatus::NoDataSource;
    result.noDataSourceReason_ = NoDataSourceReason::MissingMapOrLayer;
    if (dataSources_.empty()) {
        auto const configPath = DataSourceConfigService::get().getConfigFilePath();
        auto const configStats = DataSourceConfigService::get().getDataSourceConfigStats();
        if (!configPath) {
            result.noDataSourceReason_ = NoDataSourceReason::NoConfig;
        }
        else if (configStats.configured == 0) {
            result.noDataSourceReason_ = NoDataSourceReason::EmptySources;
        }
        else if (configStats.enabled == 0) {
            result.noDataSourceReason_ = NoDataSourceReason::AllSourcesDisabled;
        }
        else {
            std::shared_lock lock(catalogMutex_);
            if (dataSourceConstructionFailed_ > 0) {
                result.noDataSourceReason_ = NoDataSourceReason::DatasourceInitializationFailed;
            }
        }
    }
    return result;
}

}  // namespace mapget
