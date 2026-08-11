#include "service-locate.h"

#include "locate.h"
#include "mapget/log.h"
#include "mapget/model/featureid.h"

namespace mapget::detail
{

LocateRequestExecution::LocateRequestExecution(Service::Impl& service, LocateRequest request)
    : service_(service), request_(std::move(request))
{
}

std::vector<LocateRequestExecution::CandidateGroup> LocateRequestExecution::planCandidates() const
{
    std::vector<CandidateGroup> groups;
    std::map<std::pair<std::string, MapTileKey>, size_t> groupIndex;
    for (auto const& source : service_.dataSources_.snapshot()) {
        if (source->info->mapId_ != request_.mapId_ || source->info->isAddOn_) {
            continue;
        }
        auto appendCandidates = [&](LocateRequest const& resolvedRequest)
        {
            for (auto&& candidate : source->dataSource->locate(resolvedRequest)) {
                if (candidate.tileKey_.layer_ != LayerType::Features ||
                    candidate.tileKey_.mapId_ != request_.mapId_ ||
                    !source->info->getLayer(candidate.tileKey_.layerId_))
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
                        .sourceId = source->sourceId,
                        .tileKey = candidate.tileKey_,
                    });
                }
                groups[found->second].candidates.push_back(std::move(candidate));
            }
        };

        if (!request_.canonicalFeatureId_) {
            appendCandidates(request_);
            continue;
        }
        for (auto const& [_, layerInfo] : source->info->layers_) {
            if (!layerInfo || layerInfo->type_ != LayerType::Features) {
                continue;
            }
            ParsedFeatureId parsed;
            if (parseFeatureIdString(*request_.canonicalFeatureId_, *layerInfo, parsed)) {
                appendCandidates(LocateRequest{
                    request_.mapId_,
                    std::move(parsed.typeId_),
                    std::move(parsed.keyValuePairs_),
                });
            }
        }
    }
    return groups;
}

std::vector<LocateResponse> LocateRequestExecution::run()
{
    auto groups = planCandidates();
    pending_ = groups.size();
    for (auto& group : groups) {
        auto child = std::make_shared<LayerTilesRequest>(
            group.tileKey.mapId_,
            group.tileKey.layerId_,
            std::vector<TileId>{group.tileKey.tileId_});
        if (!group.sourceId.empty()) {
            child->sourceId_ = group.sourceId;
        }
        child->onFeatureLayer(
            [this, group = std::move(group)](TileFeatureLayer::Ptr tile)
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
        (void)service_.requestTiles({child}, {});
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

std::vector<LocateResponse> Service::Impl::locate(LocateRequest const& request)
{
    return detail::LocateRequestExecution(*this, request).run();
}

}  // namespace mapget
