// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "simfil/diagnostics.h"
#include "simfil/model/memory.h"

namespace mapget
{

/** Named capacity-oriented components which together own one memory footprint. */
struct MemoryUsageBreakdown
{
    std::map<std::string, simfil::MemoryUsage, std::less<>> components;

    /** Insert or accumulate an independently owned component. */
    void add(std::string name, simfil::MemoryUsage usage);

    /** Merge another breakdown, qualifying every imported component. */
    void merge(std::string_view prefix, MemoryUsageBreakdown const& other);

    /** Sum all independently owned components. */
    [[nodiscard]] simfil::MemoryUsage total() const;

    /** Render totals and component-level measurements for diagnostics APIs. */
    [[nodiscard]] nlohmann::json toJson() const;
};

/** Estimate one string's live characters and external (non-SSO) character allocation. */
[[nodiscard]] simfil::MemoryUsage stringMemoryUsage(std::string const& value);

/** Estimate recursively allocated JSON backing storage, excluding the inline root value. */
[[nodiscard]] simfil::MemoryUsage jsonMemoryUsage(nlohmann::json const& value);

/** Measure diagnostics vector storage and retained field-name strings. */
[[nodiscard]] simfil::MemoryUsage diagnosticsMemoryUsage(simfil::Diagnostics const& diagnostics);

/** Measure a vector's element payload and retained element capacity. */
template<typename T>
[[nodiscard]] simfil::MemoryUsage vectorMemoryUsage(std::vector<T> const& values)
{
    return {
        values.size() * sizeof(T),
        values.capacity() * sizeof(T),
    };
}

/** Measure vector storage plus the character buffers owned by its strings. */
[[nodiscard]] simfil::MemoryUsage stringVectorMemoryUsage(std::vector<std::string> const& values);

} // namespace mapget
