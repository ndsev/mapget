#pragma once

#include <drogon/HttpResponse.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace mapget
{
class MapSchema;

namespace detail
{

/**
 * Bounded binary schema representation cache for the two HTTP server boundaries.
 * Retains only the latest encoded artifact, never its decoded owner. Response streams
 * share the immutable bytes until transmission ends; eviction does not invalidate them.
 * Callers must authorize the exact current source binding before every call, including
 * conditional requests. Encoding and waiting belong off the HTTP event loop.
 */
class SchemaResponseCache
{
public:
    static constexpr size_t MaxArtifactBytes = 64 * 1024 * 1024;

    /** Return a private, revalidated 200/304 response; throw on serialization failure. */
    [[nodiscard]] drogon::HttpResponsePtr response(
        std::shared_ptr<MapSchema const> const& schema,
        std::string_view ifNoneMatch);

    /** All encoded buffer capacity, counted once across cache and active response streams. */
    [[nodiscard]] size_t allocatedCapacity() const noexcept;

    /** Subset retained by the cache itself; a lock-free snapshot excludes evicted streams. */
    [[nodiscard]] size_t retainedCapacity() const noexcept;

private:
    mutable std::mutex mutex_;
    std::weak_ptr<MapSchema const> owner_;
    std::shared_ptr<std::string const> bytes_;
    std::string etag_;
    // A stream may outlive this cache, so its buffer deleter owns the allocation counter.
    std::shared_ptr<std::atomic_size_t> allocatedCapacity_ = std::make_shared<std::atomic_size_t>(0);
    std::atomic_size_t retainedCapacity_{0};
};

} // namespace detail
} // namespace mapget
