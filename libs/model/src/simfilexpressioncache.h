#pragma once

#include <algorithm>
#include <compare>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>

#include "featurelayer.h"
#include "mapget/model/memory.h"

#include "simfil/error.h"
#include "tl/expected.hpp"

#include "simfil/environment.h"
#include "simfil/model/model.h"
#include "simfil/model/nodes.h"
#include "simfil/simfil.h"
#include "simfil/expression.h"

namespace mapget
{

/**
 * Callback type for a function which returns a StringPool instance
 * for a given node identifier.
 */
using StringPoolResolveFun = std::function<std::shared_ptr<simfil::StringPool>(std::string_view const&)>;
using CompileEnvironmentFactory = std::function<std::unique_ptr<simfil::Environment>()>;

/**
 * Simfil compiled expression cache.
 */
struct SimfilExpressionCache
{
    explicit SimfilExpressionCache(
        std::unique_ptr<simfil::Environment> env,
        CompileEnvironmentFactory compileEnvironmentFactory = {})
        : env_(std::move(env))
        , compileEnvironmentFactory_(std::move(compileEnvironmentFactory))
    {}

    struct CacheKey
    {
        std::string query;
        bool anyMode = true;
        simfil::RewriteMode rewriteMode = simfil::RewriteMode::None;
        simfil::SchemaId rootSchema = simfil::NoSchemaId;

        auto operator<=>(CacheKey const&) const = default;
    };

    auto eval(
        std::string_view query,
        bool anyMode,
        bool autoWildcard,
        simfil::SchemaId rootSchema,
        std::function<tl::expected<TileFeatureLayer::QueryResult, simfil::Error>(const simfil::AST&)> evalFun)
        -> tl::expected<TileFeatureLayer::QueryResult, simfil::Error>
    {
        auto const rewriteMode = autoWildcard && rootSchema != simfil::NoSchemaId
            ? simfil::RewriteMode::Schema
            : simfil::RewriteMode::None;
        auto key = CacheKey{std::string(query), anyMode, rewriteMode, rootSchema};

        std::shared_lock s(mtx_);
        auto iter = cache_.find(key);
        if (iter != cache_.end())
            return evalFun(*iter->second);
        s.unlock();

        std::unique_lock u(mtx_);
        auto compileEnv = compileEnvironmentFactory_ ? compileEnvironmentFactory_() : nullptr;
        auto& env = compileEnv ? *compileEnv : *env_;
        auto ast = simfil::compile(
            env,
            query,
            simfil::CompileOptions{
                .any = anyMode,
                .rewriteMode = rewriteMode,
                .rootSchema = rootSchema});
        if (!ast)
            return tl::unexpected<simfil::Error>(std::move(ast.error()));

        auto [newIter, _] = cache_.emplace(
            std::move(key),
            std::move(*ast)
        );
        return evalFun(*newIter->second);
    }

    auto eval(std::string_view query, simfil::ModelNode const& node, bool anyMode, bool autoWildcard) -> tl::expected<TileFeatureLayer::QueryResult, simfil::Error>
    {
        auto evalFun = [&](const simfil::AST& ast) -> tl::expected<TileFeatureLayer::QueryResult, simfil::Error> {
            TileFeatureLayer::QueryResult r;
            auto result = simfil::eval(*env_, ast, node, &r.diagnostics);
            if (!result)
                return tl::unexpected<simfil::Error>(std::move(result.error()));

            r.values = std::move(*result);
            r.traces = env_->traces;

            return r;
        };

        return eval(query, anyMode, autoWildcard, node.schema(), evalFun);
    }

    auto compile(
        std::string_view query,
        bool anyMode,
        bool autoWildcard,
        simfil::SchemaId rootSchema) -> tl::expected<std::reference_wrapper<const simfil::ASTPtr>, simfil::Error>
    {
        auto const rewriteMode = autoWildcard && rootSchema != simfil::NoSchemaId
            ? simfil::RewriteMode::Schema
            : simfil::RewriteMode::None;
        auto key = CacheKey{std::string(query), anyMode, rewriteMode, rootSchema};

        std::shared_lock s(mtx_);
        auto iter = cache_.find(key);
        if (iter != cache_.end())
            return iter->second;
        s.unlock();

        std::unique_lock u(mtx_);
        auto compileEnv = compileEnvironmentFactory_ ? compileEnvironmentFactory_() : nullptr;
        auto& env = compileEnv ? *compileEnv : *env_;
        auto ast = simfil::compile(
            env,
            query,
            simfil::CompileOptions{
                .any = anyMode,
                .rewriteMode = rewriteMode,
                .rootSchema = rootSchema});
        if (!ast)
            return tl::unexpected<simfil::Error>(std::move(ast.error()));

        auto [newIter, _] = cache_.emplace(
            std::move(key),
            std::move(*ast)
        );
        return newIter->second;
    }

    auto diagnostics(
        std::string_view query,
        const simfil::Diagnostics& diag,
        bool anyMode,
        simfil::SchemaId rootSchema = simfil::NoSchemaId) -> tl::expected<std::vector<simfil::Diagnostics::Message>, simfil::Error>
    {
        auto ast = compile(query, anyMode, true, rootSchema);
        if (!ast)
            return tl::unexpected<simfil::Error>(std::move(ast.error()));

        return simfil::diagnostics(diag);
    }

    auto completions(std::string_view query, size_t point, simfil::ModelNode const& node, simfil::CompletionOptions const& opts) -> tl::expected<std::vector<simfil::CompletionCandidate>, simfil::Error>
    {
        return simfil::complete(environment(), query, point, node, opts);
    }

    void reset(std::unique_ptr<simfil::Environment> env)
    {
        std::unique_lock l(mtx_);
        cache_.clear();
        env_ = std::move(env);
    }

    simfil::Environment& environment()
    {
        return *env_;
    }

    /**
     * Estimate retained cache keys and AST owner nodes.
     *
     * AST-internal allocations and Environment registries are intentionally
     * left to the process-level unclassified remainder.
     */
    [[nodiscard]] simfil::MemoryUsage memoryUsage() const
    {
        std::shared_lock lock(mtx_);
        simfil::MemoryUsage result{
            cache_.size() * sizeof(decltype(cache_)::value_type),
            cache_.size() *
                (sizeof(decltype(cache_)::value_type) + 3 * sizeof(void*)),
        };
        for (auto const& [key, _] : cache_) {
            result.logicalBytes += key.query.size();
            result.allocatedBytes += key.query.capacity() + 1;
        }
        result.allocatedBytes = std::max(result.logicalBytes, result.allocatedBytes);
        return result;
    }

    mutable std::shared_mutex mtx_;
    std::map<CacheKey, simfil::ASTPtr> cache_;
    std::unique_ptr<simfil::Environment> env_;
    CompileEnvironmentFactory compileEnvironmentFactory_;
};

}
