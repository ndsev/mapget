#include "tiles-ws-status.h"

namespace mapget::detail
{

/** Convert backend request status values into stable status-frame strings. */
std::string_view requestStatusToString(RequestStatus status)
{
    switch (status) {
    case RequestStatus::Open:
        return "Open";
    case RequestStatus::Success:
        return "Success";
    case RequestStatus::NoDataSource:
        return "NoDataSource";
    case RequestStatus::Unauthorized:
        return "Unauthorized";
    case RequestStatus::Aborted:
        return "Aborted";
    }
    return "Unknown";
}

/** Convert missing-datasource diagnostics into compact JSON values. */
std::string_view noDataSourceReasonToString(NoDataSourceReason reason)
{
    switch (reason) {
    case NoDataSourceReason::EmptySources:
        return "emptySources";
    case NoDataSourceReason::AllSourcesDisabled:
        return "allSourcesDisabled";
    case NoDataSourceReason::DatasourceInitializationFailed:
        return "datasourceInitializationFailed";
    case NoDataSourceReason::MissingMapOrLayer:
        return "missingMapOrLayer";
    case NoDataSourceReason::NoConfig:
        return "noConfig";
    case NoDataSourceReason::None:
        break;
    }
    return "";
}

/** Convert tile load-state updates into stable load-state-frame strings. */
std::string_view loadStateToString(TileLayer::LoadState state)
{
    switch (state) {
    case TileLayer::LoadState::LoadingQueued:
        return "LoadingQueued";
    case TileLayer::LoadState::BackendFetching:
        return "BackendFetching";
    case TileLayer::LoadState::BackendConverting:
        return "BackendConverting";
    }
    return "Unknown";
}

} // namespace mapget::detail
