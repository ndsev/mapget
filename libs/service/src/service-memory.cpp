// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "service-memory.h"

#include <charconv>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <psapi.h>
#include <windows.h>
#endif

#include "mapget/model/info.h"
#include "mapget/model/memory.h"
#include "mapget/service/config.h"

namespace mapget::detail
{
namespace
{

/** Parse an unsigned decimal after trimming surrounding ASCII whitespace. */
[[nodiscard]] std::optional<uint64_t> parseUnsigned(std::string_view text)
{
    auto const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    auto const last = text.find_last_not_of(" \t\r\n");
    text = text.substr(first, last - first + 1);

    uint64_t value = 0;
    auto const [end, error] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value);
    if (error != std::errc{} || end == text.data()) {
        return std::nullopt;
    }
    return value;
}

/** Parse one numeric kB field from Linux's proc status representation. */
[[nodiscard]] std::optional<uint64_t> procStatusBytes(
    std::unordered_map<std::string, std::string> const& fields,
    std::string const& key)
{
    auto found = fields.find(key);
    if (found == fields.end()) {
        return std::nullopt;
    }
    auto kilobytes = parseUnsigned(found->second);
    if (!kilobytes || *kilobytes > std::numeric_limits<uint64_t>::max() / 1024) {
        return std::nullopt;
    }
    return *kilobytes * 1024;
}

/** Read a decimal byte counter while accepting the cgroup `max` sentinel. */
[[nodiscard]] nlohmann::json readByteCounterFile(std::string const& path)
{
    std::ifstream input(path);
    std::string value;
    if (!(input >> value) || value == "max") {
        return nullptr;
    }
    auto parsed = parseUnsigned(value);
    return parsed ? nlohmann::json(*parsed) : nlohmann::json(nullptr);
}

} // namespace

void RetainedMemoryGauge::add(uint64_t bytes)
{
    auto const current = current_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    updatePeak(current);
}

void RetainedMemoryGauge::subtract(uint64_t bytes)
{
    auto current = current_.load(std::memory_order_relaxed);
    while (!current_.compare_exchange_weak(
        current,
        current > bytes ? current - bytes : 0,
        std::memory_order_relaxed)) {
    }
}

void RetainedMemoryGauge::set(uint64_t bytes)
{
    current_.store(bytes, std::memory_order_relaxed);
    updatePeak(bytes);
}

nlohmann::json RetainedMemoryGauge::toJson() const
{
    return {
        {"current-bytes", current_.load(std::memory_order_relaxed)},
        {"peak-bytes", peak_.load(std::memory_order_relaxed)},
    };
}

uint64_t RetainedMemoryGauge::current() const
{
    return current_.load(std::memory_order_relaxed);
}

void RetainedMemoryGauge::updatePeak(uint64_t candidate)
{
    auto peak = peak_.load(std::memory_order_relaxed);
    while (peak < candidate &&
           !peak_.compare_exchange_weak(
               peak,
               candidate,
               std::memory_order_relaxed)) {
    }
}

nlohmann::json FilterMemoryTracker::toJson()
{
    if (sampleOrchestration) {
        orchestration.set(sampleOrchestration());
    }
    auto const current =
        sourceTileModels.current() +
        outputSubsetModels.current() +
        relationTargetModels.current() +
        evaluationTemporaries.current() +
        orchestration.current();
    return {
        {"map-id", mapId},
        {"layer-id", layerId},
        {"filter-id", filterId},
        {"generation", generation},
        {"requested-tiles", requestedTiles},
        {"current-bytes", current},
        {"source-tile-models", sourceTileModels.toJson()},
        {"output-subset-models", outputSubsetModels.toJson()},
        {"relation-target-models", relationTargetModels.toJson()},
        {"evaluation-temporaries", evaluationTemporaries.toJson()},
        {"orchestration", orchestration.toJson()},
    };
}

