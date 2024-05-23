#include "locate.h"

namespace mapget
{

LocateRequest::LocateRequest(const nlohmann::json& j)
{
    if (j.contains("mapId"))
        mapId_ = j["mapId"].get<std::string>();
    if (j.contains("typeId"))
        typeId_ = j["typeId"].get<std::string>();
    if (j.contains("featureId")) {
        auto featureIdParts = j["featureId"];
        auto numFeatureIdParts = featureIdParts.size();
        for (auto kvIndex = 0; kvIndex < numFeatureIdParts; kvIndex += 2) {
            auto key = featureIdParts.at(kvIndex).get<std::string>();
            auto value = featureIdParts.at(kvIndex + 1);
            if (value.is_number())
                featureId_.emplace_back(key, value.get<int64_t>());
            else
                featureId_.emplace_back(key, value.get<std::string>());
        }
    }
}

LocateRequest::LocateRequest(std::string mapId, std::string typeId, KeyValuePairs featureId) :
    mapId_(std::move(mapId)), typeId_(std::move(typeId)), featureId_(std::move(featureId))
{}

nlohmann::json LocateRequest::serialize() const
{
    nlohmann::json featureId = nlohmann::json::array();
    for (auto const& [k, v] : featureId_) {
        featureId.emplace_back(k);
        std::visit([&featureId](auto&& vv){
            featureId.emplace_back(vv);
        }, v);
    }
    return nlohmann::json::object({
        {"mapId", mapId_},
        {"typeId", typeId_},
        {"featureId", featureId},
    });
}

LocateResponse::LocateResponse(const LocateRequest& req) : LocateRequest(req)
{
    tileKey_.mapId_ = req.mapId_;
    tileKey_.layer_ = LayerType::Features;
}

LocateResponse::LocateResponse(const nlohmann::json& j) : LocateRequest(j)
{
    if (j.contains("tileId")) {
        tileKey_ = MapTileKey(j["tileId"].get<std::string>());
    }
}

nlohmann::json LocateResponse::serialize() const
{
    auto result = LocateRequest::serialize();
    result["tileId"] = tileKey_.toString();
    return result;
}

std::optional<int64_t> LocateRequest::getIntIdPart(const std::string_view& idPart) const
{
    for (auto const& [key, value] : featureId_) {
        if (key == idPart) {
            if (std::holds_alternative<int64_t>(value)) {
                return std::get<int64_t>(value);
            }
        }
    }
    return {};
}

std::optional<std::string_view> LocateRequest::getStrIdPart(const std::string_view& idPart) const
{
    for (auto const& [key, value] : featureId_) {
        if (key == idPart) {
            if (std::holds_alternative<std::string>(value)) {
                return std::get<std::string>(value);
            }
        }
    }
    return {};
}

void LocateRequest::setFeatureId(const KeyValueViewPairs& kvp)
{
    // Convert KeyValueViewPairs to KeyValuePairs
    featureId_.clear();
    for (auto const& [k, v] : kvp) {
        std::visit([this, &k](auto&& vv){
            if constexpr (std::is_same_v<std::decay_t<decltype(vv)>, std::string_view>)
                featureId_.emplace_back(k, std::string(vv));
            else
                featureId_.emplace_back(k, vv);
        }, v);
    }
}

}  // namespace mapget
