// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "mapget/service/detail/allocator-memory.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <type_traits>

#if defined(__linux__)
#include <dlfcn.h>
#include <sys/types.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#endif

namespace mapget::detail
{
namespace
{

#if defined(__linux__)
using Mallctl = int (*)(char const*, void*, size_t*, void*, size_t);

/** Match a control entry point to the DSO serving process-wide malloc. */
bool belongsToProcessAllocator(void* symbol)
{
    Dl_info allocator{}, control{};
    return symbol && dladdr(dlsym(RTLD_DEFAULT, "malloc"), &allocator) &&
        dladdr(symbol, &control) && allocator.dli_fbase == control.dli_fbase;
}

/** Resolve jemalloc without introducing an allocator dependency into Python/DSOs. */
Mallctl jemallocControl()
{
    static auto const control = []() -> Mallctl
    {
        auto* symbol = dlsym(RTLD_DEFAULT, "mallctl");
        return belongsToProcessAllocator(symbol) ? reinterpret_cast<Mallctl>(symbol) : nullptr;
    }();
    return control;
}

/** Omit unsupported mallctl counters rather than reporting invented zeroes. */
template <typename T>
std::optional<T> readJemalloc(Mallctl control, char const* name)
{
    T value{};
    size_t size = sizeof(value);
    if (control(name, &value, &size, nullptr, 0) != 0 || size != sizeof(value)) {
        return std::nullopt;
    }
    return value;
}
#endif

#if defined(__linux__) && defined(__GLIBC__)
/** Older manylinux glibc versions expose signed, potentially overflowing counters. */
template <typename T>
uint64_t allocatorByteCount(T value)
{
    if constexpr (std::is_signed_v<T>) {
        return value > 0 ? static_cast<uint64_t>(value) : 0;
    }
    return static_cast<uint64_t>(value);
}
#endif

}  // namespace

bool allocatorTrimSupported()
{
#if defined(__linux__) && defined(__GLIBC__)
    // A preloaded allocator (or sanitizer) may replace malloc even in a glibc build.
    static bool const supported = belongsToProcessAllocator(dlsym(RTLD_DEFAULT, "__libc_malloc"));
    return supported;
#else
    return false;
#endif
}

nlohmann::json allocatorMemoryStatistics()
{
#if defined(__linux__)
    if (auto const control = jemallocControl()) {
        nlohmann::json result = {{"backend", "jemalloc"}, {"measurement", "mallctl"}};
        if (auto const version = readJemalloc<char const*>(control, "version")) {
            result["version"] = *version;
        }
        if (auto const enabled = readJemalloc<bool>(control, "background_thread")) {
            result["background-thread-enabled"] = *enabled;
        }
        for (auto const* name : {"dirty", "muzzy"}) {
            auto const key = std::string("opt.") + name + "_decay_ms";
            if (auto const decay = readJemalloc<ssize_t>(control, key.c_str())) {
                result[std::string(name) + "-decay-ms"] = *decay;
            }
        }

        // Statistics are cached by jemalloc; refresh once per status sample, not
        // per counter. This does not force a purge or flush other threads' caches.
        uint64_t epoch = 1;
        if (control("epoch", nullptr, nullptr, &epoch, sizeof(epoch)) == 0) {
            for (auto const* name :
                 {"allocated", "active", "resident", "mapped", "retained", "metadata"}) {
                if (auto const bytes =
                        readJemalloc<size_t>(control, (std::string("stats.") + name).c_str())) {
                    result[std::string(name) + "-bytes"] = *bytes;
                }
            }
            if (auto const count =
                    readJemalloc<size_t>(control, "stats.background_thread.num_threads")) {
                result["background-thread-count"] = *count;
            }
            if (auto const runs =
                    readJemalloc<uint64_t>(control, "stats.background_thread.num_runs")) {
                result["background-thread-runs"] = *runs;
            }
        }
        return result;
    }
#endif

#if defined(__linux__) && defined(__GLIBC__)
    if (allocatorTrimSupported()) {
#if defined(__GLIBC_PREREQ) && __GLIBC_PREREQ(2, 33)
        auto const allocator = mallinfo2();
        auto const measurement = "mallinfo2";
#else
        auto const allocator = mallinfo();
        auto const measurement = "mallinfo";
#endif
        auto const arenaLive = allocatorByteCount(allocator.uordblks);
        auto const mmap = allocatorByteCount(allocator.hblkhd);
        return {
            {"backend", "glibc"},
            {"allocated-bytes", arenaLive + mmap},
            {"arena-bytes", allocatorByteCount(allocator.arena)},
            {"free-arena-bytes", allocatorByteCount(allocator.fordblks)},
            {"in-use-arena-bytes", arenaLive},
            {"mmap-bytes", mmap},
            {"releasable-top-bytes", allocatorByteCount(allocator.keepcost)},
            {"measurement", measurement},
        };
    }
#endif
    return nullptr;
}

}  // namespace mapget::detail
