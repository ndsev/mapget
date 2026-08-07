// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "mapget/model/memory.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace mapget
{
namespace
{

/** Recursively measure JSON while avoiding double-counting child value objects. */
simfil::MemoryUsage jsonMemoryUsageInternal(nlohmann::json const& value)
{
    simfil::MemoryUsage result;

    if (value.is_string()) {
        auto const& string = value.get_ref<std::string const&>();
        auto const stringUsage = stringMemoryUsage(string);
        result.logicalBytes += stringUsage.logicalBytes;
        // nlohmann owns the std::string itself behind a pointer; parent JSON
        // storage therefore does not already contain the string object.
        result.allocatedBytes += sizeof(std::string) + stringUsage.allocatedBytes;
        return result;
    }
    if (value.is_binary()) {
        auto const& bytes = value.get_binary();
        result.logicalBytes += bytes.size();
        result.allocatedBytes += sizeof(nlohmann::json::binary_t) + bytes.capacity();
        return result;
    }
    if (value.is_array()) {
        auto const& array = value.get_ref<nlohmann::json::array_t const&>();
        result.logicalBytes += array.size() * sizeof(nlohmann::json);
        result.allocatedBytes += sizeof(nlohmann::json::array_t) +
            array.capacity() * sizeof(nlohmann::json);
        for (auto const& child : array) {
            // Child JSON values already live in the array's retained capacity.
            result += jsonMemoryUsageInternal(child);
        }
        return result;
    }
    if (value.is_object()) {
        auto const& object = value.get_ref<nlohmann::json::object_t const&>();
        // nlohmann's default object is node-based. Include occupied key/value
        // objects and three links per tree node; allocator metadata is omitted.
        result.logicalBytes += object.size() * sizeof(nlohmann::json::object_t::value_type);
        result.allocatedBytes += sizeof(nlohmann::json::object_t) + object.size() *
            (sizeof(nlohmann::json::object_t::value_type) + 3 * sizeof(void*));
        for (auto const& [key, child] : object) {
            result.logicalBytes += key.size();
            result.allocatedBytes += stringMemoryUsage(key).allocatedBytes;
            // Child JSON values already live in the map node counted above.
            result += jsonMemoryUsageInternal(child);
        }
    }
    return result;
}

} // namespace

void MemoryUsageBreakdown::add(std::string name, simfil::MemoryUsage usage)
{
    components[std::move(name)] += usage;
}

void MemoryUsageBreakdown::merge(std::string_view prefix, MemoryUsageBreakdown const& other)
{
    for (auto const& [name, usage] : other.components) {
        auto qualified = std::string(prefix);
        if (!qualified.empty()) {
            qualified += '.';
        }
        qualified += name;
        add(std::move(qualified), usage);
    }
}

simfil::MemoryUsage MemoryUsageBreakdown::total() const
{
    simfil::MemoryUsage result;
    for (auto const& [_, usage] : components) {
        result += usage;
    }
    return result;
}

nlohmann::json MemoryUsageBreakdown::toJson() const
{
    auto componentJson = nlohmann::json::object();
    for (auto const& [name, usage] : components) {
        componentJson[name] = {
            {"logical-bytes", usage.logicalBytes},
            {"allocated-bytes", usage.allocatedBytes},
            {"slack-bytes", usage.allocatedBytes >= usage.logicalBytes
                ? usage.allocatedBytes - usage.logicalBytes
                : 0},
        };
    }
    auto const totals = total();
    return {
        {"logical-bytes", totals.logicalBytes},
        {"allocated-bytes", totals.allocatedBytes},
        {"slack-bytes", totals.allocatedBytes >= totals.logicalBytes
            ? totals.allocatedBytes - totals.logicalBytes
            : 0},
        {"components", std::move(componentJson)},
        {"quality", "capacity-lower-bound"},
    };
}

simfil::MemoryUsage stringMemoryUsage(std::string const& value)
{
    auto const begin = reinterpret_cast<std::uintptr_t>(&value);
    auto const end = begin + sizeof(value);
    auto const data = reinterpret_cast<std::uintptr_t>(value.data());
    auto const usesInlineStorage = data >= begin && data < end;
    return {
        value.size(),
        usesInlineStorage ? size_t{0} : value.capacity() + 1,
    };
}

simfil::MemoryUsage jsonMemoryUsage(nlohmann::json const& value)
{
    // The root JSON value lives inline in its owner; only recursively allocated
    // backing storage belongs in this helper's additive result.
    auto result = jsonMemoryUsageInternal(value);
    result.allocatedBytes = std::max(result.logicalBytes, result.allocatedBytes);
    return result;
}

simfil::MemoryUsage diagnosticsMemoryUsage(simfil::Diagnostics const& diagnostics)
{
    auto result = vectorMemoryUsage(diagnostics.exprIndex_) +
        vectorMemoryUsage(diagnostics.fieldData_) +
        vectorMemoryUsage(diagnostics.comparisonData_);
    for (auto const& field : diagnostics.fieldData_) {
        result.logicalBytes += field.name.size();
        result.allocatedBytes += stringMemoryUsage(field.name).allocatedBytes;
    }
    result.allocatedBytes = std::max(result.logicalBytes, result.allocatedBytes);
    return result;
}

simfil::MemoryUsage stringVectorMemoryUsage(std::vector<std::string> const& values)
{
    auto result = vectorMemoryUsage(values);
    for (auto const& value : values) {
        result.logicalBytes += value.size();
        result.allocatedBytes += stringMemoryUsage(value).allocatedBytes;
    }
    result.allocatedBytes = std::max(result.logicalBytes, result.allocatedBytes);
    return result;
}

} // namespace mapget
