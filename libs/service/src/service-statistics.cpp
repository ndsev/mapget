#include "mapget/model/stream.h"
#include "service-impl.h"
#include "service-memory.h"

#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace mapget
{

namespace
{

using detail::dataSourceDescriptorMemoryUsage;
using detail::FilterMemoryTracker;

/** Build the explicitly requested one-shot cache-size distribution report. */
[[nodiscard]] nlohmann::json buildTileSizeDistribution(std::vector<int64_t> tileSizes)
{
    if (tileSizes.empty())
        return nlohmann::json::object();

    std::sort(tileSizes.begin(), tileSizes.end());

    const int64_t totalBytes = std::accumulate(tileSizes.begin(), tileSizes.end(), int64_t{0});
    const int64_t tileCount = static_cast<int64_t>(tileSizes.size());
    const int64_t meanBytes = totalBytes / tileCount;

    /** One stable display bucket in the human-facing cache report. */
    struct HistogramBin
    {
        int64_t upperBound;
        const char* label;
        int64_t count = 0;
    };
    std::vector<HistogramBin> bins = {
        {16 * 1024, "<=16 KiB"},
        {32 * 1024, "16-32 KiB"},
        {64 * 1024, "32-64 KiB"},
        {128 * 1024, "64-128 KiB"},
        {256 * 1024, "128-256 KiB"},
        {512 * 1024, "256-512 KiB"},
        {1024 * 1024, "512 KiB-1 MiB"},
        {2 * 1024 * 1024, "1-2 MiB"},
        {4 * 1024 * 1024, "2-4 MiB"},
    };
    int64_t overflowCount = 0;

    for (const auto bytes : tileSizes) {
        bool assigned = false;
        for (auto& bin : bins) {
            if (bytes <= bin.upperBound) {
                ++bin.count;
                assigned = true;
                break;
            }
        }
        if (!assigned) {
            ++overflowCount;
        }
    }

    auto histogram = nlohmann::json::array();
    for (const auto& bin : bins) {
        histogram.push_back(nlohmann::json::object({
            {"label", bin.label},
            {"count", bin.count},
        }));
    }
    histogram.push_back(nlohmann::json::object({
        {"label", ">4 MiB"},
        {"count", overflowCount},
    }));

    return nlohmann::json::object({
        {"tile-count", tileCount},
        {"total-tile-bytes", totalBytes},
        {"min-bytes", tileSizes.front()},
        {"max-bytes", tileSizes.back()},
        {"mean-bytes", meanBytes},
        {"histogram", std::move(histogram)},
    });
}

}  // namespace

nlohmann::json
Service::Impl::statistics(bool includeCachedFeatureTreeBytes, bool includeTileSizeDistribution)
    const
{
    auto const sources = dataSources_.snapshot();
    auto const schedulerStats = scheduler_.statistics();
    auto const sourceStats = scheduler_.sourceStatistics();
    auto datasources = nlohmann::json::array();
    for (auto const& source : sources) {
        auto row = nlohmann::json{
            {"name", source->info->mapId_},
            {"source-id", source->sourceId},
            {"add-on", source->info->isAddOn_},
        };
        auto permit = std::ranges::find_if(
            sourceStats,
            [&](auto const& candidate) { return candidate.sourceId == source->sourceId; });
        if (permit != sourceStats.end()) {
            row["parallel-limit"] = permit->limit;
            row["running"] = permit->running;
        }
        datasources.push_back(std::move(row));
    }

    size_t constructionFailures = 0;
    {
        std::shared_lock lock(catalogMutex_);
        constructionFailures = dataSourceConstructionFailed_;
    }
    auto configStats = DataSourceConfigService::get().getDataSourceConfigStats();
    auto result = nlohmann::json{
        {"datasources", datasources},
        {"active-requests", schedulerStats.activeTileRequests},
        {"workers",
         nlohmann::json{
             {"configured", schedulerStats.workerCount},
             {"running", schedulerStats.runningJobs}}},
        {"in-flight-tile-jobs", schedulerStats.inFlightTileJobs},
        {"datasource-config",
         nlohmann::json{
             {"configured", configStats.configured},
             {"enabled", configStats.enabled},
             {"disabled", configStats.disabled},
             {"construction-failed", constructionFailures}}}};

    if (auto allocator = detail::allocatorMemoryStatistics(); !allocator.is_null()) {
        result["allocator"] = std::move(allocator);
    }

    if (!includeCachedFeatureTreeBytes && !includeTileSizeDistribution) {
        return result;
    }

    auto featureLayerTotals = nlohmann::json::object();
    auto modelPoolTotals = nlohmann::json::object();
    auto geometryUsageTotals = nlohmann::json::object();
    auto validityUsageTotals = nlohmann::json::object();
    auto arrayArenaSingletonTotals = nlohmann::json::object();
    int64_t parsedTiles = 0;
    int64_t totalTileBytes = 0;
    int64_t parseErrors = 0;
    std::vector<int64_t> tileSizes;

    auto addTotals =
        [](nlohmann::json& totals, const nlohmann::json& stats, const auto& self) -> void
    {
        for (const auto& [key, value] : stats.items()) {
            if (value.is_number_integer()) {
                totals[key] = totals.template value<int64_t>(key, 0) +
                    value.template get<int64_t>();
            }
            else if (value.is_number_float()) {
                totals[key] = totals.template value<double>(key, .0) + value.template get<double>();
            }
            else if (value.is_object()) {
                if (!totals.contains(key) || !totals[key].is_object()) {
                    totals[key] = nlohmann::json::object();
                }
                self(totals[key], value, self);
            }
        }
    };

    LayerInfoResolveFun resolveLayerInfo;
    std::function<void(TileLayer::Ptr)> collectFeatureTreeStats;
    if (includeCachedFeatureTreeBytes) {
        auto layerInfoByMap = std::unordered_map<
            std::string,
            std::unordered_map<std::string, std::shared_ptr<LayerInfo>>>{};
        std::vector<DataSourceInfo> infos;
        infos.reserve(sources.size());
        for (auto const& source : sources) {
            infos.push_back(*source->info);
        }
        for (auto const& info : infos) {
            auto& layers = layerInfoByMap[info.mapId_];
            for (auto const& [layerId, layerInfo] : info.layers_) {
                layers[layerId] = layerInfo;
            }
        }

        resolveLayerInfo = [layerInfoByMap](std::string_view mapId, std::string_view layerId)
            -> std::shared_ptr<LayerInfo>
        {
            auto mapIt = layerInfoByMap.find(std::string(mapId));
            if (mapIt == layerInfoByMap.end())
                return std::make_shared<LayerInfo>();
            auto layerIt = mapIt->second.find(std::string(layerId));
            if (layerIt == mapIt->second.end()) {
                auto fallback = std::make_shared<LayerInfo>();
                fallback->layerId_ = std::string(layerId);
                return fallback;
            }
            return layerIt->second;
        };

        collectFeatureTreeStats = [&](auto&& parsedLayer)
        {
            auto tile = std::dynamic_pointer_cast<mapget::TileFeatureLayer>(parsedLayer);
            if (!tile) {
                ++parseErrors;
                return;
            }
            auto sizeStats = tile->serializationSizeStats();
            addTotals(featureLayerTotals, sizeStats["feature-layer"], addTotals);
            addTotals(modelPoolTotals, sizeStats["model-pool"], addTotals);
            addTotals(geometryUsageTotals, sizeStats["geometry-usage"], addTotals);
            addTotals(validityUsageTotals, sizeStats["validity-usage"], addTotals);
            addTotals(arrayArenaSingletonTotals, sizeStats["array-arena-singletons"], addTotals);
        };
    }

    auto cache = scheduler_.cache();
    cache->forEachTileLayerBlob(
        [&](const MapTileKey& key, const std::string& blob)
        {
            if (key.layer_ != LayerType::Features)
                return;

            const int64_t tileBytes = static_cast<int64_t>(blob.size());
            ++parsedTiles;
            totalTileBytes += tileBytes;

            if (includeTileSizeDistribution) {
                tileSizes.push_back(tileBytes);
            }

            if (!includeCachedFeatureTreeBytes) {
                return;
            }

            try {
                // A malformed cache value must not poison the framing state used
                // for the next independent value. Construct one reader per blob:
                // Reader::read deliberately retains an incomplete/error phase for
                // incremental streams.
                TileLayerStream::Reader
                    tileReader(resolveLayerInfo, collectFeatureTreeStats, cache);
                tileReader.read(blob);
                if (!tileReader.eos()) {
                    ++parseErrors;
                }
            }
            catch (const std::exception&) {
                ++parseErrors;
            }
        });

    if (includeCachedFeatureTreeBytes && parsedTiles > 0) {
        result["cached-feature-tree-bytes"] = nlohmann::json{
            {"tile-count", parsedTiles},
            {"total-tile-bytes", totalTileBytes},
            {"parse-errors", parseErrors},
            {"feature-layer", featureLayerTotals},
            {"model-pool", modelPoolTotals},
            {"geometry-usage", geometryUsageTotals},
            {"validity-usage", validityUsageTotals},
            {"array-arena-singletons", arrayArenaSingletonTotals}};
    }

    if (includeTileSizeDistribution && !tileSizes.empty()) {
        result["cached-feature-tile-size-distribution"] =
            buildTileSizeDistribution(std::move(tileSizes));
    }

    return result;
}

nlohmann::json Service::Impl::memoryStatistics()
{
    MemoryUsageBreakdown metadata;
    MemoryUsageBreakdown catalog;
    MemoryUsageBreakdown scheduler;
    MemoryUsageBreakdown telemetry;

    struct DataSourceMemoryCandidate
    {
        DataSource::Ptr dataSource;
        std::string sourceId;
        std::string mapId;
        std::string status;
    };
    std::vector<DataSourceMemoryCandidate> dataSourceCandidates;
    auto const registeredSources = dataSources_.snapshot();
    for (auto const& source : registeredSources) {
        metadata.add("canonical-snapshots", source->metadataMemory);
        metadata.add(
            "registered-source-objects",
            {
                sizeof(detail::RegisteredDataSource),
                sizeof(detail::RegisteredDataSource),
            });
    }

    {
        std::shared_lock lock(catalogMutex_);
        catalog.add("entries", vectorMemoryUsage(sourceCatalog_));
        std::unordered_set<DataSource const*> catalogDataSources;
        for (auto const& entry : sourceCatalog_) {
            catalog.add("descriptors", dataSourceDescriptorMemoryUsage(entry.descriptor));
            catalog.add("status-messages", stringMemoryUsage(entry.statusMessage));
            auto status = std::string("initializing");
            if (entry.status == DataSourceCatalogStatus::Ready) {
                status = "ready";
            }
            else if (entry.status == DataSourceCatalogStatus::Failed) {
                status = "failed";
            }
            dataSourceCandidates.push_back({
                entry.dataSource,
                entry.descriptor.sourceId,
                entry.info ? entry.info->mapId_ : entry.descriptor.displayName,
                std::move(status),
            });
            if (entry.dataSource) {
                catalogDataSources.insert(entry.dataSource.get());
            }
        }
        for (auto const& source : registeredSources) {
            if (!catalogDataSources.contains(source->dataSource.get())) {
                dataSourceCandidates.push_back({
                    source->dataSource,
                    source->sourceId,
                    source->info->mapId_,
                    "ready",
                });
            }
        }

        catalog.add(
            "ready-source-handles",
            {
                registeredSources.size() * sizeof(detail::RegisteredDataSource::Ptr),
                registeredSources.size() * sizeof(detail::RegisteredDataSource::Ptr),
            });
        for (auto const& source : registeredSources) {
            catalog.add("source-ids", stringMemoryUsage(source->sourceId));
        }
        catalog.add("config-status", stringMemoryUsage(sourceConfigStatus_));
        catalog.add("config-status-message", stringMemoryUsage(sourceConfigStatusMessage_));
        catalog.add("construction-threads", vectorMemoryUsage(dataSourceConstructionThreads_));
        catalog.add("config-datasource-handles", vectorMemoryUsage(dataSourcesFromConfig_));
        auto const addOnCount = std::ranges::count_if(
            registeredSources,
            [](auto const& source) { return source->info->isAddOn_; });
        catalog.add(
            "add-on-handles",
            {
                static_cast<uint64_t>(addOnCount) * sizeof(detail::RegisteredDataSource::Ptr),
                static_cast<uint64_t>(addOnCount) * sizeof(detail::RegisteredDataSource::Ptr),
            });
    }

    std::vector<std::shared_ptr<FilterMemoryTracker>> filterTrackers;
    scheduler_.collectMemoryUsage(scheduler, telemetry, filterTrackers);

    // Sampling may briefly acquire a filter execution mutex, so never do it
    // while holding the scheduler mutex used to publish completed work.
    auto activeFilters = nlohmann::json::array();
    uint64_t activeFilterBytes = 0;
    for (auto const& tracker : filterTrackers) {
        auto snapshot = tracker->toJson();
        activeFilterBytes += snapshot.value("current-bytes", uint64_t{0});
        activeFilters.push_back(std::move(snapshot));
    }
    telemetry.add("active-filter-owned", {0, activeFilterBytes});

    auto dataSources = nlohmann::json::array();
    uint64_t measuredDataSourceBytes = 0;
    for (auto const& candidate : dataSourceCandidates) {
        auto row = nlohmann::json{
            {"source-id", candidate.sourceId},
            {"map-id", candidate.mapId},
            {"status", candidate.status},
            {"measurement", "unavailable"},
        };
        if (candidate.dataSource) {
            try {
                if (auto bytes = candidate.dataSource->estimatedRetainedMemoryBytes()) {
                    row["retained-bytes"] = *bytes;
                    row["measurement"] = "datasource-estimate";
                    measuredDataSourceBytes += *bytes;
                }
            }
            catch (std::exception const& error) {
                row["measurement"] = "error";
                row["error"] = error.what();
            }
            catch (...) {
                row["measurement"] = "error";
                row["error"] = "Datasource memory estimator threw a non-standard exception.";
            }
        }
        dataSources.push_back(std::move(row));
    }

    MemoryUsageBreakdown mapgetOwned;
    mapgetOwned.merge("metadata", metadata);
    mapgetOwned.merge("catalog", catalog);
    mapgetOwned.merge("scheduler", scheduler);
    mapgetOwned.merge("telemetry", telemetry);
    auto result = nlohmann::json{
        {"process", detail::processMemoryStatistics()},
        {"mapget", mapgetOwned.toJson()},
        {"active-filters", std::move(activeFilters)},
        {"datasources", std::move(dataSources)},
        {"datasource-measured-bytes", measuredDataSourceBytes},
        {"quality",
         {
             {"mapget", "capacity-lower-bound"},
             {"datasources", "cooperative-exclusive-estimate"},
             {"allocator-bookkeeping-included", false},
         }},
    };
    if (auto allocator = detail::allocatorMemoryStatistics(); !allocator.is_null()) {
        result["allocator"] = std::move(allocator);
    }
    auto const knownBytes = mapgetOwned.total().allocatedBytes + measuredDataSourceBytes;
    result["known-current-bytes"] = knownBytes;
    return result;
}

nlohmann::json Service::getStatistics() const
{
    return impl_->statistics(true, false);
}

nlohmann::json
Service::getStatistics(bool includeCachedFeatureTreeBytes, bool includeTileSizeDistribution) const
{
    return impl_->statistics(includeCachedFeatureTreeBytes, includeTileSizeDistribution);
}

nlohmann::json Service::getMemoryStatistics() const
{
    return impl_->memoryStatistics();
}

}  // namespace mapget
