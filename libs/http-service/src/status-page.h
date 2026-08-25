#pragma once

#include <string_view>

namespace mapget::detail
{

/** Return the self-contained customer-facing service status dashboard. */
[[nodiscard]] std::string_view statusPageHtml();

}  // namespace mapget::detail
