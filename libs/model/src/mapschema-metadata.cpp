#include "mapschema-impl.h"

namespace mapget
{

simfil::ArrayIndex MapSchema::Impl::newOwners(std::span<OwnerData const> sites)
{
    if (sites.empty())
        return 0;
    if (sites.size() > MaxEdges - ownerRows.size())
        throw std::length_error("Attribute owner row budget exceeded.");
    auto result = allocateMetadataArray(owners, std::max(size_t{2}, sites.size()));
    for (auto const& site : sites) {
        auto index = ownerRows.size();
        ownerRows.push_back(site);
        owners.push_back(result, uint32_t(index));
    }
    return result;
}

void MapSchema::Impl::bindProduced(size_t index, std::string_view feature, TypeId root)
{
    pool.requireMutable();
    requireType(root);
    pool.appendExtension(
        [&]
        {
            appendMetadata(
                [&]
                {
                    auto sid = intern(feature);
                    auto old = layerAt(index).produced;
                    auto entries = producedFeatures.range(old);
                    for (auto const& entry : entries)
                        if (entry.name == sid) {
                            if (entry.root != root)
                                throw std::invalid_argument("Conflicting produced feature root.");
                            return;
                        }
                    // Stage a replacement rather than mutating an array covered by an outer append
                    // guard.
                    std::vector<ProducedRootData> staged(entries.begin(), entries.end());
                    staged.push_back({sid, root});
                    auto handle = allocateMetadataArray(producedFeatures, staged.size(), true);
                    for (auto const& row : staged)
                        producedFeatures.push_back(handle, row);
                    layerAt(index).produced = handle;
                    if (old)
                        producedFeatures.truncate(old, 0);
                });
        });
}

LayerSchema::RelationDeclaration MapSchema::Impl::relationDeclaration(RelationData const& row) const
{
    LayerSchema::RelationDeclaration result;
    result.ownerType_ = name(row.owner);
    result.name_ = name(row.name);
    for (auto target : relationTargets.range(row.targets))
        result.targetTypes_.emplace_back(name(target));
    result.complete_ = row.flags & 1;
    result.locality_ = LayerSchema::RelationDeclaration::Locality(row.locality);
    return result;
}

void MapSchema::Impl::setRelations(
    size_t index,
    std::vector<LayerSchema::RelationDeclaration> declarations,
    bool known)
{
    pool.requireMutable();
    (void)layerAt(index);
    std::set<std::pair<std::string, std::string>> names;
    for (auto& declaration : declarations) {
        if (declaration.ownerType_.empty() || declaration.name_.empty() ||
            declaration.targetTypes_.empty() ||
            !names.emplace(declaration.ownerType_, declaration.name_).second ||
            unsigned(declaration.locality_) >
                unsigned(LayerSchema::RelationDeclaration::Locality::MayCrossContainer))
            throw std::invalid_argument("Invalid or duplicate forward relation declaration.");
        std::ranges::sort(declaration.targetTypes_);
        declaration.targetTypes_.erase(
            std::unique(declaration.targetTypes_.begin(), declaration.targetTypes_.end()),
            declaration.targetTypes_.end());
        if (declaration.targetTypes_.front().empty())
            throw std::invalid_argument("A relation target type cannot be empty.");
    }
    if (declarations.size() > MaxEdges - relationRows.size())
        throw std::length_error("Relation row budget exceeded.");
    auto old = layerAt(index).relations;
    auto oldIds = relationLists.range(old);
    std::vector<uint32_t> retired(oldIds.begin(), oldIds.end());
    size_t targets = 0;
    for (auto const& row : relationRows)
        if (row.name)
            targets += relationTargets.size(row.targets);
    for (auto id : retired)
        targets -= relationTargets.size(relationRows[id].targets);
    for (auto const& declaration : declarations) {
        if (targets > MaxEdges || declaration.targetTypes_.size() > MaxEdges - targets)
            throw std::length_error("Relation target edge budget exceeded.");
        targets += declaration.targetTypes_.size();
    }
    pool.appendExtension(
        [&]
        {
            appendMetadata(
                [&]
                {
                    auto handle = allocateMetadataArray(relationLists, declarations.size(), true);
                    for (auto const& declaration : declarations) {
                        RelationData row;
                        row.owner = intern(declaration.ownerType_);
                        row.name = intern(declaration.name_);
                        row.layer = uint16_t(index);
                        row.flags = declaration.complete_ ? 1 : 0;
                        row.locality = uint8_t(declaration.locality_);
                        row.targets = allocateMetadataArray(
                            relationTargets,
                            declaration.targetTypes_.size(),
                            true);
                        for (auto const& target : declaration.targetTypes_)
                            relationTargets.push_back(row.targets, intern(target));
                        auto id = relationRows.size();
                        relationRows.push_back(row);
                        relationLists.push_back(handle, uint32_t(id));
                    }
                    // All allocating work precedes publication. Retirement only empties known-valid
                    // storage.
                    auto& layer = layerAt(index);
                    layer.relations = handle;
                    layer.flags = RelationsDeclaredFlag | (known ? RelationsKnownFlag : 0);
                    for (auto id : retired) {
                        relationTargets.truncate(relationRows[id].targets, 0);
                        relationRows[id] = {};
                    }
                    if (old)
                        relationLists.truncate(old, 0);
                });
        });
}

void MapSchema::Impl::normalizeRelations()
{
    size_t work = 0;
    for (size_t index = 0; index < layers.size(); ++index) {
        auto const& layer = layers[index];
        if (relationsDeclared(layer))
            continue;
        auto produced = producedRoots(layer);
        auto hasInventory = [&](TypeId id)
        {
            if (!valid(id))
                return false;
            auto extra = annotation(data(id).annotations);
            auto meta = schema_detail::mapgetMetadata(extra);
            return meta && meta->contains("relationDeclarations") &&
                meta->value("relationDeclarationsKnown", true);
        };
        bool layerWide = valid(layer.root) && metaType(layer.root) != "Feature" &&
            simfil::SchemaType::kindNameId(pool.schemaType(layer.root).kind()) !=
                simfil::SchemaType::kindNameId(LayerSchema::FeatureKind) &&
            hasInventory(layer.root);
        bool known = layerWide ||
            (!produced.empty() && std::ranges::all_of(produced, hasInventory));
        std::vector<LayerSchema::RelationDeclaration> declarations;
        std::vector<TypeId> pending{layer.root};
        pending.insert(pending.end(), produced.begin(), produced.end());
        std::vector<bool> seen(types.size());
        auto append = [&](TypeId id)
        {
            if (++work > 64 * 1024 * 1024)
                throw std::length_error("Relation normalization work budget exceeded.");
            pending.push_back(id);
        };
        while (!pending.empty()) {
            auto id = pending.back();
            pending.pop_back();
            if (!valid(id) || seen[id])
                continue;
            seen[id] = true;
            auto extra = annotation(data(id).annotations);
            auto meta = schema_detail::mapgetMetadata(extra);
            if (meta && meta->contains("relationDeclarations")) {
                auto const& inputs = meta->at("relationDeclarations");
                if (!inputs.is_array())
                    throw std::invalid_argument("relationDeclarations must be an array.");
                for (auto const& input : inputs) {
                    LayerSchema::RelationDeclaration declaration{
                        input.at("ownerType").get<std::string>(),
                        input.at("name").get<std::string>(),
                        input.at("targetTypes").get<std::vector<std::string>>(),
                        input.value("complete", false)};
                    auto locality = input.value("locality", "unknown");
                    using Locality = LayerSchema::RelationDeclaration::Locality;
                    if (locality == "same-container")
                        declaration.locality_ = Locality::SameContainer;
                    else if (locality == "may-cross-container")
                        declaration.locality_ = Locality::MayCrossContainer;
                    else if (locality != "unknown")
                        throw std::invalid_argument("Invalid relation locality.");
                    if (!pool.layer(name(layer.name))->producesFeatureType(declaration.ownerType_))
                        continue;
                    auto previous = std::ranges::find_if(
                        declarations,
                        [&](auto const& item) {
                            return item.ownerType_ == declaration.ownerType_ &&
                                item.name_ == declaration.name_;
                        });
                    if (previous != declarations.end()) {
                        if (previous->targetTypes_ != declaration.targetTypes_ ||
                            previous->complete_ != declaration.complete_ ||
                            previous->locality_ != declaration.locality_)
                            throw std::invalid_argument(
                                "Conflicting forward relation declaration.");
                    }
                    else
                        declarations.push_back(std::move(declaration));
                }
            }
            auto type = pool.schemaType(id);
            type.forEachDirectField(
                [&](auto const& field)
                {
                    if (field.role() != simfil::SchemaType::FieldRole::Context)
                        for (auto child : field.types())
                            append(child);
                });
            if (type.baseType())
                append(type.baseType().id());
            for (auto child : type.elements())
                append(child);
            for (auto child : type.alternatives())
                append(child);
        }
        setRelations(index, std::move(declarations), known);
    }
}

}  // namespace mapget
