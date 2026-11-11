#include "bounded-json.h"

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace mapget::detail
{

nlohmann::json parseBoundedJson(std::string_view input, size_t maxBytes, size_t maxDepth)
{
    if (input.size() > maxBytes)
        throw std::invalid_argument("JSON exceeds its byte limit.");

    std::vector<std::set<std::string>> objectKeys;
    return nlohmann::json::parse(input, [maxDepth, &objectKeys](int depth, auto event, auto& value) {
        if (depth < 0 || static_cast<size_t>(depth) > maxDepth)
            throw std::invalid_argument("JSON exceeds its nesting limit.");
        if (event == nlohmann::json::parse_event_t::object_start)
            objectKeys.emplace_back();
        if (event == nlohmann::json::parse_event_t::object_end)
            objectKeys.pop_back();
        if (event == nlohmann::json::parse_event_t::key &&
            !objectKeys.back().insert(value.template get<std::string>()).second)
            throw std::invalid_argument("JSON contains a duplicate object member.");
        return true;
    });
}

}  // namespace mapget::detail
