#include "mapget/model/simfilexpressioncache.h"

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
public:
    explicit Impl(size_t maxEntries) : maxEntries_(maxEntries) {}

    tl::expected<simfil::SharedAST, simfil::Error> getOrCompile(
        std::string_view query,
        simfil::CompileOptions options,
        LayerSchema const* schemaIdentity,
        std::string_view compileContext,
        CompileFunction const& compile)
    {
        CacheKey key{
            std::string(query),
            options.any,
            options.rewriteMode,
            options.rootSchema,
            reinterpret_cast<std::uintptr_t>(schemaIdentity),
            std::string(compileContext),
        };

        {
            std::shared_lock lock(mutex_);
            if (auto found = cache_.find(key); found != cache_.end()) {
                ++hits_;
                return found->second.result();
            }
        }

        // Serializing misses prevents a large worker set from compiling the
        // same first-use expression concurrently. Hits remain shared-lock only.
        std::unique_lock lock(mutex_);
        if (auto found = cache_.find(key); found != cache_.end()) {
            ++hits_;
            return found->second.result();
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

        Entry entry;
        if (compiled) {
            entry.ast_ = simfil::SharedAST(std::move(*compiled));
        }
        else {
            entry.error_ = compiled.error();
            ++failedCompiles_;
        }

        if (cache_.size() >= maxEntries_)
            return entry.result();

        auto inserted = cache_.emplace(std::move(key), std::move(entry));
        return inserted.first->second.result();
    }

    void clear()
    {
        std::unique_lock lock(mutex_);
        cache_.clear();
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
            cache_.size(),
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
        simfil::MemoryUsage result{
            cache_.size() * (sizeof(CacheKey) + sizeof(Entry)),
            cache_.size() * (sizeof(decltype(cache_)::value_type) + 3 * sizeof(void*)),
        };
        for (auto const& [key, entry] : cache_) {
            result.logicalBytes += key.query_.size() + key.compileContext_.size();
            result.allocatedBytes += key.query_.capacity() + 1 + key.compileContext_.capacity() + 1;
            if (entry.error_) {
                result.logicalBytes += entry.error_->message.size();
                result.allocatedBytes += entry.error_->message.capacity() + 1;
            }
            if (entry.ast_) {
                result.logicalBytes += sizeof(simfil::AST);
                result.allocatedBytes += sizeof(simfil::AST);
            }
        }
        result.allocatedBytes = std::max(result.logicalBytes, result.allocatedBytes);
        return result;
    }

private:
    struct CacheKey
    {
        std::string query_;
        bool anyMode_ = true;
        simfil::RewriteMode rewriteMode_ = simfil::RewriteMode::None;
        simfil::SchemaId rootSchema_ = simfil::NoSchemaId;
        std::uintptr_t schemaIdentity_ = 0;
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

    struct Entry
    {
        simfil::SharedAST ast_;
        std::optional<simfil::Error> error_;

        [[nodiscard]] tl::expected<simfil::SharedAST, simfil::Error> result() const
        {
            if (error_)
                return tl::unexpected(*error_);
            return ast_;
        }
    };

    size_t maxEntries_ = DefaultMaxEntries;
    mutable std::shared_mutex mutex_;
    std::map<CacheKey, Entry> cache_;
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
    LayerSchema const* schemaIdentity,
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
