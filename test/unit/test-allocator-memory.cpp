// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "mapget/service/detail/allocator-memory.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace mapget::detail;
using namespace std::chrono_literals;

TEST_CASE("Allocator reporting identifies the allocator serving the process", "[allocator]")
{
    auto const stats = allocatorMemoryStatistics();
#if MAPGET_TEST_JEMALLOC
    REQUIRE(stats["backend"] == "jemalloc");
    REQUIRE(stats["measurement"] == "mallctl");
    REQUIRE_FALSE(allocatorTrimSupported());
    REQUIRE_FALSE(stats.contains("free-arena-bytes"));
    auto const* expected = std::getenv("MAPGET_TEST_BACKGROUND_THREADS");
    REQUIRE(stats["background-thread-enabled"] == (!expected || std::strcmp(expected, "0") != 0));
    REQUIRE(stats["active-bytes"].get<uint64_t>() >= stats["allocated-bytes"].get<uint64_t>());
    REQUIRE(stats["resident-bytes"].get<uint64_t>() >= stats["active-bytes"].get<uint64_t>());
    REQUIRE(stats.contains("retained-bytes"));
    REQUIRE(stats.contains("metadata-bytes"));
#elif defined(__linux__) && defined(__GLIBC__)
    if (stats.is_null()) {
        REQUIRE_FALSE(allocatorTrimSupported());
        SKIP("Process allocator replaced by an unsupported allocator, e.g. a sanitizer");
    }
    REQUIRE(stats["backend"] == "glibc");
    REQUIRE(allocatorTrimSupported());
    REQUIRE(
        stats["allocated-bytes"].get<uint64_t>() ==
        stats["in-use-arena-bytes"].get<uint64_t>() + stats["mmap-bytes"].get<uint64_t>());
    REQUIRE_FALSE(stats.contains("retained-bytes"));
#else
    REQUIRE(stats.is_null());
    REQUIRE_FALSE(allocatorTrimSupported());
#endif
}

TEST_CASE("Allocator counters track C and C++ allocations freed on another thread", "[allocator]")
{
    auto const before = allocatorMemoryStatistics();
    if (before.is_null()) {
        SKIP("The process allocator does not expose allocation counters");
    }
    constexpr size_t size = 32 * 1024 * 1024;
    constexpr size_t tolerance = 1024 * 1024;
    auto const baseline = before["allocated-bytes"].get<uint64_t>();
    auto cpp = std::make_unique<char[]>(size);
    auto c = std::unique_ptr<void, decltype(&std::free)>(std::malloc(size), &std::free);
    REQUIRE(c);
    std::memset(c.get(), 0x5a, size);
    auto const allocated = allocatorMemoryStatistics()["allocated-bytes"].get<uint64_t>();
    REQUIRE(allocated + tolerance >= baseline + 2 * size);

    // C++ delete and C free must agree on ownership across worker boundaries.
    std::thread release(
        [cpp = std::move(cpp), c = std::move(c)]() mutable
        {
            cpp.reset();
            c.reset();
        });
    release.join();
    auto const released = allocatorMemoryStatistics()["allocated-bytes"].get<uint64_t>();
    REQUIRE(released + 2 * size <= allocated + tolerance);
}

#if MAPGET_TEST_JEMALLOC
TEST_CASE(
    "jemalloc background purging reclaims unused pages without malloc_trim",
    "[allocator-purge]")
{
    constexpr size_t blockSize = 1024 * 1024;
    std::vector<std::unique_ptr<char[]>> blocks;
    for (size_t index = 0; index < 128; ++index) {
        auto block = std::make_unique<char[]>(blockSize);
        // Make the pages physically resident; do not rely on untouched virtual allocations.
        volatile char* bytes = block.get();
        for (size_t offset = 0; offset < blockSize; offset += 4096) {
            bytes[offset] = 1;
        }
        blocks.push_back(std::move(block));
    }
    auto const loaded = allocatorMemoryStatistics();
    REQUIRE(loaded["background-thread-enabled"] == true);
    auto const loadedResident = loaded["resident-bytes"].get<uint64_t>();
    blocks.clear();

    // CTest shortens decay for this test only. Sampling refreshes counters but
    // never flushes caches, requests decay, or purges arenas itself.
    // Require most pages back: free() can release roughly half synchronously,
    // so a smaller threshold would not actually exercise background reclamation.
    constexpr size_t minimumReclaimed = 112 * blockSize;
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    auto current = allocatorMemoryStatistics();
    while (current["resident-bytes"].get<uint64_t>() + minimumReclaimed > loadedResident &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(100ms);
        current = allocatorMemoryStatistics();
    }
    REQUIRE(current["resident-bytes"].get<uint64_t>() + minimumReclaimed <= loadedResident);
    REQUIRE(current["background-thread-runs"].get<uint64_t>() > 0);
}
#endif
