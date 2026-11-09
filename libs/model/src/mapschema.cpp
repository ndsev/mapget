#include "mapget/model/mapschema.h"
#include "mapschema-impl.h"

#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>
#include <picosha2.h>
#include "simfil/model/bitsery-traits.h"
#include "simfil/model/schema-node.h"

namespace mapget
{
using schema_detail::Composition;
using schema_detail::hasAffinity;
using schema_detail::Kind;
using schema_detail::matchesKind;
using simfil::ModelNodeAddress;
using simfil::StringId;
using simfil::TypeId;

#include "mapschema-composition.h"

MapSchema::MapSchema(MapFormat format)
    : SchemaPool(std::make_shared<StringPool>("schema")),
      schema_(std::make_unique<Impl>(*this, MapFormat::fromJson(format.toJson())))
{
}

MapSchema::~MapSchema() = default;

void MapSchema::requireMutable() const
{
    if (schema_->frozen || simfil::SchemaPool::finalized())
        throw std::logic_error("MapSchema is immutable after finalization.");
}

bool MapSchema::finalized() const
{
    return schema_->frozen;
}

MapFormat const& MapSchema::format() const
{
    return schema_->format;
}

std::string_view MapSchema::schemaId() const
{
    if (!finalized())
        throw std::logic_error("MapSchema identity is unavailable before finalization.");
    return simfil::SchemaPool::identity();
}

size_t MapSchema::typeCount() const
{
    return simfil::SchemaPool::typeCount();
}

LayerSchema::Ptr MapSchema::newLayer(std::string_view name, LayerType type)
{
    requireMutable();
    if (name.empty() || unsigned(type) > unsigned(LayerType::SourceData))
        throw std::invalid_argument("A schema layer requires a nonempty name and valid kind.");
    if (auto existing = layer(name)) {
        if (existing->layerType() != type)
            throw std::invalid_argument(
                "Schema layer kind conflicts with an existing declaration.");
        return existing;
    }
    if (schema_->layers.size() == UINT16_MAX)
        throw std::length_error("MapSchema layer count exceeds 16-bit namespace.");
    auto index = schema_->layers.size();
    LayerSchema::Ptr result;
    appendExtension(
        [&]
        {
            schema_->appendMetadata(
                [&]
                {
                    Impl::LayerData item;
                    item.name = schema_->intern(name);
                    item.type = uint8_t(type);
                    schema_->layerBuild.emplace_back();
                    schema_->layers.push_back(item);
                    result = LayerSchema::Ptr::make(
                        shared_from_this(),
                        ModelNodeAddress{Impl::Layers, index});
                });
        });
    return result;
}

LayerSchema::Ptr MapSchema::layer(std::string_view name) const
{
    auto sid = schema_->dictionary.get(name);
    if (!sid)
        return {};
    for (size_t i = 0; i < schema_->layers.size(); ++i)
        if (schema_->layers[i].name == sid)
            return LayerSchema::Ptr::make(shared_from_this(), ModelNodeAddress{Impl::Layers, i});
    return {};
}

std::vector<std::string> MapSchema::layerNames() const
{
    std::vector<std::string> names;
    for (auto const& layer : schema_->layers)
        names.emplace_back(schema_->name(layer.name));
    return names;
}

TypeId MapSchema::addSchema(Kind kind, std::string key, std::string metaType, std::string pointer)
{
    requireMutable();
    if (!key.empty() && findSchema(key))
        throw std::invalid_argument("Duplicate global schema key: " + key);
    // Legacy declarations still carry mapget semantics in metaType. Preserve
    // their structural affinity while assigning the canonical specialized name.
    if (kind == Kind::Object && metaType == "Feature")
        kind = simfil::SchemaType::makeKind(
            simfil::SchemaType::kindNameId(LayerSchema::FeatureKind),
            simfil::SchemaType::affinities(kind));
    else if (kind == Kind::Object && metaType == "Attribute")
        kind = simfil::SchemaType::makeKind(
            simfil::SchemaType::kindNameId(LayerSchema::AttributeKind),
            simfil::SchemaType::affinities(kind));
    auto id = simfil::SchemaPool::newType(kind).id();
    auto& row = schema_->data(id);
    row.key = schema_->text(key);
    row.pointer = schema_->text(pointer);
    row.metaType = schema_->text(metaType);
    if (!key.empty())
        registerSchemaKey(std::move(key), id);
    return id;
}

void MapSchema::registerSchemaKey(std::string key, TypeId id)
{
    requireMutable();
    schema_->requireType(id);
    auto found = schema_->buildAliases.find(key);
    if (found != schema_->buildAliases.end()) {
        if (found->second != id)
            throw std::invalid_argument("A global schema key has conflicting declarations.");
        return;
    }
    schema_->aliases.push_back({schema_->text(key), id, 0});
    try {
        schema_->buildAliases.emplace(std::move(key), id);
    }
    catch (...) {
        schema_->aliases.resize(schema_->aliases.size() - 1);
        throw;
    }
}

TypeId MapSchema::Impl::findAlias(size_t layerIndex, std::string_view key) const
{
    auto scope = aliasScope(layerIndex);
    if (!indexed) {
        auto const& cache = scope ? layerBuild.at(layerIndex).aliases : buildAliases;
        if (auto found = cache.find(key); found != cache.end())
            return found->second;
        if (!pool.simfil::SchemaPool::finalized())
            return 0;
        // A private staged pool can be generically frozen without derived indexes.
        TypeId result = 0;
        forEachAlias(
            layerIndex,
            [&](auto name, auto id)
            {
                if (name == key)
                    result = id;
            });
        return result;
    }
    auto found = std::lower_bound(
        aliases.begin(),
        aliases.end(),
        std::pair{scope, key},
        [&](auto const& alias, auto const& wanted)
        {
            return alias.layer < wanted.first ||
                (alias.layer == wanted.first && text(alias.key) < wanted.second);
        });
    return found != aliases.end() && found->layer == scope && text(found->key) == key ?
        found->id :
        0;
}

TypeId MapSchema::findSchema(std::string_view key) const
{
    return schema_->findAlias(Impl::GlobalLayer, key);
}

void MapSchema::setZserioType(TypeId id, std::string name)
{
    requireMutable();
    schema_->requireType(id);
    schema_->data(id).zserioType = schema_->text(name);
    if (!schemaType(id).sealed() && schemaType(id).typeName().empty())
        schemaType(id).setTypeName(name);
}

void MapSchema::setAttributeMetadata(
    TypeId id,
    LayerSchema::AttributePathOwner owner,
    std::string type,
    std::string zserio)
{
    requireMutable();
    schema_->requireType(id);
    if (!type.empty() || !zserio.empty())
        requireMutableType(id);
    appendExtension(
        [&]
        {
            schema_->appendMetadata(
                [&]
                {
                    auto old = schema_->data(id).owners;
                    auto row = schema_->prepareAttributeOwner(id, owner);
                    if (schema_->isAttributeType(id))
                        row.metaType = schema_->text("Attribute");
                    if (!type.empty())
                        row.attributeType = schema_->text(type);
                    if (!zserio.empty())
                        row.zserioType = schema_->text(zserio);
                    // Named ownership is independent of sealing a physical payload.
                    // Its generic typename is a producer declaration, not a use-site side effect.
                    schema_->data(id) = row;
                    if (old && old != row.owners)
                        schema_->owners.truncate(old, 0);
                });
        });
}

MapSchema::Impl::TypeData
MapSchema::Impl::prepareAttributeOwner(TypeId id, LayerSchema::AttributePathOwner const& owner)
{
    auto row = data(id);
    if (owner.featureType_.empty() || owner.attributeLayerName_.empty() ||
        owner.attributeName_.empty())
        return row;
    OwnerData site{
        intern(owner.featureType_),
        intern(owner.attributeLayerName_),
        intern(owner.attributeName_),
        owner.featureLayer_.empty() ? StringId{0} : intern(owner.featureLayer_)};
    auto sites = ownerSites(row.owners);
    if (std::ranges::any_of(
            sites,
            [&](auto const& item)
            {
                return item.feature == site.feature && item.layer == site.layer &&
                    item.name == site.name && item.featureLayer == site.featureLayer;
            }))
        return row;
    if (ownerRows.size() == MaxEdges)
        throw std::length_error("Attribute owner row budget exceeded.");
    auto ids = owners.range(row.owners);
    auto symbol = sites.empty() ? site.name : sites.front().name;
    row.attributeCode = site.name == symbol &&
            std::ranges::all_of(sites, [&](auto const& item) { return item.name == symbol; }) ?
        text(name(symbol)) :
        ModelNodeAddress{};
    auto handle = allocateMetadataArray(owners, std::max(size_t{2}, ids.size() + 1));
    for (auto value : ids)
        owners.push_back(handle, value);
    auto index = ownerRows.size();
    ownerRows.push_back(site);
    owners.push_back(handle, uint32_t(index));
    row.owners = handle;
    return row;
}

void MapSchema::Impl::bindAttributeOwner(TypeId id, LayerSchema::AttributePathOwner const& owner)
{
    pool.requireMutable();
    requireType(id);
    if (owner.featureType_.empty() || owner.attributeLayerName_.empty() ||
        owner.attributeName_.empty())
        return;
    pool.appendExtension(
        [&]
        {
            appendMetadata(
                [&]
                {
                    auto old = data(id).owners;
                    auto staged = prepareAttributeOwner(id, owner);
                    // No allocations after publication; a failed append leaves the old use sites
                    // intact.
                    data(id) = staged;
                    if (old && old != staged.owners)
                        owners.truncate(old, 0);
                });
        });
}

TypeId MapSchema::Impl::import(
    MapSchema const& source,
    size_t layerIndex,
    TypeId root,
    std::span<TypeId const> reuse)
{
    pool.requireMutable();
    auto const& input = *source.schema_;
    if (input.layers.size() > 1 || !input.valid(root))
        throw std::invalid_argument("Schema import requires one staged layer and a valid root.");
    if (layerIndex != GlobalLayer)
        (void)layerAt(layerIndex);
    for (auto identity : source.featureIdentities()) {
        if (auto existing = pool.getTypeInfo(identity.name(), false);
            existing && !existing.sameIdentity(identity))
            throw std::invalid_argument(
                "Conflicting map-wide feature identity: " + std::string(identity.name()));
    }
    // Local spellings take precedence inside the one staged JSON boundary.
    std::map<std::string, TypeId, std::less<>> sourceAliases;
    for (auto const& alias : input.aliases)
        if (!alias.layer)
            sourceAliases[std::string(input.text(alias.key))] = alias.id;
    for (auto const& alias : input.aliases)
        if (alias.layer)
            sourceAliases[std::string(input.text(alias.key))] = alias.id;
    auto remap = pool.importGraphWithExtension(
        source,
        reuse,
        [&](std::span<TypeId const> mapping)
        {
            appendMetadata(
                [&]
                {
                    auto mapped = [&](TypeId id)
                    {
                        if (!id)
                            return TypeId{0};
                        if (id >= mapping.size() || !mapping[id])
                            throw std::invalid_argument(
                                "Dangling type reference in imported metadata.");
                        return mapping[id];
                    };
                    auto const& targetAliases = layerIndex == GlobalLayer ?
                        buildAliases :
                        layerBuild.at(layerIndex).aliases;
                    for (auto const& [key, id] : sourceAliases)
                        if (auto existing = targetAliases.find(key);
                            existing != targetAliases.end() && existing->second != mapped(id))
                            throw std::invalid_argument(
                                "Conflicting imported schema alias: " + key);
                    for (size_t n = 1; n < mapping.size(); ++n) {
                        if (!reuse.empty() && reuse[n])
                            continue;
                        auto id = TypeId(n);
                        auto const& original = input.data(id);
                        if (original.flags & QueryRootFlag)
                            throw std::invalid_argument(
                                "Staged imports must not contain derived query roots.");
                        TypeData row{};
                        row.flags = original.flags;
                        row.key = text(input.text(original.key));
                        row.pointer = text(input.text(original.pointer));
                        row.metaType = text(input.text(original.metaType));
                        row.zserioType = text(input.text(original.zserioType));
                        row.attributeType = text(input.text(original.attributeType));
                        row.attributeCode = text(input.text(original.attributeCode));
                        auto sites = input.ownerSites(original.owners);
                        std::vector<OwnerData> copiedSites;
                        for (auto const& site : sites) {
                            auto layer = layerIndex == GlobalLayer ?
                                (site.featureLayer ?
                                     input.name(site.featureLayer) :
                                     std::string_view{}) :
                                name(layerAt(layerIndex).name);
                            copiedSites.push_back(
                                {intern(input.name(site.feature)),
                                 intern(input.name(site.layer)),
                                 intern(input.name(site.name)),
                                 layer.empty() ? StringId{0} : intern(layer)});
                        }
                        row.owners = newOwners(copiedSites);
                        auto extra = input.annotation(original.annotations);
                        schema_detail::forEachValidationSchema(
                            extra,
                            [&](auto& constraint, auto const&)
                            {
                                auto reference = constraint.at("$ref").template get<std::string>();
                                constexpr std::string_view prefix = "#/definitions/";
                                if (!reference.starts_with(prefix))
                                    throw std::invalid_argument(
                                        "Unresolved schema validation reference.");
                                auto digits = std::string_view(reference).substr(prefix.size());
                                uint32_t value = 0;
                                auto [end, error] = std::from_chars(
                                    digits.data(),
                                    digits.data() + digits.size(),
                                    value);
                                if (error != std::errc{} || end != digits.data() + digits.size() ||
                                    value > simfil::MaxTypeId)
                                    throw std::invalid_argument(
                                        "Invalid schema validation reference.");
                                constraint["$ref"] = "#/definitions/" +
                                    std::to_string(mapped(TypeId(value)));
                            });
                        row.annotations = annotation(extra);
                        data(mapped(id)) = row;
                    }
                    for (auto identity : source.featureIdentities())
                        pool.addFeatureType(identity);
                    if (layerIndex != GlobalLayer && !input.layers.empty()) {
                        for (auto const& produced : input.featureRoots(input.layers[0]))
                            bindProduced(
                                layerIndex,
                                input.name(produced.name),
                                mapped(produced.root));
                    }

                    for (auto const& [key, id] : sourceAliases) {
                        if (layerIndex == GlobalLayer)
                            pool.registerSchemaKey(key, mapped(id));
                        else
                            pool.layer(name(layerAt(layerIndex).name))
                                ->registerSchemaKey(key, mapped(id));
                    }
                    // Recreate construction policy only after every imported edge and
                    // declaration is available, before exposing mutable imported views.
                    for (size_t n = 1; n < mapping.size(); ++n)
                        if (reuse.empty() || !reuse[n])
                            protectNativeType(mapping[n]);
                });
        });
    return remap[root];
}

void MapSchema::Impl::appendMetadata(std::function<void()> const& extension)
{
    auto typeCount = types.size();
    auto layerCount = layers.size();
    auto layerBuildCount = layerBuild.size();
    auto ownerRowCount = ownerRows.size();
    auto relationRowCount = relationRows.size();
    auto producedCheckpoint = producedFeatures.checkpointAppends();
    auto relationListCheckpoint = relationLists.checkpointAppends();
    auto relationTargetCheckpoint = relationTargets.checkpointAppends();
    auto aliasCount = aliases.size();
    auto identityCount = identities.size();
    auto compositionCheckpoint = identityCompositions.checkpointAppends();
    auto partCheckpoint = identityParts.checkpointAppends();
    auto ownerCheckpoint = owners.checkpointAppends();
    auto textStart = appendedTextKeys.size();
    auto policyStart = appendedPolicyTypes.size();
    ++metadataAppendDepth;
    try {
        extension();
    }
    catch (...) {
        // A narrow generic mutation can roll back its new value storage before
        // this derived scope unwinds. Cache-owned strings stay valid either way.
        for (size_t n = aliasCount; n < aliases.size(); ++n) {
            auto const& alias = aliases[n];
            auto key = std::ranges::find_if(
                textIndex,
                [&](auto const& item) { return item.second == alias.key; });
            auto& cache = alias.layer ? layerBuild.at(alias.layer - 1).aliases : buildAliases;
            if (key != textIndex.end())
                cache.erase(key->first);
        }
        aliases.resize(aliasCount);
        types.resize(typeCount);
        layers.resize(layerCount);
        layerBuild.resize(layerBuildCount);
        ownerRows.resize(ownerRowCount);
        relationRows.resize(relationRowCount);
        producedFeatures.rollbackAppends(producedCheckpoint);
        relationLists.rollbackAppends(relationListCheckpoint);
        relationTargets.rollbackAppends(relationTargetCheckpoint);
        identities.resize(identityCount);
        identityCompositions.rollbackAppends(compositionCheckpoint);
        identityParts.rollbackAppends(partCheckpoint);
        owners.rollbackAppends(ownerCheckpoint);
        for (size_t n = textStart; n < appendedTextKeys.size(); ++n)
            textIndex.erase(appendedTextKeys[n]);
        appendedTextKeys.resize(textStart);
        for (size_t n = policyStart; n < appendedPolicyTypes.size(); ++n)
            reservedFields.erase(appendedPolicyTypes[n]);
        appendedPolicyTypes.resize(policyStart);
        --metadataAppendDepth;
        throw;
    }
    if (!--metadataAppendDepth) {
        appendedTextKeys.clear();
        appendedPolicyTypes.clear();
    }
}

LayerSchema::LayerSchema(simfil::detail::mp_key key) : ModelNodeBase(key) {}

LayerSchema::LayerSchema(
    simfil::ModelConstPtr owner,
    ModelNodeAddress address,
    simfil::detail::mp_key key)
    : ModelNodeBase(std::move(owner), address, key)
{
}

std::shared_ptr<MapSchema const> LayerSchema::mapSchema() const
{
    return std::static_pointer_cast<MapSchema const>(model_);
}

MapSchema& LayerSchema::mutableOwner()
{
    auto& owner = *const_cast<MapSchema*>(mapSchema().get());
    owner.requireMutable();
    return owner;
}

std::string_view LayerSchema::name() const
{
    auto const& graph = *mapSchema()->schema_;
    return graph.name(graph.layerAt(addr_.index()).name);
}

LayerType LayerSchema::layerType() const
{
    return LayerType(mapSchema()->schema_->layerAt(addr_.index()).type);
}

void LayerSchema::setRootSchema(TypeId root)
{
    auto& owner = mutableOwner();
    if (root)
        owner.schema_->requireType(root);
    owner.schema_->layerAt(addr_.index()).root = root;
}

TypeId LayerSchema::rootSchema() const
{
    return mapSchema()->schema_->layerAt(addr_.index()).root;
}

size_t MapSchema::identityCount() const
{
    return schema_->identities.size();
}

FeatureIdentity MapSchema::getTypeInfo(std::string_view name, bool throwIfMissing) const
{
    auto sid = schema_->dictionary.get(name);
    if (sid && schema_->identityIndex.size() == identityCount()) {
        auto found = std::lower_bound(
            schema_->identityIndex.begin(),
            schema_->identityIndex.end(),
            sid,
            [&](auto index, auto wanted) { return schema_->identities[index].name < wanted; });
        if (found != schema_->identityIndex.end() && schema_->identities[*found].name == sid)
            return {this, *found};
    }
    else if (sid) {
        for (size_t i = 0; i < identityCount(); ++i)
            if (schema_->identities[i].name == sid)
                return {this, i};
    }
    if (throwIfMissing)
        throw std::invalid_argument(
            "Unknown feature identity '" + std::string(name) + "' in MapSchema.");
    return {};
}

void MapSchema::addFeatureType(FeatureTypeInfo info)
{
    requireMutable();
    info.validate();
    if (auto existing = getTypeInfo(info.name_, false)) {
        if (!existing.sameIdentity(info))
            throw std::invalid_argument("Conflicting map-wide feature identity: " + info.name_);
        return;
    }
    if (identityCount() == UINT16_MAX)
        throw std::length_error("MapSchema feature identity capacity exceeded.");
    appendExtension(
        [&]
        {
            schema_->appendMetadata(
                [&]
                {
                    schema_->appendIdentity(
                        info.name_,
                        info.uniqueIdCompositions_,
                        [](auto const& parts) -> auto const& { return parts; });
                });
        });
}

void MapSchema::addFeatureType(FeatureIdentity identity)
{
    requireMutable();
    if (!identity)
        throw std::invalid_argument("Cannot register an absent feature identity.");
    if (auto existing = getTypeInfo(identity.name(), false)) {
        if (!existing.sameIdentity(identity))
            throw std::invalid_argument(
                "Conflicting map-wide feature identity: " + std::string(identity.name()));
        return;
    }
    if (identityCount() == UINT16_MAX)
        throw std::length_error("MapSchema feature identity capacity exceeded.");
    appendExtension(
        [&]
        {
            schema_->appendMetadata(
                [&]
                {
                    schema_->appendIdentity(
                        identity.name(),
                        identity.compositions(),
                        [](auto composition) { return composition.parts(); });
                });
        });
}

FeatureIdentity LayerSchema::getTypeInfo(std::string_view name, bool throwIfMissing) const
{
    return mapSchema()->getTypeInfo(name, throwIfMissing);
}

bool LayerSchema::producesFeatureType(std::string_view name) const
{
    auto const& graph = *mapSchema()->schema_;
    auto const& layer = graph.layerAt(addr_.index());
    auto roots = graph.featureRoots(layer);
    if (!roots.empty()) {
        auto sid = graph.dictionary.get(name);
        return sid &&
            std::ranges::any_of(roots, [sid](auto const& row) { return row.name == sid; });
    }
    auto types = featureTypes();
    return std::ranges::find(types, name) != types.end();
}

std::vector<std::string> LayerSchema::featureTypes() const
{
    auto const& graph = *mapSchema()->schema_;
    auto const& layer = graph.layerAt(addr_.index());
    std::vector<std::string> result;
    auto entries = graph.featureRoots(layer);
    for (auto const& row : entries)
        result.emplace_back(graph.name(row.name));
    if (entries.empty()) {
        auto roots = graph.producedRoots(layer);
        graph.forEachAlias(
            addr_.index(),
            [&](std::string_view key, TypeId id)
            {
                constexpr std::string_view prefix = "Feature:";
                if (key.starts_with(prefix) && key.size() > prefix.size() &&
                    std::ranges::find(roots, id) != roots.end())
                    result.emplace_back(key.substr(prefix.size()));
            });
    }
    std::ranges::sort(result);
    return result;
}

void LayerSchema::addFeatureType(FeatureTypeInfo info, TypeId root)
{
    auto name = info.name_;
    auto& owner = mutableOwner();
    if (root)
        owner.schema_->requireType(root);
    owner.addFeatureType(std::move(info));
    addFeatureType(std::string_view(name), root);
}

void LayerSchema::addFeatureType(std::string_view name, TypeId root)
{
    auto& owner = mutableOwner();
    (void)owner.getTypeInfo(name);
    if (!root)
        root = schemaId(featureKey(name));
    if (root)
        owner.schema_->requireType(root);
    auto entries = owner.schema_->featureRoots(owner.schema_->layerAt(addr_.index()));
    for (auto const& row : entries)
        if (owner.schema_->name(row.name) == name) {
            if (root && row.root != root)
                throw std::invalid_argument(
                    "Conflicting produced feature root: " + std::string(name));
            return;
        }
    if (!root) {
        root = addSchema(FeatureKind, featureKey(name), "Feature");
        owner.schemaType(root).asObject().setOpen(true);
        owner.schemaType(root).setTypeName(name);
    }
    registerSchemaKey(featureKey(name), root);
    owner.schema_->bindProduced(addr_.index(), name, root);
}

TypeId LayerSchema::addSchema(Kind kind, std::string key, std::string metaType, std::string pointer)
{
    auto& owner = mutableOwner();
    auto id = owner.addSchema(kind, {}, std::move(metaType), std::move(pointer));
    owner.schema_->data(id).key = owner.schema_->text(key);
    if (!key.empty())
        registerSchemaKey(std::move(key), id);
    return id;
}

void LayerSchema::registerSchemaKey(std::string key, TypeId id)
{
    auto& owner = mutableOwner();
    owner.schema_->requireType(id);
    auto& aliases = owner.schema_->layerBuild.at(addr_.index()).aliases;
    auto it = aliases.find(key);
    if (it != aliases.end()) {
        if (it->second != id)
            throw std::invalid_argument("Conflicting layer-local schema key: " + it->first);
        return;
    }
    owner.schema_->aliases.push_back({owner.schema_->text(key), id, uint16_t(addr_.index() + 1)});
    try {
        aliases.emplace(std::move(key), id);
    }
    catch (...) {
        owner.schema_->aliases.resize(owner.schema_->aliases.size() - 1);
        throw;
    }
}

TypeId LayerSchema::schemaId(std::string_view key) const
{
    auto owner = mapSchema();
    if (auto id = owner->schema_->findAlias(addr_.index(), key))
        return id;
    return owner->findSchema(key);
}

void LayerSchema::finalize()
{
    if (!mapSchema()->finalized())
        mutableOwner().finalize();
}

std::vector<TypeId> MapSchema::Impl::producedRoots(LayerData const& layer) const
{
    std::vector<TypeId> result;
    if (!featureRoots(layer).empty()) {
        for (auto const& [_, root] : featureRoots(layer))
            result.push_back(root);
        return result;
    }
    // Schema-only legacy inputs may describe produced roots without ID contracts.
    std::vector<TypeId> pending{layer.root};
    std::vector<bool> seen(pool.typeCount() + 1);
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (!valid(id) || seen[id])
            continue;
        seen[id] = true;
        auto type = pool.schemaType(id);
        if (metaType(id) == "Feature" ||
            simfil::SchemaType::kindNameId(type.kind()) ==
                simfil::SchemaType::kindNameId(LayerSchema::FeatureKind))
        {
            auto extra = annotation(data(id).annotations);
            auto metadata = schema_detail::mapgetMetadata(extra);
            if (!metadata || metadata->value("produced", true))
                result.push_back(id);
            continue;
        }
        auto alternatives = type.alternatives();
        pending.insert(pending.end(), alternatives.begin(), alternatives.end());
        auto elements = type.elements();
        pending.insert(pending.end(), elements.begin(), elements.end());
    }
    std::ranges::sort(result);
    return result;
}

