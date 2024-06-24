#pragma once

#include <memory>

#include "simfil/environment.h"
#include "simfil-geometry.h"

namespace mapget
{

/**
 * Callback type for a function which returns a field name cache instance
 * for a given node identifier.
 */
using FieldNameResolveFun = std::function<std::shared_ptr<simfil::Fields>(std::string_view const&)>;

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

}
