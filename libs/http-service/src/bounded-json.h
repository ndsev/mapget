#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string_view>

namespace mapget::detail
{

/** Bound external JSON before recursive consumers; reject ambiguous duplicate object keys. */
[[nodiscard]] nlohmann::json parseBoundedJson(std::string_view input, size_t maxBytes, size_t maxDepth = 64);

}  // namespace mapget::detail
