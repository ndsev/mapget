#include "service-locate.h"

#include "locate.h"
#include "mapget/log.h"
#include "mapget/model/featureid.h"
#include "mapget/model/layerschema.h"

namespace mapget::detail
{

LocateRequestExecution::LocateRequestExecution(Service::Impl& service, LocateRequest request,
    std::optional<ClientAuthorization> authorization)
    : service_(service), request_(std::move(request)), authorization_(std::move(authorization))
{
}

std::vector<LocateRequestExecution::CandidateGroup> LocateRequestExecution::planCandidates() const
{
    std::vector<CandidateGroup> groups;
    std::map<std::pair<std::string, MapPartitionKey>, size_t> groupIndex;
    for (auto const& source : service_.dataSources_.snapshot()) {
        if (source->info->mapId_ != request_.mapId_ || source->info->isAddOn_ ||
            !source->authorized(authorization_)) {
            continue;
        }
        auto appendCandidates = [&](LocateRequest const& resolvedRequest)
        {
            for (auto&& candidate : source->dataSource->locate(resolvedRequest)) {
                if ((resolvedRequest.layerId_ &&
                     candidate.tileKey_.layerId_ != *resolvedRequest.layerId_) ||
                    (request_.partition_ &&
                     candidate.tileKey_.partitionId_ != *request_.partition_))
                    continue;
                auto layer = source->info->getLayer(candidate.tileKey_.layerId_, false);
                if (candidate.tileKey_.layer_ != LayerType::Features ||
                    candidate.tileKey_.mapId_ != request_.mapId_ || !layer ||
                    layer->partitionKind_ != candidate.tileKey_.partitionId_.kind())
                {
                    log().warn(
                        "Datasource returned an invalid locate candidate for {}.",
                        candidate.tileKey_.toString());
                    continue;
                }
                auto key = std::make_pair(source->sourceId, candidate.tileKey_);
                auto [found, inserted] = groupIndex.emplace(key, groups.size());
                if (inserted) {
                    groups.push_back(CandidateGroup{
                        .registration = source,
                        .tileKey = candidate.tileKey_,
                    });
                }
                groups[found->second].candidates.push_back(std::move(candidate));
            }
        };

        auto normalized = false;
        for (auto const& [_, layerInfo] : source->info->layers_) {
            if (!layerInfo || layerInfo->type_ != LayerType::Features ||
                (request_.layerId_ && *request_.layerId_ != layerInfo->layerId_))
            {
                continue;
            }
            ParsedFeatureId parsed;
            // Named foreign parts become canonical only against the destination layer.
            // Their JSON/object order is not an ID-composition order.
            bool const matched = request_.canonicalFeatureId_ ?
                parseFeatureIdString(*request_.canonicalFeatureId_, *layerInfo, parsed) :
                normalizeFeatureIdParts(request_.typeId_, request_.featureId_, *layerInfo, parsed);
            if (matched && layerInfo->layerSchema()->producesFeatureType(parsed.typeId_)) {
                normalized = true;
                auto resolved = LocateRequest{
                    request_.mapId_,
                    std::move(parsed.typeId_),
                    std::move(parsed.keyValuePairs_)};
                resolved.layerId_ = layerInfo->layerId_;
                resolved.partition_ = request_.partition_;
                appendCandidates(resolved);
            }
        }
        // Named lookup keys need not be complete feature identities: providers may
        // resolve partial IDs or their own secondary keys. Delegate once, unchanged,
        // when no schema composition could order them. Canonical strings remain strict.
        if (!normalized && !request_.canonicalFeatureId_)
            appendCandidates(request_);
    }
    return groups;
}

std::vector<LocateResponse> LocateRequestExecution::run()
{
    auto groups = planCandidates();
    pending_ = groups.size();
    for (auto& group : groups) {
        auto registration = group.registration;
        auto child = std::make_shared<LayerTilesRequest>(
            group.tileKey.mapId_,
            group.tileKey.layerId_,
            std::vector<PartitionId>{group.tileKey.partitionId_});
        child->sourceId_ = registration->sourceId;
        child->onFeatureLayer(
            [this, group = std::move(group)](PartitionFeatureLayer::Ptr tile)
            {
                if (!tile || tile->id() != group.tileKey) {
                    return;
                }
                std::lock_guard lock(mutex_);
                for (auto const& candidate : group.candidates) {
                    auto selected = resolveLocateCandidate(candidate, *tile);
                    if (!selected) {
                        log().warn(
                            "Could not evaluate locate selector in {}: {}",
                            tile->id().toString(),
                            selected.error().message);
                        continue;
                    }
                    for (auto const& feature : *selected) {
                        LocateResponse response(request_);
                        response.tileKey_ = tile->id();
                        response.typeId_ = std::string(feature->typeId());
                        response.featureId_ = castToKeyValue(feature->id()->keyValuePairs());
                        response.resolvedCanonicalFeatureId_ = feature->id()->toString();
                        if (seen_.insert(response.serialize().dump()).second) {
                            results_.push_back(std::move(response));
                        }
                    }
                }
            });
        child->onDone_ = [this](RequestStatus)
        {
            {
                std::lock_guard lock(mutex_);
                if (pending_ > 0) {
                    --pending_;
                }
            }
            cv_.notify_all();
        };
        // Candidate selectors and loaded models must belong to the same registration.
        (void)service_.requestTiles({child}, authorization_, registration);
    }

    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return pending_ == 0; });
    auto results = std::move(results_);
    lock.unlock();
    std::ranges::sort(
        results,
        [](auto const& left, auto const& right)
        {
            return std::tie(left.tileKey_, left.resolvedCanonicalFeatureId_) <
                std::tie(right.tileKey_, right.resolvedCanonicalFeatureId_);
        });
    return results;
}

}  // namespace mapget::detail

namespace mapget
{

std::vector<LocateResponse> Service::Impl::locate(
    LocateRequest const& request, std::optional<ClientAuthorization> const& authorization)
{
    return detail::LocateRequestExecution(*this, request, authorization).run();
}

}  // namespace mapget