void MapSchema::Impl::prepareQueryRoots()
{
    std::set<std::pair<TypeId, TypeId>> contexts;
    std::map<TypeId, std::pair<std::string, std::string>> featureNames;
    for (auto const& layer : layers) {
        for (auto const& [name, root] : featureRoots(layer))
            featureNames.emplace(
                root,
                std::pair{std::string(this->name(layer.name)), std::string(this->name(name))});
        for (auto feature : producedRoots(layer)) {
            auto source = pool.schemaType(feature);
            auto extra = annotation(data(feature).annotations);
            auto name =
                schema_detail::metadataString(schema_detail::mapgetMetadata(extra), "featureType");
            featureNames.try_emplace(
                feature,
                std::string(this->name(layer.name)),
                name.empty() ? std::string(source.typeName()) : std::move(name));
            std::vector<TypeId> pending{feature};
            std::vector<bool> seen(pool.typeCount() + 1);
            while (!pending.empty()) {
                auto id = pending.back();
                pending.pop_back();
                if (!valid(id) || seen[id])
                    continue;
                seen[id] = true;
                if (text(data(id).metaType) == "Attribute")
                    contexts.emplace(feature, id);
                auto type = pool.schemaType(id);
                type.forEachDirectField(
                    [&](simfil::SchemaField const& field)
                    {
                        auto children = field.types();
                        pending.insert(pending.end(), children.begin(), children.end());
                    });
                auto elements = type.elements();
                pending.insert(pending.end(), elements.begin(), elements.end());
                auto alternatives = type.alternatives();
                pending.insert(pending.end(), alternatives.begin(), alternatives.end());
            }
        }
    }
    for (auto const& query : queryRoots)
        contexts.erase({query.feature, query.attribute});
    if (contexts.empty())
        return;
    auto string = pool.primitive(Kind::String);
    auto integer = pool.primitive(Kind::Int);
    auto boolean = pool.primitive(Kind::Bool);
    for (auto [feature, attribute] : contexts) {
        auto source = pool.schemaType(attribute);
        auto id = pool.addSchema(LayerSchema::AttributeKind, {}, "Attribute");
        auto row = data(attribute);
        row.flags = QueryRootFlag;
        row.key = {};
        row.pointer = {};
        row.owners = 0;
        auto const& [featureLayer, featureName] = featureNames.at(feature);
        auto sourceSites = ownerSites(data(attribute).owners);
        std::vector<OwnerData> sites;
        for (auto const& site : sourceSites)
            if ((!site.featureLayer || name(site.featureLayer) == featureLayer) &&
                name(site.feature) == featureName)
                sites.push_back(site);
        if (!sites.empty()) {
            row.owners = newOwners(sites);
            auto code = sites.front().name;
            row.attributeCode =
                std::ranges::all_of(sites, [&](auto const& site) { return site.name == code; }) ?
                text(name(code)) :
                ModelNodeAddress{};
        }
        else
            row.attributeCode = {};
        data(id) = row;
        pool.schemaType(id).setTypeName(source.typeName());
        pool.simfil::SchemaPool::setBaseSchema(id, attribute);
        pool.simfil::SchemaPool::setComposition(id, source.composition());
        // Read each source edge by index; appending in the same arena invalidates ranges.
        auto count = source.alternatives().size();
        for (size_t n = 0; n < count; ++n) {
            auto child = source.alternatives()[n];
            pool.simfil::SchemaPool::addAlternative(id, child);
        }
        auto overlay = [&](std::string_view field, TypeId child)
        {
            pool.simfil::SchemaPool::addFieldSchema(id, field, child);
            pool.simfil::SchemaPool::setFieldRequired(id, field, true);
            pool.simfil::SchemaPool::setFieldRole(
                id,
                field,
                simfil::SchemaType::FieldRole::Context);
        };
        overlay("$name", string);
        overlay("$layer", string);
        overlay("$feature", feature);
        overlay("$attributeIndex", integer);
        overlay("$validityIndex", integer);
        overlay("$validityCount", integer);
        overlay("$hasValidity", boolean);
        queryRoots.push_back({feature, attribute, id});
    }
    std::sort(
        queryRoots.begin(),
        queryRoots.end(),
        [](auto const& left, auto const& right) {
            return std::pair{left.feature, left.attribute} <
                std::pair{right.feature, right.attribute};
        });
}

