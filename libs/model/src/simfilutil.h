#pragma once

#include <memory>
#include <mutex>
#include <string_view>

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

    auto eval(std::string_view query, bool anyMode, std::function<std::vector<simfil::Value>(const simfil::Expr&)> evalFun)
    {
        std::shared_lock s(mtx_);
        auto iter = cache_.find(query);
        if (iter != cache_.end())
            return evalFun(*iter->second);
        s.unlock();

        std::unique_lock u(mtx_);
        auto [newIter, _] = cache_.emplace(
            std::string(query),
            simfil::compile(*env_, query, anyMode, true)
        );
        return evalFun(*newIter->second);
    }

    std::vector<simfil::Value> eval(std::string_view query, simfil::ModelNode const& node, bool anyMode)
    {
        auto evalFun = [&](auto&& expr) {
            return simfil::eval(*env_, expr, node);
        };

        return eval(query, anyMode, evalFun);
    }

    const simfil::ExprPtr& compile(std::string_view query, bool anyMode)
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
    std::map<std::string, simfil::ExprPtr, std::less<>> cache_;
    std::unique_ptr<simfil::Environment> env_;
};

}
