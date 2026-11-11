#include "mapschema-impl.h"

namespace mapget
{

std::vector<simfil::SchemaPath> MapSchema::symbolEqualityPaths(TypeId id, StringId symbol) const
{
    auto const& graph = std::as_const(*schema_);
    if (!symbol || !graph.valid(id) || !(graph.data(id).flags & Impl::QueryRootFlag))
        return {};
    auto type = schemaType(id);
    auto payload = type.baseType();
    auto feature = type.field("$feature").type();
    auto name = dictionary().get("$name");
    if (!payload || !feature || !name || !type.field(name) ||
        graph.findQueryRoot(feature.id(), payload.id()) != id)
        return {};

    // Query overlays contain only the owning feature's applicable use sites.
    // A physical payload's global label is not a substitute for runtime $name.
    for (auto const& owner : graph.ownerSites(graph.data(id).owners))
        if (owner.name == symbol)
            return {{{simfil::SchemaPathSegment::Kind::Field, name}}};
    return {};
}

std::vector<simfil::SchemaPath>
MapSchema::scalarFieldPathsForSymbol(TypeId id, StringId symbol) const
{
    auto const& graph = std::as_const(*schema_);
    auto name = dictionary().resolve(symbol);
    if (!graph.valid(id) || !name || name->empty())
        return {};
    auto const& row = graph.data(id);
    if (row.flags & Impl::QueryRootFlag) {
        auto owners = graph.ownerSites(row.owners);
        if (!owners.empty() &&
            std::ranges::any_of(
                owners,
                [&](auto const& owner) { return owner.name != owners.front().name; }))
            return {};  // A scalar path alone cannot enforce a shared context's $name guard.
    }

    auto named = graph.scalarPaths(id, *name);
    std::vector<simfil::SchemaPath> result;
    result.reserve(named.size());
    for (auto const& path : named) {
        simfil::SchemaPath converted;
        converted.reserve(path.size());
        bool complete = true;
        for (auto const& segment : path) {
            if (segment.kind_ == simfil::SchemaPathSegment::Kind::ArrayElement)
                converted.push_back({segment.kind_, 0});
            else {
                auto field = dictionary().get(segment.field_);
                if (!field) {
                    complete = false;
                    break;
                }
                converted.push_back({segment.kind_, field});
            }
        }
        if (complete)
            result.push_back(std::move(converted));
    }
    return result;
}

}  // namespace mapget