void MapSchema::Impl::compile()
{
    // Generic typed factories need no map-specific metadata until publication.
    types.resize(pool.simfil::SchemaPool::typeCount() + 1);
    // Legacy imports and explicit Feature declarations have the same lookup
    // contract as newFeature(). The alias never becomes an enumerable member.
    for (size_t id = 1; id < types.size(); ++id) {
        auto type = pool.schemaType(TypeId(id));
        if (simfil::SchemaType::kindNameId(type.kind()) !=
                simfil::SchemaType::kindNameId(LayerSchema::FeatureKind) ||
            !type.field("properties"))
            continue;
        // An explicitly declared member takes precedence, matching Feature::get().
        if (!type.field("attributes"))
            pool.simfil::SchemaPool::addFieldAlias(TypeId(id), "attributes", "properties");
    }
    for (auto const& layer : layers) {
        auto entries = producedFeatures.range(layer.produced);
        std::vector<ProducedRootData> sorted(entries.begin(), entries.end());
        std::ranges::sort(
            sorted,
            [&](auto const& a, auto const& b) { return name(a.name) < name(b.name); });
        for (size_t i = 0; i < sorted.size(); ++i)
            entries[i] = sorted[i];
    }
    for (auto& layer : layers) {
        if (layer.root)
            continue;
        std::vector<TypeId> roots;
        for (auto const& [_, root] : featureRoots(layer))
            roots.push_back(root);
        if (roots.size() == 1)
            layer.root = roots.front();
        else {
            layer.root = pool.addSchema(roots.empty() ? Kind::Unknown : Kind::Union);
            if (!roots.empty()) {
                pool.setComposition(layer.root, Composition::AnyOf);
                for (auto root : roots)
                    pool.addAlternative(layer.root, root);
            }
        }
    }
    normalizeRelations();
    prepareQueryRoots();
    for (size_t i = 0; i < descriptorKeys.size(); ++i)
        descriptorKeys[i] = intern(std::array<std::string_view, 21>{
            "name",
            "layerType",
            "root",
            "featureTypes",
            "id",
            "kind",
            "typename",
            "fields",
            "items",
            "alternatives",
            "enum",
            "open",
            "schemaKey",
            "jsonPointer",
            "metaType",
            "zserioType",
            "attributeType",
            "attributeTypeCode",
            "attributeOwners",
            "featureType",
            "featureLayer"}[i]);
    pool.finalizeSchema();
    buildAliases.clear();
    std::vector<LayerBuildData>().swap(layerBuild);
    reservedFields.clear();
    std::vector<TypeId>().swap(appendedPolicyTypes);
    arrayTypes.clear();
    nullableTypes.clear();
    textIndex.clear();
    textIndex.rehash(0);
    std::vector<std::string>().swap(appendedTextKeys);
}

/**
 * Prepare alias, identity and relation lookups before publishing a constructed
 * or decoded schema. Canonical alias rows are sorted in place; conflicting
 * aliases, inconsistent produced roots and missing descriptor vocabulary fail
 * publication instead of leaving callers with different lookup results.
 */
void MapSchema::Impl::index()
{
    for (size_t i = 0; i < descriptorKeys.size(); ++i) {
        descriptorKeys[i] = pool.strings()->get(std::array<std::string_view, 21>{
            "name",
            "layerType",
            "root",
            "featureTypes",
            "id",
            "kind",
            "typename",
            "fields",
            "items",
            "alternatives",
            "enum",
            "open",
            "schemaKey",
            "jsonPointer",
            "metaType",
            "zserioType",
            "attributeType",
            "attributeTypeCode",
            "attributeOwners",
            "featureType",
            "featureLayer"}[i]);
        if (!descriptorKeys[i])
            throw std::invalid_argument("Missing schema descriptor vocabulary.");
    }
    // Sort the canonical records themselves; the runtime lookup needs no mirror.
    std::sort(
        aliases.begin(),
        aliases.end(),
        [&](auto const& left, auto const& right)
        {
            return left.layer < right.layer ||
                (left.layer == right.layer && text(left.key) < text(right.key));
        });
    size_t written = 0;
    for (size_t n = 0; n < aliases.size(); ++n) {
        auto value = aliases[n];
        if (written && aliases[written - 1].layer == value.layer &&
            text(aliases[written - 1].key) == text(value.key))
        {
            if (aliases[written - 1].id != value.id)
                throw std::invalid_argument("Conflicting stored schema aliases.");
            continue;
        }
        aliases[written++] = value;
    }
    aliases.resize(written);
    identityIndex.resize(identities.size());
    for (size_t i = 0; i < identities.size(); ++i)
        identityIndex[i] = uint16_t(i);
    std::sort(
        identityIndex.begin(),
        identityIndex.end(),
        [&](auto a, auto b) { return identities[a].name < identities[b].name; });
    indexed = true;
    relationTargetIndex.clear();
    for (size_t index = 0; index < layers.size(); ++index) {
        // Runtime feature lookup uses the produced name/root pairs. Check the
        // corresponding local aliases once before publishing, now that both
        // native and decoded graphs have the sorted alias lookup available.
        for (auto const& produced : featureRoots(layers[index])) {
            if (findAlias(index, LayerSchema::featureKey(name(produced.name))) != produced.root)
                throw std::invalid_argument(
                    "Produced feature root disagrees with its schema alias.");
        }
        auto ids = relationIds(layers[index]);
        for (auto id : ids)
            for (auto target : relationTargets.range(relationRows[id].targets))
                relationTargetIndex.push_back({target, uint16_t(index), id});
    }
    std::sort(
        relationTargetIndex.begin(),
        relationTargetIndex.end(),
        [](auto const& a, auto const& b) {
            return std::tuple{a.target, a.layer, a.relation} <
                std::tuple{b.target, b.layer, b.relation};
        });
}

