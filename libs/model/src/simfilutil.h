#pragma once

#include <memory>
#include <mutex>
#include <string_view>

#include "featurelayer.h"

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

    auto eval(std::string_view query, bool anyMode, bool autoWildcard, std::function<TileFeatureLayer::QueryResult(const simfil::AST&)> evalFun) -> TileFeatureLayer::QueryResult
    {
        std::shared_lock s(mtx_);
        auto iter = cache_.find(query);
        if (iter != cache_.end())
            return evalFun(*iter->second);
        s.unlock();

        std::unique_lock u(mtx_);
        auto [newIter, _] = cache_.emplace(
            std::string(query),
            simfil::compile(*env_, query, anyMode, autoWildcard)
        );
        return evalFun(*newIter->second);
    }

    auto eval(std::string_view query, simfil::ModelNode const& node, bool anyMode, bool autoWildcard)
    {
        auto evalFun = [&](auto&& ast) {
            TileFeatureLayer::QueryResult r;
            r.values = simfil::eval(*env_, ast, node, &r.diagnostics);
            r.traces = env_->traces;

            return r;
        };

        return eval(query, anyMode, autoWildcard, evalFun);
    }

    const simfil::ASTPtr& compile(std::string_view query, bool anyMode)
    {
        std::shared_lock s(mtx_);
        auto iter = cache_.find(query);
        if (iter != cache_.end())
            return iter->second;
        s.unlock();

        std::unique_lock u(mtx_);
        auto [newIter, _] = cache_.emplace(
            std::string(query),
            simfil::compile(*env_, query, anyMode, true)
        );
        return newIter->second;
    }

    auto diagnostics(std::string_view query, const simfil::Diagnostics& diag) -> std::vector<simfil::Diagnostics::Message>
    {
        std::shared_lock s(mtx_);
        auto iter = cache_.find(query);
        if (iter == cache_.end())
            return {};

        return simfil::diagnostics(environment(), *iter->second, diag);
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
