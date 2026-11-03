#include "service-datasources.h"

#include "mapget/log.h"
#include "mapget/model/mapschema.h"

#include <algorithm>
#include <map>
#include <set>
#include <system_error>

namespace mapget::detail
{

void DataSourceRegistry::compose(std::vector<RegisteredDataSource::Ptr>& sources) const
{
    for (auto& source : sources) {
        if (source->nativeInfo->isAddOn_)
            continue;
        std::vector<RegisteredDataSource::Ptr> addons;
        for (auto const& candidate : sources)
            if (candidate->nativeInfo->isAddOn_ &&
                candidate->nativeInfo->mapId_ == source->nativeInfo->mapId_)
                addons.push_back(candidate);
        if (addons == source->addOns)
            continue;

        auto replacement = std::make_shared<RegisteredDataSource>(*source);
        replacement->addOns = std::move(addons);
        replacement->info = replacement->nativeInfo;
        if (replacement->addOns.empty()) {
            replacement->metadataMemory = replacement->info->memoryUsage().total();
            source = std::move(replacement);
            continue;
        }
        auto metadata = cloneDataSourceInfo(*source->nativeInfo);
        auto format = metadata.mapFormat_;
        auto base = metadata.mapSchema_;
        std::vector<MapSchema::ConstPtr> owners{base};
        // Native add-on iteration remains in registration order. Only graph composition is
        // canonicalized by immutable content so startup timing cannot change its identity.
        std::map<std::string, MapSchema::ConstPtr> contributions;
        for (auto const& addon : replacement->addOns) {
            auto const& native = *addon->nativeInfo;
            if (!native.mapSchema_ || !base)
                throw std::invalid_argument("Add-on composition requires finalized schema owners.");
            for (auto const& [name, layer] : native.layers_) {
                if (layer->type_ != LayerType::Features)
                    throw std::invalid_argument("Add-on contributions must be feature layers.");
                if (auto existing = metadata.getLayer(name, false)) {
                    if (existing->type_ != layer->type_ ||
                        existing->partitionKind_ != layer->partitionKind_)
                        throw std::invalid_argument(
                            "Add-on layer kind/partition conflicts with the base map: " + name);
                }
                else if (layer->partitionKind_ == PartitionKind::Object)
                    throw std::invalid_argument(
                        "An add-on-only object layer requires an explicit discovery and geometry "
                        "anchor provider; advertise the layer on the primary datasource first: " +
                        name);
                else
                    metadata.layers_.emplace(name, cloneLayerInfo(*layer));
            }
            if (native.schemaId_ == base->schemaId())
                continue;
            contributions.emplace(native.schemaId_, native.mapSchema_);
            if (!native.mapFormat_.name_.empty() &&
                (native.mapFormat_.name_ != format.name_ ||
                 native.mapFormat_.version_ != format.version_))
                log().warn(
                    "Add-on '{}' format '{} {}' differs from base '{} {}'; retaining the base "
                    "family/version.",
                    addon->sourceId,
                    native.mapFormat_.name_,
                    native.mapFormat_.version_,
                    format.name_,
                    format.version_);
            format.subsystems_
                .insert(native.mapFormat_.subsystems_.begin(), native.mapFormat_.subsystems_.end());
            if (native.mapFormat_.name_.empty())
                format.subsystems_.emplace("mapget.legacy-addon", "v1:" + native.schemaId_);
        }
        if (!contributions.empty()) {
            // An unnamed legacy base has no family to retain; give only that compatibility
            // case a reserved family, never derive format identity from a display map name.
            if (format.name_.empty()) {
                format.name_ = "mapget.legacy";
                format.version_ = "1";
            }
            for (auto const& [_, owner] : contributions)
                owners.push_back(owner);
            metadata.mapFormat_ = {};
            metadata.schemaId_.clear();
            metadata.setMapSchema(MapSchema::compose(std::move(format), owners));
            replacement->info = std::make_shared<DataSourceInfo const>(std::move(metadata));
        }
        else if (metadata.layers_.size() != replacement->nativeInfo->layers_.size()) {
            metadata.setMapSchema(base);
            replacement->info = std::make_shared<DataSourceInfo const>(std::move(metadata));
        }
        replacement->metadataMemory = replacement->info->memoryUsage().total();
        if (replacement->info != replacement->nativeInfo)
            replacement->metadataMemory += replacement->nativeInfo->memoryUsage().total();
        source = std::move(replacement);
    }
}

void DataSourceRegistry::rememberSchemas()
{
    std::erase_if(knownSchemas_, [](auto const& owner) { return owner.expired(); });
    std::set<MapSchema const*> seen;
    for (auto const& weak : knownSchemas_)
        if (auto owner = weak.lock())
            seen.insert(owner.get());
    for (auto const& source : sources_)
        for (auto const& info : {source->nativeInfo, source->info})
            if (info->mapSchema_ && seen.insert(info->mapSchema_.get()).second)
                knownSchemas_.push_back(info->mapSchema_);
}

MemoryUsageBreakdown DataSourceRegistry::schemaMemoryUsage() const
{
    std::vector<std::pair<MapSchema::ConstPtr, bool>> owners;
    {
        std::unique_lock lock(mutex_);
        std::erase_if(knownSchemas_, [](auto const& owner) { return owner.expired(); });
        std::set<MapSchema const*> current;
        for (auto const& source : sources_) {
            current.insert(source->info->mapSchema_.get());
            current.insert(source->nativeInfo->mapSchema_.get());
        }
        for (auto const& weak : knownSchemas_)
            if (auto owner = weak.lock())
                owners.emplace_back(owner, current.contains(owner.get()));
    }
    MemoryUsageBreakdown result;
    for (auto const& [owner, current] : owners)
        result.merge(current ? "current" : "retired-pinned", owner->memoryUsage());
    return result;
}

std::shared_ptr<MapSchema const> DataSourceRegistry::getSchema(
    std::string_view sourceId,
    std::string_view schemaId,
    std::optional<ClientAuthorization> const& clientAuthorization) const
{
    if (schemaId.empty())
        return {};

    for (auto const& source : snapshot()) {
        if ((!sourceId.empty() && source->sourceId != sourceId) ||
            (source->info->schemaId_ != schemaId &&
             (!sourceId.empty() || source->nativeInfo->schemaId_ != schemaId)))
            continue;
        // A content identity is not a capability. Check a current provider even for
        // format-only clients, and never call arbitrary authorization code under our lock.
        if (!source->authorized(clientAuthorization))
            continue;
        std::shared_lock lock(mutex_);
        if (std::ranges::find(sources_, source) != sources_.end())
            return source->info->schemaId_ == schemaId ?
                source->info->mapSchema_ :
                source->nativeInfo->mapSchema_;
    }
    return {};
}

nlohmann::json DataSourceRegistry::schemaInventory(
    std::optional<ClientAuthorization> const& clientAuthorization,
    std::function<bool(MapSchema const&)> const& include,
    std::function<void()> const& checkpoint,
    std::function<bool(std::string_view sourceId, std::string_view mapId)> const& includeSource)
    const
{
    if (checkpoint)
        checkpoint();
    auto const sources = snapshot();
    auto result = nlohmann::json::array();
    std::map<std::string, size_t> artifacts;
    std::set<MapSchema const*> countedOwners;
    for (auto const& source : sources) {
        if (checkpoint)
            checkpoint();
        if (!source->authorized(clientAuthorization))
            continue;
        // Select providers before expanding either native or effective graphs. Filtering
        // after row construction would let an unrelated large source exhaust the budget.
        if (includeSource && !includeSource(source->sourceId, source->info->mapId_))
            continue;
        auto metadata = std::vector{source->info};
        if (source->nativeInfo != source->info)
            metadata.push_back(source->nativeInfo);
        for (auto const& info : metadata) {
            if (checkpoint)
                checkpoint();
            auto const& owner = info->mapSchema_;
            // Rejected artifacts allocate no layer/type/statistics/consumer rows. The
            // predicate sees only authorized owners, never hidden provider metadata.
            if (!owner || (include && !include(*owner)))
                continue;
            auto [position, inserted] =
                artifacts.try_emplace(std::string(owner->schemaId()), result.size());
            if (inserted) {
                auto layers = nlohmann::json::array();
                for (auto const& name : owner->layerNames()) {
                    if (checkpoint)
                        checkpoint();
                    auto layer = owner->layer(name);
                    auto types = nlohmann::json::array();
                    for (auto const& type : layer->featureTypes()) {
                        if (checkpoint)
                            checkpoint();
                        types.push_back({{"name", type}, {"schemaId", layer->featureSchema(type)}});
                    }
                    layers.push_back(
                        {{"layerId", name},
                         {"type", layer->layerType()},
                         {"schemaId", layer->rootSchema()},
                         {"featureTypes", std::move(types)}});
                }
                result.push_back(
                    {{"mapSchemaId", owner->schemaId()},
                     {"mapFormat", owner->format().toJson()},
                     {"layers", std::move(layers)},
                     {"statistics", owner->statistics()},
                     {"ownership", "current"},
                     {"ownerInstances", 0},
                     {"retainedBytes", 0},
                     {"retiredOwnership", "aggregate-memory-accounting"},
                     {"consumers", nlohmann::json::array()}});
            }
            if (checkpoint)
                checkpoint();
            auto& item = result[position->second];
            if (countedOwners.insert(owner.get()).second) {
                item["ownerInstances"] = item["ownerInstances"].get<size_t>() + 1;
                item["retainedBytes"] = item["retainedBytes"].get<size_t>() +
                    owner->statistics().at("memory").value("allocated-bytes", size_t{0});
            }
            auto consumer = nlohmann::json{
                {"sourceId", source->sourceId},
                {"mapId", info->mapId_},
                {"addOn", info->isAddOn_},
                {"role", info == source->info ? "published" : "provider"}};
            if (info == source->info && !source->addOns.empty()) {
                auto& contributors = consumer["contributors"] = nlohmann::json::array();
                contributors.push_back(
                    {{"sourceId", source->sourceId},
                     {"mapSchemaId", source->nativeInfo->schemaId_},
                     {"mapFormat", source->nativeInfo->mapFormat_.toJson()}});
                for (auto const& addon : source->addOns) {
                    if (checkpoint)
                        checkpoint();
                    contributors.push_back(
                        {{"sourceId", addon->sourceId},
                         {"mapSchemaId", addon->nativeInfo->schemaId_},
                         {"mapFormat", addon->nativeInfo->mapFormat_.toJson()}});
                }
            }
            item["consumers"].push_back(std::move(consumer));
        }
    }
    if (checkpoint)
        checkpoint();
    // Registrations are immutable and pinned above. One linear identity comparison
    // catches replacement/removal, including effective add-on generations, without
    // quadratic membership scans or invoking caller/auth code under the lock.
    {
        std::shared_lock lock(mutex_);
        if (sources_ != sources)
            throw std::system_error(
                std::make_error_code(std::errc::resource_unavailable_try_again),
                "Datasource schema inventory changed during traversal");
    }
    return result;
}

}  // namespace mapget::detail