void MapSchema::Impl::validate() const
{
    (void)MapFormat::fromJson(format.toJson());
    std::set<std::string_view> constituentIds;
    for (auto const& [id, format] : constituents) {
        constexpr std::string_view prefix = "mapget-schema:sha256:";
        if (!id.starts_with(prefix) || id.size() != prefix.size() + 64 ||
            id.find_first_not_of("0123456789abcdef", prefix.size()) != std::string::npos ||
            !constituentIds.insert(id).second)
            throw std::invalid_argument("Invalid or duplicate constituent schema identity.");
        (void)MapFormat::fromJson(format.toJson());
    }
    if (types.size() != pool.simfil::SchemaPool::typeCount() + 1)
        throw std::invalid_argument("Map metadata and generic type namespaces disagree.");
    auto type = [&](TypeId id)
    {
        if (!id || id >= types.size() || !pool.schemaType(id))
            throw std::invalid_argument("Dangling MapSchema type reference.");
    };
    std::set<simfil::ArrayIndex> ownerArrays;
    size_t ownerCount = 0;
    std::vector<bool> usedOwners(ownerRows.size());
    for (size_t id = 1; id < types.size(); ++id) {
        auto const& d = types[id];
        if (d.flags & ~8u)
            throw std::invalid_argument("Invalid map-type flags.");
        for (auto address :
             {d.key, d.pointer, d.metaType, d.zserioType, d.attributeType, d.attributeCode})
            text(address);
        if (!owners.valid(d.owners) || !owners.valid(0) || owners.size(0) ||
            (d.owners && !ownerArrays.insert(d.owners).second))
            throw std::invalid_argument("Invalid or multiply-owned attribute owner array.");
        auto count = owners.size(d.owners);
        if (count > MaxEdges - ownerCount)
            throw std::length_error("Attribute owner storage exceeds its edge budget.");
        ownerCount += count;
        if (d.owners && !count)
            throw std::invalid_argument("Empty owner lists must use the reserved head.");
        for (auto index : owners.range(d.owners)) {
            if (index >= ownerRows.size() || usedOwners[index])
                throw std::invalid_argument("Invalid or multiply-owned attribute owner row.");
            usedOwners[index] = true;
            auto const& owner = ownerRows[index];
            if (!owner.feature || !owner.layer || !owner.name)
                throw std::invalid_argument("Incomplete attribute owner row.");
            name(owner.feature);
            name(owner.layer);
            name(owner.name);
            if (owner.featureLayer)
                name(owner.featureLayer);
        }
        if (d.annotations)
            (void)annotation(d.annotations);
        if (auto base = pool.schemaType(TypeId(id)).baseType().id()) {
            if ((types[base].flags & QueryRootFlag) ||
                ((d.flags & QueryRootFlag) &&
                 (base >= id || metaType(base) != "Attribute" || !isAttributeType(base))))
                throw std::invalid_argument(
                    "Query overlays must refer directly to a non-overlay attribute payload.");
        }
    }
    requireOwnedArrays(owners, ownerArrays);
    for (size_t index = 0; index < ownerRows.size(); ++index) {
        auto const& row = ownerRows[index];
        if (!usedOwners[index] && (row.feature || row.layer || row.name || row.featureLayer))
            throw std::invalid_argument("Unreferenced attribute owner row payload.");
    }
    for (auto const& alias : aliases) {
        type(alias.id);
        text(alias.key);
        if (alias.layer > layers.size())
            throw std::invalid_argument("Unknown schema alias layer.");
    }
    std::set<TypeId> overlays;
    std::optional<std::pair<TypeId, TypeId>> previousContext;
    for (auto const& query : queryRoots) {
        auto context = std::pair{query.feature, query.attribute};
        type(query.feature);
        type(query.attribute);
        type(query.root);
        if (query.reserved || (previousContext && *previousContext >= context) ||
            pool.schemaType(query.root).baseType().id() != query.attribute ||
            !overlays.insert(query.root).second ||
            simfil::SchemaType::kindNameId(pool.schemaType(query.feature).kind()) !=
                simfil::SchemaType::kindNameId(LayerSchema::FeatureKind))
            throw std::invalid_argument("Invalid query overlay binding.");
        previousContext = context;
    }
    for (size_t id = 1; id < types.size(); ++id)
        if (bool(types[id].flags & QueryRootFlag) != overlays.contains(TypeId(id)))
            throw std::invalid_argument(
                "Query overlay bindings must cover every overlay exactly once.");
    if (!identityCompositions.valid(0) || identityCompositions.size(0) || !identityParts.valid(0) ||
        identityParts.size(0))
        throw std::invalid_argument("Invalid empty identity arena head.");
    std::set<simfil::ArrayIndex> compositionArrays, partArrays;
    size_t identityEdges = 0;
    std::set<std::string_view> identityNames;
    for (size_t index = 0; index < identities.size(); ++index) {
        auto const& record = identities[index];
        auto identityName = name(record.name);
        validateIdentifierName("feature type name", identityName, ".");
        if (!record.compositions || !identityCompositions.valid(record.compositions) ||
            !compositionArrays.insert(record.compositions).second)
            throw std::invalid_argument("Invalid or multiply-owned identity composition array.");
        auto compositions = identityCompositions.range(record.compositions);
        if (identityName.empty() || record.reserved || !identityNames.insert(identityName).second ||
            compositions.empty() || compositions.size() > 256)
            throw std::invalid_argument("Invalid map-wide feature identity.");
        if (compositions.size() > MaxEdges - identityEdges)
            throw std::length_error("Feature identity storage exceeds its edge budget.");
        identityEdges += compositions.size();
        for (auto handle : compositions) {
            // Only the reserved empty head may be shared across immutable definitions.
            if (!identityParts.valid(handle) || (handle && !partArrays.insert(handle).second))
                throw std::invalid_argument("Invalid or multiply-owned identity part array.");
            auto parts = identityParts.range(handle);
            if (parts.size() > MaxEdges - identityEdges)
                throw std::length_error("Feature identity storage exceeds its edge budget.");
            identityEdges += parts.size();
            if ((handle && parts.empty()) || parts.size() > 16)
                throw std::invalid_argument(
                    "Feature ID parts require the canonical empty head or 1..16 parts.");
            std::set<std::string_view> labels;
            for (auto const& part : parts) {
                auto label = name(part.name);
                validateIdentifierName("id-part label", label, ".");
                if (label.empty() || !labels.insert(label).second || part.flags & ~uint8_t{3} ||
                    part.datatype > unsigned(IdPartDataType::STR))
                    throw std::invalid_argument("Invalid or duplicate feature ID part.");
                (void)this->text(part.description);
            }
        }
    }
    if (compositionArrays.size() !=
            identityCompositions.size() - 1 + identityCompositions.singleton_handle_count() ||
        partArrays.size() != identityParts.size() - 1 + identityParts.singleton_handle_count())
        throw std::invalid_argument("Unreferenced identity arena storage.");
    std::set<StringId> layerNames;
    std::set<simfil::ArrayIndex> producedArrays, relationArrays, targetArrays;
    std::vector<bool> usedRelations(relationRows.size());
    size_t relationEdges = 0;
    for (size_t index = 0; index < layers.size(); ++index) {
        auto const& layer = layers[index];
        if (!layer.name || name(layer.name).empty() || !layerNames.insert(layer.name).second ||
            layer.type > unsigned(LayerType::SourceData) || layer.reserved ||
            layer.flags & ~(RelationsKnownFlag | RelationsDeclaredFlag))
            throw std::invalid_argument("Invalid or duplicate schema layer.");
        if (layer.root)
            type(layer.root);
        if (!producedFeatures.valid(layer.produced) ||
            (layer.produced && !producedArrays.insert(layer.produced).second) ||
            !relationLists.valid(layer.relations) ||
            (layer.relations && !relationArrays.insert(layer.relations).second))
            throw std::invalid_argument("Invalid or multiply-owned layer memberships.");
        std::set<StringId> producedNames;
        std::string_view previous;
        for (auto const& produced : featureRoots(layer)) {
            auto spelling = name(produced.name);
            if (!identityNames.contains(spelling) || !producedNames.insert(produced.name).second ||
                (!previous.empty() && spelling <= previous))
                throw std::invalid_argument("Invalid or unordered produced feature identity.");
            previous = spelling;
            type(produced.root);
        }
        std::set<std::pair<StringId, StringId>> declarations;
        for (auto id : relationIds(layer)) {
            if (id >= relationRows.size() || usedRelations[id])
                throw std::invalid_argument("Invalid or multiply-owned relation row.");
            usedRelations[id] = true;
            auto const& row = relationRows[id];
            if (row.layer != index || !row.owner || !row.name || row.flags & ~uint8_t{1} ||
                row.locality >
                    unsigned(LayerSchema::RelationDeclaration::Locality::MayCrossContainer) ||
                !pool.layer(name(layer.name))->producesFeatureType(name(row.owner)) ||
                !declarations.emplace(row.owner, row.name).second ||
                !relationTargets.valid(row.targets) || !row.targets ||
                !targetArrays.insert(row.targets).second)
                throw std::invalid_argument("Invalid native forward relation declaration.");
            name(row.name);
            auto targets = relationTargets.range(row.targets);
            if (targets.empty() || targets.size() > MaxEdges - relationEdges)
                throw std::length_error("Invalid or excessive relation target storage.");
            relationEdges += targets.size();
            std::string_view previousTarget;
            for (auto target : targets) {
                auto spelling = name(target);
                if (spelling.empty() || (!previousTarget.empty() && spelling <= previousTarget))
                    throw std::invalid_argument("Invalid or unordered relation target names.");
                previousTarget = spelling;
            }
        }
    }
    requireOwnedArrays(producedFeatures, producedArrays);
    requireOwnedArrays(relationLists, relationArrays);
    requireOwnedArrays(relationTargets, targetArrays);
    for (size_t index = 0; index < relationRows.size(); ++index) {
        auto const& row = relationRows[index];
        if (!usedRelations[index] &&
            (row.owner || row.name || row.targets || row.layer || row.flags || row.locality))
            throw std::invalid_argument("Unreferenced relation row payload.");
    }
}

void MapSchema::Impl::validatePool() const
{
    // Schema payloads use primitive values only. Never walk arbitrary embedded
    // arrays/objects (including malicious cycles) via ModelPool::checkForErrors.
    auto const& model = std::as_const(pool);
    auto stats = model.memoryUsageStats();
    if (model.objectMemberStorage().size() != size_t(simfil::FirstRegularArrayIndex) ||
        model.arrayMemberStorage().size() != size_t(simfil::FirstRegularArrayIndex) ||
        model.objectMemberStorage().singleton_handle_count() ||
        model.arrayMemberStorage().singleton_handle_count() || stats.objectSchemas.logicalBytes ||
        stats.arraySchemas.logicalBytes || stats.typedScalars.logicalBytes ||
        stats.bitmasks.logicalBytes || stats.schemaOwners.logicalBytes)
        throw std::invalid_argument(
            "MapSchema payload may not contain generic compound/qualified values.");
    if (model.numRoots() != layers.size())
        throw std::invalid_argument("Invalid MapSchema root count.");
    for (size_t i = 0; i < layers.size(); ++i) {
        auto root = model.root(i);
        if (!root || (*root)->addr() != ModelNodeAddress{Layers, i})
            throw std::invalid_argument("MapSchema roots must exactly identify its layer views.");
    }
}

