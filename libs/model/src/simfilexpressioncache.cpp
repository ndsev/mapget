#include "mapget/model/simfilexpressioncache.h"
#include "mapget/model/layerschema.h"
#include "mapget/model/mapschema.h"
#include "mapschema-pointquery.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <tuple>

namespace mapget
{

class SimfilExpressionCache::Impl
{
    using Result = tl::expected<simfil::SharedAST, simfil::Error>;

public:
    explicit Impl(size_t maxEntries) : maxEntries_(maxEntries) {}

    tl::expected<simfil::SharedAST, simfil::Error> getOrCompile(
        std::string_view query,
        simfil::CompileOptions options,
        simfil::model_ptr<LayerSchema> const& schemaIdentity,
        std::string_view compileContext,
        CompileFunction const& compile)
    {
        // Both trusted owners derive their identities from verified source content.
        // A generic caller can publish the same string but not the same typed namespace.
        auto const* pool = options.rootSchema.pool();
        auto const* map = dynamic_cast<MapSchema const*>(pool);
        bool const pointRoot = pool && !map && isPublishedPointQueryPool(pool);
        bool const cacheable = !options.rootSchema || (map && map->finalized()) || pointRoot;
        CacheKey key{
            std::string(query),
            options.any,
            options.rewriteMode,
            options.rootSchema.id(),
            std::pair(
                options.rootSchema ? std::string(options.rootSchema.pool()->identity()) :
                    schemaIdentity ? std::string(schemaIdentity->mapSchema()->schemaId()) :
                                     std::string{},
                schemaIdentity ? std::string(schemaIdentity->name()) : std::string{}),
            std::string(compileContext),
        };

        auto lookup = [&]() -> std::optional<Result> {
            if (pointRoot) {
                if (auto it = pointPrograms_.find(key); it != pointPrograms_.end())
                    if (auto ast = it->second.lock())
                        return Result(std::move(ast));
            }
            if (auto found = cache_.find(key); found != cache_.end()) {
                return found->second;
            }
            return std::nullopt;
        };
        if (cacheable) {
            std::shared_lock lock(mutex_);
            if (auto hit = lookup()) {
                ++hits_;
                return std::move(*hit);
            }
        }

        // Serializing misses prevents a large worker set from compiling the
        // same first-use expression concurrently. Hits remain shared-lock only.
        std::unique_lock lock(mutex_);
        if (cacheable) {
            if (auto hit = lookup()) {
                ++hits_;
                return std::move(*hit);
            }
            if (pointRoot)
                pointPrograms_.erase(key);  // A miss for an existing weak key means it expired.
        }
        ++misses_;

        // Once bounded retention is full, compile without growing the cache.
        // Keep the lock so another worker cannot mistake this uncached miss for
        // an opportunity to compile the same expression in parallel.
        auto const startedAt = std::chrono::steady_clock::now();
        auto compiled = compile();
        compileMicroseconds_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - startedAt)
                .count());
        ++compiles_;

        Result result;
        if (compiled) {
            result = simfil::SharedAST(std::move(*compiled));
        }
        else {
            result = tl::unexpected(std::move(compiled.error()));
            ++failedCompiles_;
        }

