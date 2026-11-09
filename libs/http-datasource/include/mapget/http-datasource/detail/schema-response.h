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
 *
 * This is HTTP response storage, not the configurable tile Cache. That interface
 * stores partition-keyed layer blobs and source dictionaries; schemas instead
 * belong to immutable MapSchema owners shared by many maps and partitions.
 * DataSourceServer also uses this encoder without a Service or tile Cache.
 * Putting these bytes into the tile cache would invent a partition identity,
 * affect tile eviction, and still require a separate owner for active streams.
 *
 * At most one artifact (up to MaxArtifactBytes) is retained for reuse here;
 * replacing it releases the old buffer once its active responses finish. The
 * --cache-max-tiles/--cache-max-bytes options govern tile storage, not response
 * buffers. HttpService reports both retained and in-flight schema bytes under
 * schema transport memory in /status-data, separately from the tile cache.
 */
class SchemaResponseCache
{
public:
    static constexpr size_t MaxArtifactBytes = 64 * 1024 * 1024;

    /**
     * Encode/reuse the finalized graph and apply If-None-Match to select 200 or 304.
     * Marks responses private and requiring revalidation; authorization is the caller's
     * responsibility. Throws for a missing/unfinalized graph or serialization failure.
     */
    [[nodiscard]] drogon::HttpResponsePtr
    response(std::shared_ptr<MapSchema const> const& schema, std::string_view ifNoneMatch);

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