/** Serialize canonical columns only. Derived sets/views/indices are rebuilt once.
 * Every variable length is bounded independently of the envelope byte limit.
 */
template <class S, class T, class F>
void MapSchema::Impl::container(S& s, std::vector<T>& data, size_t limit, size_t& memory, F&& item)
{
    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        auto position = s.adapter().currentReadPos();
        size_t count = 0;
        bitsery::details::readSize(s.adapter(), count, limit, std::true_type{});
        // Every item here has at least one wire byte. Reject truncated lengths
        // and charge all nested allocations before bitsery resizes the container.
        if (count > memory / sizeof(T) ||
            !simfil::detail::check_bitsery_payload_available(s.adapter(), count))
            throw std::length_error("MapSchema container exceeds its input/allocation budget.");
        memory -= count * sizeof(T);
        s.adapter().currentReadPos(position);
    }
    s.container(data, limit, std::forward<F>(item));
}

template <class S>
void MapSchema::Impl::string(S& s, std::string& value, size_t limit, size_t& memory)
{
    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        auto position = s.adapter().currentReadPos();
        size_t count = 0;
        bitsery::details::readSize(s.adapter(), count, limit, std::true_type{});
        if (count > memory || !simfil::detail::check_bitsery_payload_available(s.adapter(), count))
            throw std::length_error("MapSchema text exceeds its input/allocation budget.");
        memory -= count;
        s.adapter().currentReadPos(position);
    }
    s.text1b(value, limit);
}

template <class S>
void MapSchema::Impl::readWrite(S& s, size_t budget)
{
    size_t memory = budget > SIZE_MAX / 4 ? SIZE_MAX : budget * 4;
    auto text = [&](auto& a, std::string& v)
    {
        string(a, v, budget, memory);
    };
    auto vector = [&]<class T>(auto& a, std::vector<T>& values, size_t limit)
    {
        container(
            a,
            values,
            limit,
            memory,
            [](auto& b, T& v)
            {
                if constexpr (std::is_same_v<T, uint8_t>)
                    b.value1b(v);
                else if constexpr (std::is_same_v<T, TypeId>)
                    b.value2b(v);
                else
                    b.object(v);
            });
    };
    auto mapFormat = [&](auto& archive, MapFormat& value)
    {
        text(archive, value.name_);
        text(archive, value.version_);
        std::vector<std::pair<std::string, std::string>> subsystems;
        if constexpr (!simfil::detail::bitsery_input_archive<S>)
            for (auto const& pair : value.subsystems_)
                subsystems.push_back(pair);
        container(
            archive,
            subsystems,
            UINT16_MAX,
            memory,
            [&](auto& a, auto& pair)
            {
                text(a, pair.first);
                text(a, pair.second);
            });
        if constexpr (simfil::detail::bitsery_input_archive<S>)
            for (auto const& pair : subsystems)
                if (!value.subsystems_.emplace(pair).second)
                    throw std::invalid_argument("Duplicate format subsystem.");
    };
    mapFormat(s, format);
    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        uint64_t length = 0;
        s.value8b(length);
        if (length > (size_t(simfil::MaxTypeId) + 1) * sizeof(TypeData) || length > budget ||
            length > memory || length % sizeof(TypeData))
            throw std::length_error("MapSchema type column exceeds its namespace/byte budget.");
        memory -= length;
        if (!types.read_payload_from_bitsery(s.adapter(), size_t(length)))
            throw std::invalid_argument("Truncated MapSchema type column.");
    }
    else
        s.object(types);
    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        // Bound the existing column/arena wire records before their codecs allocate.
        auto start = s.adapter().currentReadPos();
        auto column = [&](size_t width, size_t limit)
        {
            uint64_t bytes = 0;
            s.value8b(bytes);
            if (bytes > memory || bytes > simfil::bitsery_max_column_payload_bytes ||
                bytes % width || bytes / width > limit ||
                !simfil::detail::check_bitsery_payload_available(s.adapter(), size_t(bytes)))
                throw std::length_error("MapSchema metadata exceeds its column/arena budget.");
            memory -= size_t(bytes);
            s.adapter().currentReadPos(s.adapter().currentReadPos() + size_t(bytes));
        };
        auto arena = [&](size_t width)
        {
            column(8, MaxEdges + 1);  // compact heads
            column(width, MaxEdges);  // regular values
            column(width, MaxEdges);  // singleton values
            column(1, MaxEdges);      // singleton occupancy
        };
        column(sizeof(OwnerData), MaxEdges);
        arena(sizeof(uint32_t));
        column(sizeof(AliasData), MaxEdges);
        column(sizeof(IdentityData), UINT16_MAX);
        arena(sizeof(simfil::ArrayIndex));
        arena(sizeof(IdentityPartData));
        column(sizeof(LayerData), UINT16_MAX);
        arena(sizeof(ProducedRootData));
        column(sizeof(RelationData), MaxEdges);
        arena(sizeof(uint32_t));
        arena(sizeof(StringId));
        s.adapter().currentReadPos(start);
    }
    s.object(ownerRows);
    s.ext(owners, bitsery::ext::ArrayArenaExt{});
    s.object(aliases);
    s.object(identities);
    s.ext(identityCompositions, bitsery::ext::ArrayArenaExt{});
    s.ext(identityParts, bitsery::ext::ArrayArenaExt{});
    s.object(layers);
    s.ext(producedFeatures, bitsery::ext::ArrayArenaExt{});
    s.object(relationRows);
    s.ext(relationLists, bitsery::ext::ArrayArenaExt{});
    s.ext(relationTargets, bitsery::ext::ArrayArenaExt{});
    if constexpr (simfil::detail::bitsery_input_archive<S>)
        if (s.adapter().error() != bitsery::ReaderError::NoError)
            throw std::invalid_argument("Malformed MapSchema metadata columns/arenas.");
    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        uint64_t bytes = 0;
        s.value8b(bytes);
        if (bytes % sizeof(QueryData) || bytes / sizeof(QueryData) > simfil::MaxTypeId ||
            bytes > memory || bytes > budget)
            throw std::length_error("Query contexts exceed their column budget.");
        memory -= size_t(bytes);
        if (!queryRoots.read_payload_from_bitsery(s.adapter(), size_t(bytes)))
            throw std::invalid_argument("Truncated query context column.");
    }
    else
        s.object(queryRoots);
    container(
        s,
        constituents,
        256,
        memory,
        [&](auto& archive, auto& constituent)
        {
            string(archive, constituent.first, 128, memory);
            mapFormat(archive, constituent.second);
        });
}

void MapSchema::Impl::writeBody(std::ostream& output) const
{
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(output);
    const_cast<Impl*>(this)->readWrite(s, UINT32_MAX);
    std::ostringstream generic(std::ios::binary);
    pool.writeSchemaPayload(generic);
    auto data = generic.str();
    s.text1b(data, UINT32_MAX);
    s.adapter().flush();
    if (!output)
        throw std::runtime_error("Failed to write MapSchema payload.");
}

void MapSchema::Impl::readBody(std::vector<uint8_t> const& input, size_t budget)
{
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> s(Adapter(input.begin(), input.end()));
    readWrite(s, budget);
    std::string generic;
    size_t memory = budget;
    string(s, generic, budget, memory);
    if (s.adapter().error() != bitsery::ReaderError::NoError ||
        !s.adapter().isCompletedSuccessfully())
        throw std::invalid_argument("Truncated, malformed or trailing MapSchema data.");
    pool.readSchemaPayload(
        std::span(reinterpret_cast<uint8_t const*>(generic.data()), generic.size()),
        budget);
}

void MapSchema::finalize()
{
    if (finalized())
        return;
    requireMutable();
    if (schema_->metadataAppendDepth)
        throw std::logic_error("Cannot publish a schema from an active metadata append.");
    // Failed publication never exposes a partially validated owner.
    // Callers retry getOrBuild with a fresh graph, never publish partially frozen state.
    schema_->compile();
    schema_->validate();
    schema_->index();
    for (auto const& name : layerNames())
        addRoot(layer(name));
    schema_->validatePool();

    // Hash the canonical body before freezing. The envelope carries this identity
    // but is excluded from its own hash, so readers can verify it independently.
    std::ostringstream body(std::ios::binary);
    schema_->writeBody(body);
    setIdentity("mapget-schema:sha256:" + picosha2::hash256_hex_string(body.str()));
    ModelPool::freeze();
    schema_->frozen = true;
    schema_->summarize();
}

tl::expected<void, simfil::Error> MapSchema::write(std::ostream& output)
{
    return std::as_const(*this).write(output);
}

tl::expected<void, simfil::Error> MapSchema::write(std::ostream& output) const
{
    try {
        if (!finalized())
            throw std::logic_error("Cannot serialize an unfinished MapSchema.");
        std::ostringstream data(std::ios::binary);
        schema_->writeBody(data);
        auto payload = data.str();
        bitsery::Serializer<bitsery::OutputStreamAdapter> s(output);
        uint32_t magic = 0x4843534d;  // MSCH
        uint32_t version = Impl::CodecVersion;
        s.value4b(magic);
        s.value4b(version);
        auto id = std::string(schemaId());
        s.text1b(id, 128);
        s.text1b(payload, UINT32_MAX);
        s.adapter().flush();
        if (!output)
            throw std::runtime_error("Failed to write MapSchema envelope.");
        return {};
    }
    catch (std::exception const& e) {
        return tl::unexpected(simfil::Error{simfil::Error::EncodeDecodeError, e.what()});
    }
}

MapSchema::ConstPtr
MapSchema::read(std::span<uint8_t const> bytes, std::string_view expectedId, size_t maxBytes)
{
    if (bytes.size() > maxBytes || bytes.size() < 8)
        throw std::length_error("MapSchema artifact exceeds its byte budget or is truncated.");
    std::vector<uint8_t> input(bytes.begin(), bytes.end());
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> s(Adapter(input.begin(), input.end()));
    uint32_t magic = 0, version = 0;
    s.value4b(magic);
    s.value4b(version);
    if (magic != 0x4843534d || version != Impl::CodecVersion)
        throw std::invalid_argument("Unsupported MapSchema binary format.");
    std::string identity, payload;
    size_t memory = bytes.size();
    Impl::string(s, identity, 128, memory);
    Impl::string(s, payload, maxBytes, memory);
    if (s.adapter().error() != bitsery::ReaderError::NoError ||
        !s.adapter().isCompletedSuccessfully())
        throw std::invalid_argument("Malformed MapSchema envelope.");
    if (!expectedId.empty() && identity != expectedId)
        throw std::invalid_argument("MapSchema identity does not match the requested artifact.");
    if (identity != "mapget-schema:sha256:" + picosha2::hash256_hex_string(payload))
        throw std::invalid_argument("MapSchema content checksum mismatch.");
    // Every input is verified before consulting the cache; a warm artifact must
    // never conceal a truncated body or a wrong expected identity.
    return getOrBuild(
        identity,
        [identity, payload = std::move(payload), maxBytes]
        {
            auto result = std::make_shared<MapSchema>();
            result->schema_
                ->readBody(std::vector<uint8_t>(payload.begin(), payload.end()), maxBytes);
            result->schema_->validate();
            result->schema_->validatePool();
            result->schema_->index();
            // Reject noncanonical identities, hidden data and duplicate dictionary IDs.
            std::ostringstream canonical(std::ios::binary);
            result->schema_->writeBody(canonical);
            if (canonical.str() != payload)
                throw std::invalid_argument("Noncanonical MapSchema payload.");
            result->setIdentity(identity);
            result->ModelPool::freeze();
            result->schema_->frozen = true;
            result->schema_->summarize();
            return result;
        });
}

