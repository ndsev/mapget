#pragma once

#include "mapget/model/layer.h"
#include "mapget/service/service.h"

#include <string_view>

namespace mapget::detail
{

/** Convert request status enum values to stable UI-facing strings. */
[[nodiscard]] std::string_view requestStatusToString(RequestStatus status);

/** Convert missing-datasource reasons to compact JSON status strings. */
[[nodiscard]] std::string_view noDataSourceReasonToString(NoDataSourceReason reason);

/** Convert tile load-state enum values to stable UI-facing strings. */
[[nodiscard]] std::string_view loadStateToString(TileLayer::LoadState state);

} // namespace mapget::detail