        if (cacheable && maxEntries_) {
            if (entries() >= maxEntries_)
                std::erase_if(pointPrograms_, [](auto const& pair) { return pair.second.expired(); });
            if (entries() < maxEntries_) {
                // A schema-bound expression can own its compilation pool through an AST child.
                if (pointRoot && result)
                    pointPrograms_.emplace(std::move(key), *result);
                else
                    cache_.emplace(std::move(key), result);
            }
        }
        return result;
    }

    void clear()
    {
        std::unique_lock lock(mutex_);
        cache_.clear();
        pointPrograms_.clear();
        hits_.store(0, std::memory_order_relaxed);
        misses_.store(0, std::memory_order_relaxed);
        compiles_.store(0, std::memory_order_relaxed);
        failedCompiles_.store(0, std::memory_order_relaxed);
        compileMicroseconds_.store(0, std::memory_order_relaxed);
    }

    Statistics statistics() const
    {
        std::shared_lock lock(mutex_);
        return {
            entries(),
            hits_.load(std::memory_order_relaxed),
            misses_.load(std::memory_order_relaxed),
            compiles_.load(std::memory_order_relaxed),
            failedCompiles_.load(std::memory_order_relaxed),
            compileMicroseconds_.load(std::memory_order_relaxed),
        };
    }

    simfil::MemoryUsage memoryUsage() const
    {
        std::shared_lock lock(mutex_);
        simfil::MemoryUsage result{};
        auto accountKey = [&](CacheKey const& key, size_t valueBytes) {
            result.logicalBytes += sizeof(CacheKey) + valueBytes;
            result.allocatedBytes += sizeof(CacheKey) + valueBytes + 3 * sizeof(void*);
            result.logicalBytes += key.query_.size() + key.compileContext_.size();
            result.allocatedBytes += key.query_.capacity() + 1 + key.compileContext_.capacity() + 1;
            for (auto const* identity : {&key.schemaIdentity_.first, &key.schemaIdentity_.second}) {
                auto usage = stringMemoryUsage(*identity);
                result.logicalBytes += usage.logicalBytes;
                result.allocatedBytes += usage.allocatedBytes;
            }
        };
        for (auto const& [key, entry] : cache_) {
            accountKey(key, sizeof(entry));
            if (!entry) {
                result.logicalBytes += entry.error().message.size();
                result.allocatedBytes += entry.error().message.capacity() + 1;
            }
            else if (*entry) {
                result.logicalBytes += sizeof(simfil::AST);
                result.allocatedBytes += sizeof(simfil::AST);
            }
        }
        for (auto const& [key, weak] : pointPrograms_)
            accountKey(key, sizeof(weak));
        result.allocatedBytes = std::max(result.logicalBytes, result.allocatedBytes);
        return result;
    }

private:
    struct CacheKey
    {
        std::string query_;
        bool anyMode_ = true;
        simfil::RewriteMode rewriteMode_ = simfil::RewriteMode::None;
        simfil::TypeId rootSchema_ = simfil::NoTypeId;
        // LayerSchema is now a temporary ModelNode view. Pointer identity would alias unrelated
        // layers and dangle; the exact artifact + layer name is stable across readers/reloads.
        std::pair<std::string, std::string> schemaIdentity_;
        std::string compileContext_;

        bool operator<(CacheKey const& other) const
        {
            return std::tie(
                       query_,
                       anyMode_,
                       rewriteMode_,
                       rootSchema_,
                       schemaIdentity_,
                       compileContext_) <
                std::tie(
                       other.query_,
                       other.anyMode_,
                       other.rewriteMode_,
                       other.rootSchema_,
                       other.schemaIdentity_,
                       other.compileContext_);
        }
    };

    size_t entries() const { return cache_.size() + pointPrograms_.size(); }

    size_t maxEntries_ = DefaultMaxEntries;
    mutable std::shared_mutex mutex_;
    std::map<CacheKey, Result> cache_;
    std::map<CacheKey, std::weak_ptr<simfil::AST const>> pointPrograms_;
    std::atomic_uint64_t hits_ = 0;
    std::atomic_uint64_t misses_ = 0;
    std::atomic_uint64_t compiles_ = 0;
    std::atomic_uint64_t failedCompiles_ = 0;
    std::atomic_uint64_t compileMicroseconds_ = 0;
};

SimfilExpressionCache::SimfilExpressionCache(size_t maxEntries)
    : impl_(std::make_unique<Impl>(maxEntries))
{
}

SimfilExpressionCache::~SimfilExpressionCache() = default;

tl::expected<simfil::SharedAST, simfil::Error> SimfilExpressionCache::getOrCompile(
    std::string_view query,
    simfil::CompileOptions options,
    simfil::model_ptr<LayerSchema> const& schemaIdentity,
    std::string_view compileContext,
    CompileFunction const& compile)
{
    return impl_->getOrCompile(query, options, schemaIdentity, compileContext, compile);
}

void SimfilExpressionCache::clear()
{
    impl_->clear();
}

SimfilExpressionCache::Statistics SimfilExpressionCache::statistics() const
{
    return impl_->statistics();
}

simfil::MemoryUsage SimfilExpressionCache::memoryUsage() const
{
    return impl_->memoryUsage();
}

}  // namespace mapget