MapSchema::ConstPtr MapSchema::getOrBuild(
    std::string key,
    std::function<ConstPtr()> builder,
    std::function<bool()> cancel)
{
    // Admit at most four top-level builds; nested keys run on their current
    // worker and may increase the in-progress key count without another thread.
    // Workers retain the cache owner so process-static teardown cannot free a
    // mutex beneath detached construction. Completed entries hold only weak owners.
    static auto cache = std::make_shared<Impl::BuildCache>();
    auto& activeKeys = Impl::activeBuildKeys();
    if (key.empty() || !builder)
        throw std::invalid_argument("Schema construction requires a key and builder.");
    if (std::ranges::find(activeKeys, key) != activeKeys.end())
        throw std::logic_error("Reentrant construction of the same schema artifact.");
    bool nested = !activeKeys.empty();
    std::shared_future<ConstPtr> future;

    // Coalesce by construction key before allocating a graph. Waiters can leave
    // independently; only the owning builder publishes success or failure.
    for (;;) {
        if (cancel && cancel())
            throw std::runtime_error("Schema wait cancelled.");
        std::unique_lock lock(cache->mutex);
        for (auto it = cache->ready.begin(); it != cache->ready.end();)
            if (it->second.expired())
                it = cache->ready.erase(it);
            else
                ++it;
        if (auto found = cache->ready.find(key); found != cache->ready.end())
            if (auto owner = found->second.lock())
                return owner;
        if (auto found = cache->running.find(key); found != cache->running.end()) {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
            throw std::logic_error("Reentrant construction of the same schema artifact.");
#else
            future = found->second;
            break;
#endif
        }
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
        if (!nested && cache->running.size() >= 4) {
            cache->changed.wait_for(lock, std::chrono::milliseconds(20));
            continue;
        }
#endif

        // Install the shared future while locked so a competing caller cannot
        // start a second build for this key. Moving the callable retains its owned
        // captures; any borrowed inputs must outlive the shared construction.
        auto promise = std::make_shared<std::promise<ConstPtr>>();
        future = promise->get_future().share();
        cache->running.emplace(key, future);
        auto build = [cache = cache, key, promise, build = std::move(builder)]
        {
            auto& activeKeys = Impl::activeBuildKeys();
            activeKeys.push_back(key);
            auto leave = [](auto* keys)
            {
                keys->pop_back();
            };
            std::unique_ptr<std::vector<std::string>, decltype(leave)> active(&activeKeys, leave);
            try {
                auto owner = build();
                if (!owner || !owner->finalized())
                    throw std::invalid_argument("Schema builder did not return a finalized owner.");
                std::lock_guard lock(cache->mutex);
                if (cache->ready.size() >= 256)
                    cache->ready.erase(cache->ready.begin());
                cache->ready[key] = owner;
                cache->running.erase(key);
                promise->set_value(owner);
            }
            catch (...) {
                std::lock_guard lock(cache->mutex);
                cache->running.erase(key);
                promise->set_exception(std::current_exception());
            }
            cache->changed.notify_all();
        };
        try {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
            // The browser build has no pthread runtime. It still shares the
            // completed immutable artifact but constructs it on this call stack.
            lock.unlock();
            build();
#else
            // A network builder may decode a second, content-addressed key. Reuse
            // this worker instead of waiting for a fifth slot behind four parents.
            if (nested) {
                lock.unlock();
                build();
            }
            else
                std::thread(std::move(build)).detach();
#endif
        }
        catch (...) {
            cache->running.erase(key);
            cache->changed.notify_all();
            throw;
        }
        break;
    }

    // Poll caller cancellation without cancelling the shared construction.
    while (future.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready)
        if (cancel && cancel())
            throw std::runtime_error("Schema wait cancelled.");
    if (cancel && cancel())
        throw std::runtime_error("Schema wait cancelled.");
    return future.get();
}

simfil::ModelNode::Ptr MapSchema::typeNode(TypeId id) const
{
    if (!schemaType(id))
        return {};
    return simfil::SchemaPool::typeNode(id);
}

std::vector<std::string> MapSchema::checkForErrors() const
{
    std::vector<std::string> errors;
    try {
        schema_->validate();
        schema_->validatePool();
    }
    catch (std::exception const& e) {
        errors.push_back(e.what());
    }
    return errors;
}

tl::expected<void, simfil::Error>
MapSchema::resolve(simfil::ModelNode const& node, ResolveFn const& callback) const
{
    if (node.addr().column() == Impl::AttributeOwner ||
        node.addr().column() == Impl::AttributeOwners)
        return resolveAttributeOwners(node, callback);
    if (node.addr().column() == Impl::Layers) {
        if (node.addr().index() >= schema_->layers.size())
            return tl::unexpected(simfil::Error{
                simfil::Error::IndexOutOfRange,
                "Unknown LayerSchema address."});
        auto view = LayerSchema::Ptr::make(shared_from_this(), node.addr());
        callback(*view);
        return {};
    }
    return SchemaPool::resolve(node, callback);
}

LayerSchema::Ptr
resolveInternal(simfil::res::tag<LayerSchema>, MapSchema const& pool, simfil::ModelNode const& node)
{
    if (node.addr().column() != MapSchema::Impl::Layers ||
        node.addr().index() >= pool.schema_->layers.size())
        throw std::invalid_argument("Cannot resolve node as a LayerSchema.");
    return LayerSchema::Ptr::make(pool.shared_from_this(), node.addr());
}

std::string_view LayerSchema::attributeTypeCode(TypeId id) const
{
    auto const& g = *mapSchema()->schema_;
    if (!g.valid(id))
        return {};
    return g.text(g.data(id).attributeCode);
}

TypeId LayerSchema::attributeQuerySchema(std::string_view feature, TypeId attribute) const
{
    return mapSchema()->schema_->findQueryRoot(featureSchema(feature), attribute);
}

TypeId LayerSchema::featureSchema(std::string_view feature) const
{
    // Runtime Feature values request their schema for each field comparison.
    // Produced features already carry the same physical root as their Feature:
    // alias, so use those integer pairs without resolving alias text nodes.
    auto owner = mapSchema();
    auto const& graph = *owner->schema_;
    if (auto name = graph.dictionary.get(feature)) {
        for (auto const& row : graph.featureRoots(graph.layerAt(addr_.index()))) {
            if (row.name == name)
                return row.root;
        }
    }

    // Schema-only imports and reference declarations can expose a Feature:
    // alias without produced membership. Preserve local-then-global lookup;
    // an unknown name still returns zero without extending the dictionary.
    return schemaId(featureKey(feature));
}

TypeId LayerSchema::featurePropertiesSchema(std::string_view feature) const
{
    return schemaId(featurePropertiesKey(feature));
}

TypeId LayerSchema::attributeLayerMapSchema(std::string_view feature) const
{
    return schemaId(attributeLayerMapKey(feature));
}

std::string_view MapSchema::Impl::metaType(TypeId id) const
{
    if (!valid(id))
        return {};
    return text(data(id).metaType);
}

std::vector<LayerSchema::AttributePathOwner> MapSchema::Impl::attributeOwners(TypeId id) const
{
    if (!valid(id))
        return {};
    std::vector<LayerSchema::AttributePathOwner> result;
    for (auto const& owner : ownerSites(data(id).owners))
        result.push_back(
            {std::string(name(owner.feature)),
             std::string(name(owner.layer)),
             std::string(name(owner.name)),
             id,
             owner.featureLayer ? std::string(name(owner.featureLayer)) : std::string{}});
    return result;
}

TypeId
MapSchema::Impl::childSchema(TypeId root, std::string_view field, std::optional<Kind> preferred)
    const
{
    std::vector<TypeId> pending{root};
    std::vector<bool> seen(pool.typeCount() + 1);
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (!valid(id) || seen[id])
            continue;
        seen[id] = true;
        auto view = pool.schemaType(id);
        for (auto alternate : view.alternatives())
            pending.push_back(alternate);
        if (view.hasAffinity(simfil::ValueType::Array) && view.composition() == Composition::None)
            for (auto item : view.elements())
                pending.push_back(item);
        auto occurrence = view.field(field);
        if (!occurrence)
            continue;
        TypeId fallback = 0;
        for (auto child : occurrence.types()) {
            if (!child)
                continue;
            if (!fallback)
                fallback = child;
            if (!preferred || matchesKind(pool.schemaType(child).kind(), *preferred))
                return child;
        }
        if (fallback)
            return fallback;
    }
    return 0;
}

TypeId
LayerSchema::childSchema(TypeId root, std::string_view field, std::optional<Kind> preferred) const
{
    return mapSchema()->schema_->childSchema(root, field, preferred);
}

std::vector<LayerSchema::AttributePathOwner> LayerSchema::attributeOwners(TypeId type) const
{
    return mapSchema()->schema_->attributeOwners(type);
}

LayerSchema::PathOwner LayerSchema::ownerForPath(
    std::string_view featureType,
    simfil::TypeId rootSchema,
    std::span<const std::string> fieldPath) const
{
    auto const& graph = *mapSchema()->schema_;
    auto uniqueOwner = [&](TypeId id, std::string_view feature) -> std::optional<AttributePathOwner>
    {
        std::optional<AttributePathOwner> result;
        for (auto const& owner : graph.attributeOwners(id))
            if (owner.featureType_ == feature &&
                (owner.featureLayer_.empty() || owner.featureLayer_ == name())) {
                if (result)
                    return std::nullopt;
                result = owner;
            }
        return result;
    };
    auto currentSchema = rootSchema;
    if (!graph.valid(currentSchema)) {
        return {};
    }

    auto attributeOwner = uniqueOwner(rootSchema, featureType);
    auto currentLayerName = std::optional<std::string>{};
    auto enteredAttributeBranch = graph.metaType(rootSchema) == "AttributeLayerMap" ||
        graph.metaType(rootSchema) == "AttributeContainer" ||
        graph.metaType(rootSchema) == "Attribute";

    for (auto const& fieldName : fieldPath) {
        if (fieldName.empty()) {
            continue;
        }

        auto const parentSchema = currentSchema;
        auto const parentMetaType = graph.metaType(parentSchema);
        auto pendingAttributeName = std::optional<std::string>{};

        if (parentMetaType == "AttributeLayerMap") {
            // This edge selects the concrete attribute layer, but not yet a
            // concrete attribute within that layer.
            enteredAttributeBranch = true;
            currentLayerName = fieldName;
        }
        else if (parentMetaType == "AttributeContainer") {
            // This edge selects the attribute object inside the active layer.
            enteredAttributeBranch = true;
            pendingAttributeName = fieldName;
        }

        currentSchema = graph.childSchema(currentSchema, fieldName, std::nullopt);
        if (currentSchema == simfil::NoTypeId) {
            return {};
        }

        auto const currentMetaType = graph.metaType(currentSchema);
        if (currentMetaType == "AttributeLayerMap" || currentMetaType == "AttributeContainer") {
            enteredAttributeBranch = true;
        }
        if (currentMetaType == "Attribute") {
            enteredAttributeBranch = true;
            // If the schema id is intentionally shared, the path edge still
            // contains enough context to identify the selected attribute.
            auto attributeName =
                pendingAttributeName.value_or(std::string(attributeTypeCode(currentSchema)));
            if (currentLayerName && !attributeName.empty()) {
                attributeOwner = AttributePathOwner{
                    std::string(featureType),
                    *currentLayerName,
                    std::move(attributeName),
                    currentSchema,
                    std::string(name())};
            }
            else if (!attributeOwner)
                attributeOwner = uniqueOwner(currentSchema, featureType);
        }
        else if (auto owner = uniqueOwner(currentSchema, featureType); !attributeOwner && owner) {
            attributeOwner = *owner;
        }
    }

    if (attributeOwner) {
        return {PathOwnerKind::Attribute, *attributeOwner};
    }

    if (enteredAttributeBranch) {
        return {};
    }

    if (rootSchema == featureSchema(featureType) ||
        rootSchema == featurePropertiesSchema(featureType))
    {
        return {PathOwnerKind::Feature, {}};
    }

    return {};
}

