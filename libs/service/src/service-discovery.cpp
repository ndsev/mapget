#include "service-impl.h"

namespace mapget
{
void Service::discoverObjects(
    ObjectDiscoveryRequest request,
    std::function<void(ObjectDiscoveryResult)> callback,
    std::optional<AuthHeaders> const& clientHeaders)
{
    if (!callback)
        return;
    detail::RegisteredDataSource::Ptr selected;
    std::string error;
    auto matches = impl_->dataSources_
                       .matchingPrimarySources(request.mapId_, request.layerId_, request.sourceId_);
    for (auto const& source : matches) {
        if (clientHeaders && !source->dataSource->isDataSourceAuthorized(*clientHeaders))
            continue;
        if (selected) {
            error = "Ambiguous discovery datasource; specify sourceId.";
            break;
        }
        selected = source;
    }
    if (!selected)
        error = "No authorized datasource for this discovery request.";
    if (selected && error.empty()) {
        auto const& layer = selected->info->layers_.at(request.layerId_);
        if (layer->partitionKind_ != PartitionKind::Object)
            error = "Object discovery requires an object-backed layer.";
        else if (
            !request.tileId_.isValid() || !layer->tileAssociationLevel_ ||
            request.tileId_.level() != *layer->tileAssociationLevel_)
            error = "Discovery tile level must match the advertised tileAssociationLevel.";
    }
    if (!error.empty()) {
        ObjectDiscoveryResult result;
        result.status_ = ObjectDiscoveryResult::Status::Failed;
        result.message_ = std::move(error);
        callback(std::move(result));
        return;
    }
    impl_->scheduler_
        .enqueueDiscovery(std::move(selected), std::move(request), std::move(callback));
}
}  // namespace mapget