nlohmann::json processMemoryStatistics()
{
    auto result = nlohmann::json{{"measurement", "unavailable"}};
#ifdef __linux__
    std::ifstream statusInput("/proc/self/status");
    std::unordered_map<std::string, std::string> fields;
    std::string line;
    while (std::getline(statusInput, line)) {
        auto const delimiter = line.find(':');
        if (delimiter != std::string::npos) {
            fields.emplace(line.substr(0, delimiter), line.substr(delimiter + 1));
        }
    }
    auto addBytes = [&](char const* output, char const* input) {
        if (auto value = procStatusBytes(fields, input)) {
            result[output] = *value;
        }
    };
    addBytes("resident-bytes", "VmRSS");
    addBytes("resident-peak-bytes", "VmHWM");
    addBytes("virtual-bytes", "VmSize");
    addBytes("swap-bytes", "VmSwap");
    addBytes("resident-anonymous-bytes", "RssAnon");
    addBytes("resident-file-bytes", "RssFile");
    addBytes("resident-shared-bytes", "RssShmem");
    if (auto threads = fields.find("Threads"); threads != fields.end()) {
        if (auto count = parseUnsigned(threads->second)) {
            result["threads"] = *count;
        }
    }
    result["measurement"] = "proc-self-status";

    std::ifstream cgroupInput("/proc/self/cgroup");
    std::string cgroupPath;
    std::string cgroupV1MemoryPath;
    while (std::getline(cgroupInput, line)) {
        if (line.starts_with("0::")) {
            cgroupPath = line.substr(3);
            break;
        }
        auto const firstColon = line.find(':');
        auto const secondColon = line.find(':', firstColon + 1);
        if (firstColon != std::string::npos &&
            secondColon != std::string::npos)
        {
            auto const controllers = line.substr(
                firstColon + 1,
                secondColon - firstColon - 1);
            if (controllers == "memory" ||
                controllers.starts_with("memory,") ||
                controllers.ends_with(",memory") ||
                controllers.find(",memory,") != std::string::npos)
            {
                cgroupV1MemoryPath = line.substr(secondColon + 1);
            }
        }
    }
    if (!cgroupPath.empty()) {
        auto root = std::string("/sys/fs/cgroup");
        if (cgroupPath != "/") {
            root += cgroupPath;
        }
        result["cgroup"] = {
            {"current-bytes", readByteCounterFile(root + "/memory.current")},
            {"peak-bytes", readByteCounterFile(root + "/memory.peak")},
            {"limit-bytes", readByteCounterFile(root + "/memory.max")},
        };
    }
    else if (!cgroupV1MemoryPath.empty()) {
        auto root = std::string("/sys/fs/cgroup/memory");
        if (cgroupV1MemoryPath != "/") {
            root += cgroupV1MemoryPath;
        }
        result["cgroup"] = {
            {"current-bytes", readByteCounterFile(root + "/memory.usage_in_bytes")},
            {"peak-bytes", readByteCounterFile(root + "/memory.max_usage_in_bytes")},
            {"limit-bytes", readByteCounterFile(root + "/memory.limit_in_bytes")},
        };
    }
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(
            mach_task_self(),
            MACH_TASK_BASIC_INFO,
            reinterpret_cast<task_info_t>(&info),
            &count) == KERN_SUCCESS)
    {
        result = {
            {"resident-bytes", info.resident_size},
            {"virtual-bytes", info.virtual_size},
            {"measurement", "mach-task-basic-info"},
        };
    }
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)))
    {
        result = {
            {"resident-bytes", counters.WorkingSetSize},
            {"resident-peak-bytes", counters.PeakWorkingSetSize},
            {"private-bytes", counters.PrivateUsage},
            {"virtual-commit-bytes", counters.PagefileUsage},
            {"measurement", "get-process-memory-info"},
        };
    }
#else
    result["measurement"] = "unavailable";
#endif
    return result;
}

nlohmann::json allocatorMemoryStatistics()
{
#if defined(__linux__) && defined(__GLIBC__)
    auto const allocator = mallinfo2();
    return {
        {"arena-bytes", allocator.arena},
        {"free-arena-bytes", allocator.fordblks},
        {"in-use-arena-bytes", allocator.uordblks},
        {"mmap-bytes", allocator.hblkhd},
        {"releasable-top-bytes", allocator.keepcost},
        {"measurement", "mallinfo2"},
    };
#else
    return nullptr;
#endif
}

simfil::MemoryUsage dataSourceInfoContainerMemoryUsage(DataSourceInfo const& info)
{
    MemoryUsageBreakdown usage;
    usage.add("object", {sizeof(DataSourceInfo), sizeof(DataSourceInfo)});
    usage.add("string-pool-id", stringMemoryUsage(info.stringPoolId_));
    usage.add("map-id", stringMemoryUsage(info.mapId_));
    usage.add("layer-index", {
        info.layers_.size() * sizeof(decltype(info.layers_)::value_type),
        info.layers_.bucket_count() * sizeof(void*) +
            info.layers_.size() *
                (sizeof(decltype(info.layers_)::value_type) + 2 * sizeof(void*)),
    });
    for (auto const& [layerId, _] : info.layers_) {
        usage.add("layer-ids", stringMemoryUsage(layerId));
    }
    usage.add("extra-json", jsonMemoryUsage(info.extraJsonAttachment_));
    return usage.total();
}

simfil::MemoryUsage dataSourceDescriptorMemoryUsage(DataSourceDescriptor const& descriptor)
{
    MemoryUsageBreakdown usage;
    usage.add("source-id", stringMemoryUsage(descriptor.sourceId));
    usage.add("type", stringMemoryUsage(descriptor.type));
    usage.add("display-name", stringMemoryUsage(descriptor.displayName));
    usage.add("auth-index", {
        descriptor.authHeaderAlternatives.size() *
            sizeof(decltype(descriptor.authHeaderAlternatives)::value_type),
        descriptor.authHeaderAlternatives.bucket_count() * sizeof(void*) +
            descriptor.authHeaderAlternatives.size() *
                (sizeof(decltype(descriptor.authHeaderAlternatives)::value_type) + 2 * sizeof(void*)),
    });
    for (auto const& [header, _] : descriptor.authHeaderAlternatives) {
        usage.add("auth-header-names", stringMemoryUsage(header));
    }
    return usage.total();
}

} // namespace mapget::detail