void LayerSchema::setJsonSchemaAnnotations(simfil::TypeId id, nlohmann::json annotations)
{
    mutableOwner().setJsonSchemaAnnotations(id, std::move(annotations));
}

void LayerSchema::setZserioType(simfil::TypeId schemaId, std::string zserioType)
{
    mutableOwner().setZserioType(schemaId, std::move(zserioType));
}

void LayerSchema::setAttributeMetadata(
    simfil::TypeId schemaId,
    AttributePathOwner owner,
    std::string attributeType,
    std::string zserioType)
{
    owner.featureLayer_ = name();
    mutableOwner().setAttributeMetadata(
        schemaId,
        std::move(owner),
        std::move(attributeType),
        std::move(zserioType));
}

std::string LayerSchema::featureKey(std::string_view featureType)
{
    return "Feature:" + std::string(featureType);
}

std::string LayerSchema::featurePropertiesKey(std::string_view featureType)
{
    return "FeatureProperties:" + std::string(featureType);
}

std::string LayerSchema::attributeLayerMapKey(std::string_view featureType)
{
    return "AttributeLayerMap:" + std::string(featureType);
}

std::string LayerSchema::attributeContainerKey(
    std::string_view featureType,
    std::string_view attributeLayerName)
{
    return "AttributeContainer:" + std::string(featureType) + ":" + std::string(attributeLayerName);
}

std::string LayerSchema::attributeKey(
    std::string_view featureType,
    std::string_view attributeLayerName,
    std::string_view attributeTypeCode)
{
    return "Attribute:" + std::string(featureType) + ":" + std::string(attributeLayerName) + ":" +
        std::string(attributeTypeCode);
}

std::string LayerSchema::attributeKey(
    std::string_view featureType,
    std::string_view attributeLayerName,
    std::string_view attributeTypeCode,
    std::string_view sourceDefinitionKey)
{
    std::string result = "AttributeSource:";
    for (auto part : {featureType, attributeLayerName, attributeTypeCode, sourceDefinitionKey}) {
        if (part.empty())
            throw std::invalid_argument("Qualified attribute keys require all four context parts.");
        result += std::to_string(part.size());
        result += ':';
        result += part;
    }
    return result;
}

std::vector<LayerSchema::NamedSchemaPath>
MapSchema::Impl::scalarPaths(TypeId root, std::string_view attribute) const
{
    if (!valid(root) ||
        std::ranges::none_of(
            types,
            [&](auto const& row)
            {
                return text(row.attributeCode) == attribute ||
                    std::ranges::any_of(
                           ownerSites(row.owners),
                           [&](auto const& owner) { return name(owner.name) == attribute; });
            }))
        return {};

    // Probe each node once before enumerating paths. Bare query operands often
    // are not attribute codes; a shared DAG must not expand exponentially merely
    // to prove that a code (or a selected payload scalar) is absent.
    constexpr uint8_t hasAttribute = 1, hasScalar = 2;
    auto count = pool.typeCount() + 1;
    std::vector<std::vector<TypeId>> parents(count);
    std::vector<bool> seen(count);
    std::vector<uint8_t> reachable(count);
    std::vector<TypeId> pending{root};
    seen[root] = true;
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        auto const& row = data(id);
        if (text(row.attributeCode) == attribute ||
            std::ranges::any_of(
                ownerSites(row.owners),
                [&](auto const& owner) { return name(owner.name) == attribute; }))
            reachable[id] |= hasAttribute;
        auto type = pool.schemaType(id);
        if (matchesKind(type.kind(), Kind::Value))
            reachable[id] |= hasScalar;
        auto addChild = [&](TypeId child)
        {
            if (!valid(child))
                return;
            parents[child].push_back(id);
            if (!seen[child]) {
                seen[child] = true;
                pending.push_back(child);
            }
        };
        type.forEachField(
            [&](simfil::SchemaField const& field)
            {
                if (field.role() == simfil::SchemaType::FieldRole::Data)
                    for (auto child : field.types())
                        addChild(child);
            });
        for (auto child : type.elements())
            addChild(child);
        for (auto child : type.alternatives())
            addChild(child);
    }
    for (size_t id = 1; id < count; ++id)
        if (reachable[id])
            pending.push_back(TypeId(id));
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        for (auto parent : parents[id]) {
            auto added = reachable[id] & ~reachable[parent];
            if (added) {
                reachable[parent] |= added;
                pending.push_back(parent);
            }
        }
    }

    std::vector<LayerSchema::NamedSchemaPath> result;
    LayerSchema::NamedSchemaPath path;
    std::vector<bool> active(count);
    size_t work = 0;
    // An attribute shorthand chooses its first payload scalar, not every scalar
    // in the record. Keep this precedence identical to the transient compiler.
    std::function<bool(TypeId, bool, bool, std::string_view)> visit =
        [&](TypeId id, bool selected, bool first, std::string_view boundName)
    {
        if (!valid(id) || active[id])
            return false;
        auto const& row = data(id);
        auto view = pool.schemaType(id);
        auto matches =
            (boundName.empty() ?
                 text(row.attributeCode) == attribute :
                 boundName == attribute && metaType(id) == "Attribute");
        if (id == root && boundName.empty())
            matches = matches ||
                std::ranges::any_of(
                          ownerSites(row.owners),
                          [&](auto const& owner) { return name(owner.name) == attribute; });
        selected = selected || matches;
        // Reachability deliberately includes data metadata edges: only the original
        // path-local walk below decides exclusions and first-payload precedence.
        if (!(reachable[id] & hasScalar) || (!selected && !(reachable[id] & hasAttribute)))
            return false;
        if (++work > 100000 || path.size() > 256)
            throw std::length_error("Schema path traversal budget exceeded.");
        if (selected && matchesKind(view.kind(), Kind::Value)) {
            result.push_back(path);
            return true;
        }
        active[id] = true;
        bool found = false;
        for (auto child : view.alternatives()) {
            found = visit(child, selected, first || matches, boundName) || found;
            if (selected && found)
                break;
        }
        view.forEachField(
            [&](simfil::SchemaField const& field)
            {
                if ((selected && found) || field.role() != simfil::SchemaType::FieldRole::Data)
                    return;
                auto name = field.name();
                auto children = field.types();
                if ((first || matches) &&
                    schema_detail::isAttributeScalarShorthandMetadataField(name))
                    return;
                path.push_back({simfil::SchemaPathSegment::Kind::Field, std::string(name)});
                for (auto child : children) {
                    auto useSite = metaType(id) == "AttributeContainer" ? name : boundName;
                    found = visit(child, selected, false, useSite) || found;
                    if (selected && found)
                        break;
                }
                path.pop_back();
            });
        for (auto child : view.elements()) {
            if (selected && found)
                break;
            path.push_back({simfil::SchemaPathSegment::Kind::ArrayElement, {}});
            found = visit(child, selected, false, boundName) || found;
            path.pop_back();
        }
        active[id] = false;
        return found;
    };
    visit(root, false, false, {});

    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<LayerSchema::NamedSchemaPath>
LayerSchema::scalarFieldPathsForAttribute(TypeId root, std::string_view attribute) const
{
    if (attribute.empty())
        return {};
    return mapSchema()->schema_->scalarPaths(root, attribute);
}

std::vector<LayerSchema::AttributePathOwner> LayerSchema::attributeScopes() const
{
    auto const& g = *mapSchema()->schema_;
    std::vector<AttributePathOwner> result;
    std::vector<TypeId> pending;
    for (auto const& feature : featureTypes())
        pending.push_back(featureSchema(feature));
    std::set<TypeId> seen;
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (!g.valid(id) || !seen.insert(id).second)
            continue;
        for (auto const& owner : g.attributeOwners(id))
            if (producesFeatureType(owner.featureType_) &&
                (owner.featureLayer_.empty() || owner.featureLayer_ == name()))
                result.push_back(owner);
        auto view = g.pool.schemaType(id);
        view.forEachField(
            [&](auto const& field)
            {
                if (field.role() == simfil::SchemaType::FieldRole::Data) {
                    auto children = field.types();
                    pending.insert(pending.end(), children.begin(), children.end());
                }
            });
        for (auto child : view.itemTypes())
            pending.push_back(child);
        for (auto child : view.alternatives())
            pending.push_back(child);
        if (auto base = view.baseType())
            pending.push_back(base.id());
    }
    auto key = [](auto const& o)
    {
        return std::tie(
            o.featureType_,
            o.attributeLayerName_,
            o.attributeName_,
            o.attributeSchema_);
    };
    std::ranges::sort(result, {}, key);
    result.erase(
        std::unique(
            result.begin(),
            result.end(),
            [&](auto const& a, auto const& b) { return key(a) == key(b); }),
        result.end());
    return result;
}

