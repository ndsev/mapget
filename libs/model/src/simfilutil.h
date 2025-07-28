#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>

#include "featurelayer.h"

#include "simfil/error.h"
#include "tl/expected.hpp"

#include "simfil/environment.h"
#include "simfil/model/model.h"
#include "simfil/model/nodes.h"
#include "simfil/simfil.h"
#include "simfil-geometry.h"
#include "simfil/expression.h"

namespace mapget
{

/**
 * Callback type for a function which returns a StringPool instance
 * for a given node identifier.
 */
using StringPoolResolveFun = std::function<std::shared_ptr<simfil::StringPool>(std::string_view const&)>;

template <class... Args>
std::unique_ptr<simfil::Environment> makeEnvironment(Args&& ...args)
{
    auto env = std::make_unique<simfil::Environment>(std::forward<Args>(args)...);
    env->functions["geo"] = &mapget::GeoFn::Fn;
    env->functions["point"] = &mapget::PointFn::Fn;
    env->functions["bbox"] = &mapget::BBoxFn::Fn;
    env->functions["linestring"] = &mapget::LineStringFn::Fn;

    return env;
}

/**
 * Simfil compiled expression cache.
 */
struct SimfilExpressionCache
{
    explicit SimfilExpressionCache(std::unique_ptr<simfil::Environment> env)
        : env_(std::move(env))
    {}

    auto eval(std::string_view query, bool anyMode, bool autoWildcard, std::function<tl::expected<TileFeatureLayer::QueryResult, simfil::Error>(const simfil::AST&)> evalFun) -> tl::expected<TileFeatureLayer::QueryResult, simfil::Error>
    {
        std::shared_lock s(mtx_);
        auto iter = cache_.find(query);
        if (iter != cache_.end())
            return evalFun(*iter->second);
        s.unlock();

        std::unique_lock u(mtx_);
        auto ast = simfil::compile(*env_, query, anyMode, autoWildcard);
        if (!ast)
            return tl::unexpected<simfil::Error>(std::move(ast.error()));

        auto [newIter, _] = cache_.emplace(
            std::string(query),
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

        return eval(query, anyMode, autoWildcard, evalFun);
    }

    auto compile(std::string_view query, bool anyMode) -> tl::expected<std::reference_wrapper<const simfil::ASTPtr>, simfil::Error>
    {
        std::shared_lock s(mtx_);
        auto iter = cache_.find(query);
        if (iter != cache_.end())
            return iter->second;
        s.unlock();

        std::unique_lock u(mtx_);
        auto ast = simfil::compile(*env_, query, anyMode, true);
        if (!ast)
            return tl::unexpected<simfil::Error>(std::move(ast.error()));

        auto [newIter, _] = cache_.emplace(
            std::string(query),
            std::move(*ast)
        );
        return newIter->second;
    }

    auto diagnostics(std::string_view query, const simfil::Diagnostics& diag, bool anyMode) -> tl::expected<std::vector<simfil::Diagnostics::Message>, simfil::Error>
    {
        auto ast = compile(query, true);
        if (!ast)
            return tl::unexpected<simfil::Error>(std::move(ast.error()));

        return simfil::diagnostics(environment(), *ast->get(), diag);
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

    mutable std::shared_mutex mtx_;
    std::map<std::string, simfil::ASTPtr, std::less<>> cache_;
    std::unique_ptr<simfil::Environment> env_;
};

}
