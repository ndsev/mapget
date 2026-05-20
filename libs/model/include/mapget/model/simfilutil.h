#pragma once

#include <memory>

#include "simfilgeometry.h"

#include "simfil/environment.h"
#include "simfil/model/string-pool.h"

namespace mapget
{

class SchemaRegistry;

/** Attach a read-only SchemaRegistry lookup callback to an existing SIMFIL environment. */
void installSchemaRegistry(
    simfil::Environment& env,
    std::shared_ptr<SchemaRegistry const> registry,
    std::shared_ptr<simfil::StringPool const> strings);

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