std::vector<std::string> LayerSchema::constantTypeNames(TypeId root, std::string_view symbol) const
{
    auto const& g = *mapSchema()->schema_;
    std::vector<std::string> result;
    std::vector<TypeId> pending{root};
    std::vector<bool> seen(g.types.size());
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (!g.valid(id) || seen[id])
            continue;
        seen[id] = true;
        auto const& d = g.types[id];
        if (d.attributeType &&
            (g.text(d.attributeCode) == symbol ||
             std::ranges::any_of(
                 g.ownerSites(d.owners),
                 [&](auto const& owner) { return g.name(owner.name) == symbol; })))
            result.emplace_back(g.text(d.attributeType));
        auto type = g.pool.schemaType(id);
        auto typeName = type.typeName();
        if (type.hasDirectEnumSymbol(symbol) && !typeName.empty())
            result.emplace_back(typeName);
        type.forEachNamedValue(
            [&](auto const& value)
            {
                if (value.name == symbol && !typeName.empty())
                    result.emplace_back(typeName);
            });
        auto view = g.pool.schemaType(id);
        view.forEachField(
            [&](auto const& field)
            {
                if (field.role() == simfil::SchemaType::FieldRole::Data) {
                    auto children = field.types();
                    pending.insert(pending.end(), children.begin(), children.end());
                }
            });
        for (auto child : view.itemTypes())
            pending.push_back(child);
        for (auto child : view.alternatives())
            pending.push_back(child);
        if (auto base = view.baseType())
            pending.push_back(base.id());
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool LayerSchema::RelationDeclaration::permitsIncoming(
    std::string_view owner,
    std::string_view name,
    std::string_view target) const
{
    return ownerType_ == owner && name_ == name &&
        std::ranges::find(targetTypes_, target) != targetTypes_.end();
}

nlohmann::json
LayerSchema::featureTypeContract(std::string_view name, std::function<void(size_t)> const& reserve)
    const
{
    auto chargeString = [&](std::string_view value)
    {
        if (!reserve)
            return;
        if (value.size() > (std::numeric_limits<size_t>::max() - 2) / 6)
            throw std::length_error("Feature contract string is too large.");
        reserve(2 + value.size() * 6);  // Quotes and worst-case escaping, before allocating JSON.
    };
    auto identity = getTypeInfo(name);
    auto const& graph = *mapSchema()->schema_;
    auto const& layer = graph.layerAt(addr_.index());
    if (reserve)
        reserve(256);
    chargeString(identity.name());
    auto result = nlohmann::json{
        {"name", identity.name()},
        {"uniqueIdCompositions", nlohmann::json::array()},
        {"produced", producesFeatureType(name)},
        {"typeId", featureSchema(name)},
        {"forwardRelationsKnown", graph.relationsKnown(layer)},
        {"forwardRelations", nlohmann::json::array()}};
    for (auto const& composition : identity.compositions()) {
        if (reserve)
            reserve(8);
        auto parts = nlohmann::json::array();
        for (auto part : composition.parts()) {
            if (reserve)
                reserve(128);
            chargeString(part.name());
            chargeString(part.description());
            parts.push_back(part.toJson());
        }
        result["uniqueIdCompositions"].push_back(std::move(parts));
    }

    // Read the frozen relation index directly. A layer-wide vector copy would allocate
    // unrelated declarations before the caller can stop a focused inspection.
    for (auto const& declaration : graph.relationsOf(layer)) {
        if (reserve)
            reserve(0);
        if (graph.name(declaration.owner) != name)
            continue;
        if (reserve)
            reserve(128);
        chargeString(graph.name(declaration.name));
        auto targets = nlohmann::json::array();
        for (auto target : graph.relationTargets.range(declaration.targets)) {
            auto spelling = graph.name(target);
            chargeString(spelling);
            targets.push_back(spelling);
        }
        auto locality = declaration.locality ==
                unsigned(RelationDeclaration::Locality::SameContainer) ?
            "same-container" :
            declaration.locality == unsigned(RelationDeclaration::Locality::MayCrossContainer) ?
            "may-cross-container" :
            "unknown";
        result["forwardRelations"].push_back(
            {{"name", graph.name(declaration.name)},
             {"targetTypes", std::move(targets)},
             {"complete", graph.relationComplete(declaration)},
             {"locality", locality}});
    }
    return result;
}

std::vector<LayerSchema::RelationDeclaration> LayerSchema::relationDeclarations() const
{
    auto const& graph = *mapSchema()->schema_;
    std::vector<RelationDeclaration> result;
    for (auto const& row : graph.relationsOf(graph.layerAt(addr_.index())))
        result.push_back(graph.relationDeclaration(row));
    return result;
}

std::vector<LayerSchema::RelationDeclaration>
LayerSchema::relationsTo(std::string_view target) const
{
    auto const& graph = *mapSchema()->schema_;
    std::vector<RelationDeclaration> result;
    auto sid = graph.dictionary.get(target);
    if (!sid)
        return result;
    auto key = std::pair{sid, uint16_t(addr_.index())};
    auto found = std::lower_bound(
        graph.relationTargetIndex.begin(),
        graph.relationTargetIndex.end(),
        key,
        [](auto const& row, auto wanted) {
            return std::pair{row.target, row.layer} < wanted;
        });
    for (;
         found != graph.relationTargetIndex.end() && std::pair{found->target, found->layer} == key;
         ++found)
        result.push_back(graph.relationDeclaration(graph.relationRows[found->relation]));
    return result;
}

bool LayerSchema::hasRelationDeclarations() const
{
    auto const& graph = *mapSchema()->schema_;
    return graph.relationsKnown(graph.layerAt(addr_.index()));
}

/** A layer remains a small native object even without feature roots (e.g. an
 * open SourceData decoder). Descriptors read the same canonical graph directly.
 */
simfil::ValueType LayerSchema::type() const
{
    return simfil::ValueType::Object;
}

uint32_t LayerSchema::size() const
{
    return 3;
}

StringId LayerSchema::keyAt(int64_t i) const
{
    if (i < 0 || i >= size())
        return 0;
    return mapSchema()->schema_->descriptorKeys[size_t(i)];
}

simfil::ModelNode::Ptr LayerSchema::at(int64_t i) const
{
    if (i == 0)
        return simfil::model_ptr<simfil::ValueNode>::make(simfil::ScalarValueType{name()}, model_);
    if (i == 1)
        return simfil::model_ptr<simfil::ValueNode>::make(
            simfil::ScalarValueType{int64_t(layerType())},
            model_);
    if (i == 2)
        return rootSchema() ? mapSchema()->typeNode(rootSchema()) : simfil::ModelNode::Ptr{};
    return {};
}

simfil::ModelNode::Ptr LayerSchema::get(StringId const& field) const
{
    for (size_t i = 0; i < size(); ++i)
        if (keyAt(i) == field)
            return at(i);
    return {};
}

bool LayerSchema::iterate(IterCallback const& callback) const
{
    for (size_t i = 0; i < size(); ++i)
        if (auto node = at(i); node && !callback(*node))
            return false;
    return true;
}

MemoryUsageBreakdown MapSchema::memoryUsage() const
{
    if (schema_->memorySummary)
        return *schema_->memorySummary;
    MemoryUsageBreakdown result;
    result.add(
        "format",
        stringMemoryUsage(schema_->format.name_) + stringMemoryUsage(schema_->format.version_));
    result.add(
        "subsystems",
        {schema_->format.subsystems_.size() *
             sizeof(decltype(schema_->format.subsystems_)::value_type),
         schema_->format.subsystems_.size() *
             (sizeof(decltype(schema_->format.subsystems_)::value_type) + 3 * sizeof(void*))});
    for (auto const& [name, version] : schema_->format.subsystems_)
        result.add("subsystem-text", stringMemoryUsage(name) + stringMemoryUsage(version));
    result.add("query-contexts", schema_->queryRoots.memory_usage());
    result.add("object", {sizeof(*this) + sizeof(Impl), sizeof(*this) + sizeof(Impl)});
    result.add("model-pool", memoryUsageStats().total());
    result.add("dictionary", strings()->memoryUsage());
    result.add("type-column", schema_->types.memory_usage());
    result.add("attribute-owners", schema_->owners.memory_usage());
    result.add("aliases", schema_->aliases.memory_usage());
    result.add("layers", schema_->layers.memory_usage());
    result.add("attribute-owner-rows", schema_->ownerRows.memory_usage());
    result.add("produced-roots", schema_->producedFeatures.memory_usage());
    result.add("relation-rows", schema_->relationRows.memory_usage());
    result.add("relation-memberships", schema_->relationLists.memory_usage());
    result.add("relation-targets", schema_->relationTargets.memory_usage());
    result.add("relation-target-index", schema_->relationTargetIndex.memory_usage());
    result.add("generic-schema", schemaMemoryUsage());
    result.add("constituents", vectorMemoryUsage(schema_->constituents));
    for (auto const& [identity, format] : schema_->constituents) {
        result.add(
            "constituent-text",
            stringMemoryUsage(identity) + stringMemoryUsage(format.name_) +
                stringMemoryUsage(format.version_));
        result.add(
            "constituent-subsystems",
            {format.subsystems_.size() * sizeof(decltype(format.subsystems_)::value_type),
             format.subsystems_.size() *
                 (sizeof(decltype(format.subsystems_)::value_type) + 3 * sizeof(void*))});
        for (auto const& [name, version] : format.subsystems_)
            result.add(
                "constituent-subsystem-text",
                stringMemoryUsage(name) + stringMemoryUsage(version));
    }
    result.add(
        "contributor-remaps",
        {schema_->contributorRemaps.size() *
             sizeof(decltype(schema_->contributorRemaps)::value_type),
         schema_->contributorRemaps.size() *
             (sizeof(decltype(schema_->contributorRemaps)::value_type) + 3 * sizeof(void*))});
    for (auto const& [identity, remap] : schema_->contributorRemaps)
        result
            .add("contributor-remap-data", stringMemoryUsage(identity) + vectorMemoryUsage(remap));
    result.add("feature-definitions", schema_->identities.memory_usage());
    result.add("identity-compositions", schema_->identityCompositions.memory_usage());
    result.add("identity-parts", schema_->identityParts.memory_usage());
    result.add("identity-name-index", schema_->identityIndex.memory_usage());
    result.add("layer-build-cache", vectorMemoryUsage(schema_->layerBuild));
    for (auto const& cache : schema_->layerBuild) {
        result.add("import-fingerprint", stringMemoryUsage(cache.fingerprint));
        result.add(
            "layer-build-aliases",
            {cache.aliases.size() * sizeof(decltype(cache.aliases)::value_type),
             cache.aliases.size() *
                 (sizeof(decltype(cache.aliases)::value_type) + 3 * sizeof(void*))});
        for (auto const& [key, id] : cache.aliases)
            result.add("layer-build-alias-text", stringMemoryUsage(key));
    }
    return result;
}

void MapSchema::Impl::summarize()
{
    auto counts = pool.schemaStatistics();
    size_t producedCount = 0, relationCount = 0;
    for (auto const& layer : layers) {
        producedCount += featureRoots(layer).size();
        relationCount += relationIds(layer).size();
    }
    memorySummary = pool.memoryUsage();
    statistics = {
        {"counts",
         {{"types", types.empty() ? 0 : types.size() - 1},
          {"physicalTypes", types.empty() ? 0 : types.size() - 1 - queryRoots.size()},
          {"queryContexts", queryRoots.size()},
          {"fields", counts.fields},
          {"fieldEdges", counts.fieldEdges},
          {"contextFields", counts.contextFields},
          {"elementEdges", counts.elementEdges},
          {"alternativeEdges", counts.alternativeEdges},
          {"layers", layers.size()},
          {"featureTypes", identities.size()},
          {"producedFeatureTypes", producedCount},
          {"relationDeclarations", relationCount},
          {"internedNames", pool.strings()->size()}}},
        {"limits",
         {{"typeId", simfil::MaxTypeId}, {"nameId", std::numeric_limits<StringId>::max()}}},
        {"memory", memorySummary->toJson()},
        {"constituents", pool.constituents()}};
}

nlohmann::json MapSchema::constituents() const
{
    auto result = nlohmann::json::array();
    for (auto const& [identity, format] : schema_->constituents)
        result.push_back({{"schemaId", identity}, {"mapFormat", format.toJson()}});
    return result;
}

nlohmann::json MapSchema::statistics() const
{
    if (!finalized())
        throw std::logic_error("MapSchema statistics require a finalized graph.");
    return schema_->statistics;
}

std::span<TypeId const> MapSchema::schemaRemap(std::string_view sourceIdentity) const
{
    if (!finalized())
        throw std::logic_error("Schema translations require a finalized artifact.");
    auto found = schema_->contributorRemaps.find(sourceIdentity);
    if (found == schema_->contributorRemaps.end())
        throw std::invalid_argument(
            "No construction-time translation for the source schema artifact.");
    return found->second;
}

MapSchema::ConstPtr MapSchema::compose(MapFormat format, std::span<ConstPtr const> inputs)
{
    format = MapFormat::fromJson(format.toJson());
    if (inputs.empty() || inputs.size() > 256)
        throw std::invalid_argument("Schema composition requires 1..256 immutable constituents.");
    std::vector<ConstPtr> owners;
    std::set<std::string_view> identities;
    for (auto const& owner : inputs) {
        if (!owner || !owner->finalized())
            throw std::invalid_argument("Schema composition requires finalized input owners.");
        if (identities.insert(owner->schemaId()).second)
            owners.push_back(owner);
    }
    if (owners.size() == 1 && owners.front()->format() == format)
        return owners.front();
    auto key = "compose-v1:" + format.toJson().dump();
    for (auto const& owner : owners)
        key += ":" + std::string(owner->schemaId());
    // Do not share this cache key with the native reader: decoded artifacts have
    // the same content identity but deliberately no producer translation tables.
    return getOrBuild(
        std::move(key),
        [format = std::move(format), owners = std::move(owners)]
        {
            auto result = std::make_shared<MapSchema>(format);
            Impl::CompositionBuilder(*result->schema_, owners).build();
            result->finalize();
            return result;
        });
}

TypeId MapSchema::remapSchema(std::string_view sourceIdentity, TypeId id) const
{
    if (sourceIdentity == schemaId()) {
        if (id && !schema_->valid(id))
            throw std::invalid_argument("Source type is absent from the effective artifact.");
        return id;
    }
    auto remap = schemaRemap(sourceIdentity);
    if (id >= remap.size() || (id && !remap[id]))
        throw std::invalid_argument("Source type is absent from the contributor translation.");
    return remap[id];
}

MemoryUsageBreakdown LayerSchema::memoryUsage() const
{
    // This is a borrowing view. Unique-owner accounting belongs to MapSchema.
    MemoryUsageBreakdown result;
    result.add("view", {sizeof(*this), sizeof(*this)});
    return result;
}

}  // namespace mapget
