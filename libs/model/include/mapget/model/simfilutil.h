#pragma once

#include <memory>

#include "simfilgeometry.h"

#include "simfil/environment.h"
#include "simfil/model/string-pool.h"

namespace mapget
{

class LayerSchema;

/** Attach a read-only LayerSchema lookup callback to an existing SIMFIL environment. */
void installLayerSchema(
    simfil::Environment& env,
    std::shared_ptr<LayerSchema const> registry,
    std::shared_ptr<simfil::StringPool const> strings);

/** Attach a completion/compile-only LayerSchema callback which materializes schema strings locally. */
void installCompletionLayerSchema(
    simfil::Environment& env,
    std::shared_ptr<LayerSchema const> registry,
    std::shared_ptr<simfil::StringPool> strings);

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
