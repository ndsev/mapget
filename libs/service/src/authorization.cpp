#include "mapget/service/authorization.h"

#include <algorithm>
#include <utility>

namespace mapget
{

ClientAuthorization::ClientAuthorization(
    AuthHeaders headers,
    std::shared_ptr<nlohmann::json const> claims)
    : headers(std::move(headers)), claims(std::move(claims))
{
}

bool ClientAuthorization::hasClaim(
    nlohmann::json::json_pointer const& pointer,
    std::string const& expected) const
{
    if (!claims || !claims->is_object())
        return false;
    try {
        if (!claims->contains(pointer))
            return false;
        auto const& value = claims->at(pointer);
        return value.is_string() ? value == expected :
            value.is_array() && std::find(value.begin(), value.end(), expected) != value.end();
    }
    catch (nlohmann::json::exception const&) {
        // A valid object-key pointer may encounter an array in this caller's claims.
        // JSON-pointer lookup can throw for empty or overflowing array indices.
        return false;
    }
}

bool ClientAuthorization::matches(AuthClaimRequirements const& required) const
{
    return std::ranges::all_of(required, [&](auto const& rule) {
        return hasClaim(rule.first, rule.second);
    });
}

bool ClientAuthorization::operator==(ClientAuthorization const& other) const
{
    return headers == other.headers &&
        (claims == other.claims || (claims && other.claims && *claims == *other.claims));
}

}  // namespace mapget
