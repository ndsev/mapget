#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include "mapget/model/memory.h"

#include "simfil/error.h"
#include "simfil/expression.h"
#include "simfil/simfil.h"
#include "tl/expected.hpp"

namespace mapget
{

class LayerSchema;

/**
 * Thread-safe cache of immutable compiled SIMFIL expressions.
 *
 * The cache deliberately owns no evaluation Environment. Callers provide the
 * compilation operation on a miss and bind the returned SharedAST to their own
 * worker-local Environment. The schema and compile-context identities isolate
 * ASTs whose schema rewrites or constants differ despite identical query text.
 */
class SimfilExpressionCache
{
public:
    static constexpr size_t DefaultMaxEntries = 4096;
    using CompileFunction = std::function<tl::expected<simfil::ASTPtr, simfil::Error>()>;

    /** Snapshot of cache reuse and compilation work since construction or clear(). */
    struct Statistics
    {
        size_t entries = 0;
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t compiles = 0;
        uint64_t failedCompiles = 0;
        uint64_t compileMicroseconds = 0;
    };

    /** Create a cache which stops retaining new entries after `maxEntries`. */
    explicit SimfilExpressionCache(size_t maxEntries = DefaultMaxEntries);

    /** Release all retained expressions and failures. */
    ~SimfilExpressionCache();

    SimfilExpressionCache(SimfilExpressionCache const&) = delete;
    auto operator=(SimfilExpressionCache const&) -> SimfilExpressionCache& = delete;

    /**
     * Return one shared AST, compiling on a miss and retaining the result while
     * capacity permits. Deterministic compilation failures are retained too.
     *
     * `schemaIdentity` must remain immutable while its entries are cached.
     * `compileContext` must distinguish every other compile-affecting input,
     * including constants, bindings, and custom compiler environment state. It
     * need only be stable for this cache's lifetime.
     */
    [[nodiscard]] tl::expected<simfil::SharedAST, simfil::Error> getOrCompile(
        std::string_view query,
        simfil::CompileOptions options,
        LayerSchema const* schemaIdentity,
        std::string_view compileContext,
        CompileFunction const& compile);

    /** Discard every retained AST/failure and reset statistics. */
    void clear();

    /** Return exact counters without exposing cache internals. */
    [[nodiscard]] Statistics statistics() const;

    /** Estimate retained keys, entries, and shared AST owner objects. */
    [[nodiscard]] simfil::MemoryUsage memoryUsage() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mapget
