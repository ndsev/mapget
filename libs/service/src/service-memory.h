// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "simfil/model/memory.h"

namespace mapget
{

struct DataSourceDescriptor;
struct DataSourceInfo;

namespace detail
{

/** Atomically retain current and peak bytes so short-lived spikes survive polling. */
class RetainedMemoryGauge
{
public:
    /** Add newly retained bytes and update the high-water mark. */
    void add(uint64_t bytes);

    /** Release retained bytes, saturating at zero if lifecycle accounting is incomplete. */
    void subtract(uint64_t bytes);

    /** Replace the current sample and update the high-water mark. */
    void set(uint64_t bytes);

    /** Render current and peak byte counts for diagnostics. */
    [[nodiscard]] nlohmann::json toJson() const;

    /** Return the current retained-byte count. */
    [[nodiscard]] uint64_t current() const;

private:
    void updatePeak(uint64_t candidate);

    std::atomic_uint64_t current_{0};
    std::atomic_uint64_t peak_{0};
};

/** Request-scoped memory ownership retained by the asynchronous filter pipeline. */
struct FilterMemoryTracker
{
    std::string mapId;
    std::string layerId;
    std::string filterId;
    uint64_t generation = 0;
    size_t requestedTiles = 0;
    RetainedMemoryGauge sourceTileModels;
    RetainedMemoryGauge outputSubsetModels;
    RetainedMemoryGauge relationTargetModels;
    RetainedMemoryGauge evaluationTemporaries;
    RetainedMemoryGauge orchestration;
    std::function<uint64_t()> sampleOrchestration;

    /** Sample expensive container state and render the complete ownership row. */
    [[nodiscard]] nlohmann::json toJson();
};

/** Collect OS-owned process totals used to reconcile subsystem lower bounds. */
[[nodiscard]] nlohmann::json processMemoryStatistics();

/** Collect allocator counters when the platform exposes them. */
[[nodiscard]] nlohmann::json allocatorMemoryStatistics();

/** Measure cheap catalog descriptor fields; std::regex implementation state is opaque. */
[[nodiscard]] simfil::MemoryUsage dataSourceDescriptorMemoryUsage(
    DataSourceDescriptor const& descriptor);

} // namespace detail
} // namespace mapget
