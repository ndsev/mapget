#pragma once

#include <memory>

#include "simfilgeometry.h"

#include "simfil/environment.h"
#include "simfil/model/string-pool.h"

namespace mapget
{

/** Construct an owned Simfil environment and register mapget's planar geometry functions. */
template <class... Args>
std::unique_ptr<simfil::Environment> makeEnvironment(Args&&... args)
{
    auto env = std::make_unique<simfil::Environment>(std::forward<Args>(args)...);
    env->functions["geo"] = &mapget::GeoFn::Fn;
    env->functions["point"] = &mapget::PointFn::Fn;
    env->functions["bbox"] = &mapget::BBoxFn::Fn;
    env->functions["linestring"] = &mapget::LineStringFn::Fn;

    return env;
}
}
