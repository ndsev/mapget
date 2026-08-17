// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

namespace mapget::detail
{

struct AllocatorMemorySnapshot
{
    uint64_t arenaBytes = 0;
    uint64_t freeArenaBytes = 0;
    uint64_t inUseArenaBytes = 0;
    uint64_t mmapBytes = 0;
    uint64_t releasableTopBytes = 0;
    char const* measurement = "unavailable";
};

template <typename T>
[[nodiscard]] uint64_t allocatorByteCount(T value)
{
    if constexpr (std::is_signed_v<T>) {
        return value > 0 ? static_cast<uint64_t>(value) : 0;
    }
    return static_cast<uint64_t>(value);
}

/** Normalize glibc allocator counters across the manylinux glibc baselines. */
[[nodiscard]] inline std::optional<AllocatorMemorySnapshot> allocatorMemorySnapshot()
{
#if defined(__linux__) && defined(__GLIBC__)
#if defined(__GLIBC_PREREQ) && __GLIBC_PREREQ(2, 33)
    auto const allocator = mallinfo2();
    return AllocatorMemorySnapshot{
        allocatorByteCount(allocator.arena),
        allocatorByteCount(allocator.fordblks),
        allocatorByteCount(allocator.uordblks),
        allocatorByteCount(allocator.hblkhd),
        allocatorByteCount(allocator.keepcost),
        "mallinfo2",
    };
#else
    auto const allocator = mallinfo();
    return AllocatorMemorySnapshot{
        allocatorByteCount(allocator.arena),
        allocatorByteCount(allocator.fordblks),
        allocatorByteCount(allocator.uordblks),
        allocatorByteCount(allocator.hblkhd),
        allocatorByteCount(allocator.keepcost),
        "mallinfo",
    };
#endif
#else
    return std::nullopt;
#endif
}

} // namespace mapget::detail
