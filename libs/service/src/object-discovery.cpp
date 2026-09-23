#include "mapget/service/object-discovery.h"
#include <cmath>
#include <set>

namespace mapget
{
void ObjectDiscoveryResult::validate() const
{
    if (status_ != Status::Success && status_ != Status::Unavailable && status_ != Status::Failed)
        throw std::invalid_argument("Unknown discovery status.");
    if (ttl_.count() < 0)
        throw std::invalid_argument("Negative discovery TTL.");
    if (status_ != Status::Success && !objects_.empty())
        throw std::invalid_argument(
            "Failed or unavailable discovery cannot return object associations.");
    for (auto const& reference : objects_) {
        if (!reference.bounds_)
            continue;
        auto const& b = *reference.bounds_;
        for (auto value : b)
            if (!std::isfinite(value))
                throw std::invalid_argument("Discovery bounds must be finite.");
        // West > east is intentional for antimeridian-crossing extents.
        if (b[0] < -180 || b[0] > 180 || b[2] < -180 || b[2] > 180 || b[1] < -90 || b[3] > 90 ||
            b[1] > b[3])
            throw std::invalid_argument("Discovery bounds must be valid WGS84 extents.");
    }
}

nlohmann::json ObjectDiscoveryResult::toJson() const
{
    validate();
    auto objects = nlohmann::json::array();
    std::set<uint64_t> seen;
    for (auto const& reference : objects_) {
        if (!seen.insert(reference.objectId_).second)
            continue;
        auto object = nlohmann::json{{"id", std::to_string(reference.objectId_)}};
        if (reference.bounds_)
            object["bounds"] = *reference.bounds_;
        objects.push_back(std::move(object));
    }
    return {
        {"status",
         status_ == Status::Success ?
             "success" :
             status_ == Status::Unavailable ?
             "unavailable" :
             "failed"},
        {"objects", std::move(objects)},
        {"message", message_},
        {"timestamp",
         std::chrono::duration_cast<std::chrono::milliseconds>(timestamp_.time_since_epoch())
             .count()},
        {"ttlMs", ttl_.count()}};
}
ObjectDiscoveryResult ObjectDiscoveryResult::fromJson(nlohmann::json const& json)
{
    ObjectDiscoveryResult result;
    auto const status = json.at("status").get<std::string>();
    if (status == "success")
        result.status_ = Status::Success;
    else if (status == "unavailable")
        result.status_ = Status::Unavailable;
    else if (status == "failed")
        result.status_ = Status::Failed;
    else
        throw std::invalid_argument("Unknown discovery status.");
    result.message_ = json.value("message", std::string{});
    result.timestamp_ = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(json.at("timestamp").get<int64_t>()));
    result.ttl_ = std::chrono::milliseconds(json.at("ttlMs").get<int64_t>());
    if (!json.at("objects").is_array())
        throw std::invalid_argument("Discovery objects must be an array.");
    for (auto const& object : json.at("objects")) {
        Reference reference{
            PartitionId::fromJson({{"kind", "object"}, {"id", object.at("id")}}).objectId()};
        if (object.contains("bounds")) {
            if (!object.at("bounds").is_array() || object.at("bounds").size() != 4)
                throw std::invalid_argument("Discovery bounds require exactly four coordinates.");
            reference.bounds_ = object.at("bounds").get<std::array<double, 4>>();
        }
        result.objects_.push_back(std::move(reference));
    }
    result.validate();
    return result;
}
}  // namespace mapget
