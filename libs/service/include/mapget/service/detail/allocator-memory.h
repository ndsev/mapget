// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <nlohmann/json_fwd.hpp>

namespace mapget::detail
{

/**
 * Sample the allocator actually serving malloc, not a second allocator merely
 * loaded by a library. Refresh jemalloc's cached statistics before reading them;
 * unsupported allocators return null rather than misleading glibc counters.
 */
[[nodiscard]] nlohmann::json allocatorMemoryStatistics();

/** Whether malloc_trim would act on the process allocator (Linux glibc only). */
[[nodiscard]] bool allocatorTrimSupported();

} // namespace mapget::detail
