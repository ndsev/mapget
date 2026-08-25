#include "locate.h"

#include <stdexcept>

namespace mapget
{
namespace
{

FeatureLayerFilterBinding parseBinding(nlohmann::json const& value)
{
    if (value.is_null()) {
        return std::monostate{};
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<int64_t>();
    }
    if (value.is_number_float()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    throw std::invalid_argument(
        "Locate selector bindings must be null, boolean, numeric, or string scalars.");
}

nlohmann::json bindingToJson(FeatureLayerFilterBinding const& binding)
{
    return std::visit(
        [](auto const& value) -> nlohmann::json
        {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::monostate>) {
                return nullptr;
            }
            else {
                return value;
            }
        },
        binding);
}

void validateSelector(FeatureLayerSelector const& selector)
{
    if (selector.canonicalFeatureId_) {
        if (selector.canonicalFeatureId_->empty()) {
            throw std::invalid_argument("Locate selector canonicalFeatureId must not be empty.");
        }
        if (!selector.typeId_.empty() || selector.featureFilter_ ||
            selector.featureIdExpression_ || !selector.bindings_.empty())
        {
            throw std::invalid_argument(
                "An exact locate selector cannot also contain typeId, featureFilter, "
                "featureIdExpression, or bindings.");
        }
        return;
    }
    auto const hasFilter = selector.featureFilter_ && !selector.featureFilter_->empty();
    auto const hasIdExpression =
        selector.featureIdExpression_ && !selector.featureIdExpression_->empty();
    if (selector.typeId_.empty() || hasFilter == hasIdExpression) {
        throw std::invalid_argument(
            "A locate selector requires non-empty typeId and exactly one of featureFilter or "
            "featureIdExpression.");
    }
}

}  // namespace

LocateRequest::LocateRequest(const nlohmann::json& j)
{
    if (j.contains("mapId"))
        mapId_ = j["mapId"].get<std::string>();
    if (j.contains("typeId"))
        typeId_ = j["typeId"].get<std::string>();
    if (j.contains("featureId")) {
        auto featureIdParts = j["featureId"];
        if (featureIdParts.is_string()) {
            canonicalFeatureId_ = featureIdParts.get<std::string>();
            return;
        }
        if (!featureIdParts.is_array() || featureIdParts.size() % 2 != 0) {
            throw std::invalid_argument(
                "LocateRequest featureId must be a canonical string or an even key/value array.");
        }
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

LocateRequest::LocateRequest(std::string mapId, std::string typeId, KeyValuePairs featureId)
    : mapId_(std::move(mapId)), typeId_(std::move(typeId)), featureId_(std::move(featureId))
{
}

nlohmann::json LocateRequest::serialize() const
{
    if (canonicalFeatureId_) {
        return nlohmann::json::object({
            {"mapId", mapId_},
            {"featureId", *canonicalFeatureId_},
        });
    }
    nlohmann::json featureId = nlohmann::json::array();
    for (auto const& [k, v] : featureId_) {
        featureId.emplace_back(k);
        std::visit([&featureId](auto&& vv) { featureId.emplace_back(vv); }, v);
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
    if (j.contains("canonicalFeatureId")) {
        resolvedCanonicalFeatureId_ = j["canonicalFeatureId"].get<std::string>();
    }
}

nlohmann::json LocateResponse::serialize() const
{
    auto result = LocateRequest::serialize();
    result["tileId"] = tileKey_.toString();
    if (resolvedCanonicalFeatureId_) {
        result["canonicalFeatureId"] = *resolvedCanonicalFeatureId_;
    }
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
        std::visit(
            [this, &k](auto&& vv)
            {
                if constexpr (std::is_same_v<std::decay_t<decltype(vv)>, std::string_view>)
                    featureId_.emplace_back(k, std::string(vv));
                else
                    featureId_.emplace_back(k, vv);
            },
            v);
    }
}

LocateCandidate::LocateCandidate(nlohmann::json const& j)
{
    if (!j.contains("tileId") || !j.at("tileId").is_string() || !j.contains("selector") ||
        !j.at("selector").is_object())
    {
        throw std::invalid_argument("LocateCandidate requires tileId and selector.");
    }
    tileKey_ = MapTileKey(j.at("tileId").get<std::string>());
    auto const& selector = j.at("selector");
    if (auto exact = selector.find("canonicalFeatureId"); exact != selector.end()) {
        if (!exact->is_string()) {
            throw std::invalid_argument("Locate selector canonicalFeatureId must be a string.");
        }
        selector_.canonicalFeatureId_ = exact->get<std::string>();
    }
    if (auto type = selector.find("typeId"); type != selector.end()) {
        if (!type->is_string()) {
            throw std::invalid_argument("Locate selector typeId must be a string.");
        }
        selector_.typeId_ = type->get<std::string>();
    }
    if (auto filter = selector.find("featureFilter"); filter != selector.end()) {
        if (!filter->is_string()) {
            throw std::invalid_argument("Locate selector featureFilter must be a string.");
        }
        selector_.featureFilter_ = filter->get<std::string>();
    }
    if (auto expression = selector.find("featureIdExpression"); expression != selector.end()) {
        if (!expression->is_string()) {
            throw std::invalid_argument("Locate selector featureIdExpression must be a string.");
        }
        selector_.featureIdExpression_ = expression->get<std::string>();
    }
    if (auto bindings = selector.find("bindings"); bindings != selector.end()) {
        if (!bindings->is_object()) {
            throw std::invalid_argument("Locate selector bindings must be an object.");
        }
        for (auto const& item : bindings->items()) {
            selector_.bindings_.emplace(item.key(), parseBinding(item.value()));
        }
    }
    validateSelector(selector_);
}

LocateCandidate::LocateCandidate(MapTileKey tileKey, FeatureLayerSelector selector)
    : tileKey_(std::move(tileKey)), selector_(std::move(selector))
{
    validateSelector(selector_);
}

LocateCandidate::LocateCandidate(MapTileKey tileKey, std::string canonicalFeatureId)
    : LocateCandidate(
          std::move(tileKey),
          FeatureLayerSelector{.canonicalFeatureId_ = std::move(canonicalFeatureId)})
{
}

LocateCandidate::LocateCandidate(
    MapTileKey tileKey,
    std::string typeId,
    std::string featureFilter,
    std::map<std::string, FeatureLayerFilterBinding> bindings)
    : LocateCandidate(
          std::move(tileKey),
          FeatureLayerSelector{
              .typeId_ = std::move(typeId),
              .featureFilter_ = std::move(featureFilter),
              .bindings_ = std::move(bindings)})
{
}

LocateCandidate LocateCandidate::fromFeatureIdExpression(
    MapTileKey tileKey,
    std::string typeId,
    std::string featureIdExpression,
    std::map<std::string, FeatureLayerFilterBinding> bindings)
{
    return LocateCandidate(
        std::move(tileKey),
        FeatureLayerSelector{
            .typeId_ = std::move(typeId),
            .featureIdExpression_ = std::move(featureIdExpression),
            .bindings_ = std::move(bindings),
        });
}

nlohmann::json LocateCandidate::serialize() const
{
    auto selector = nlohmann::json::object();
    if (selector_.canonicalFeatureId_) {
        selector["canonicalFeatureId"] = *selector_.canonicalFeatureId_;
    }
    else {
        selector["typeId"] = selector_.typeId_;
        if (selector_.featureFilter_) {
            selector["featureFilter"] = *selector_.featureFilter_;
        }
        else {
            selector["featureIdExpression"] = *selector_.featureIdExpression_;
        }
        if (!selector_.bindings_.empty()) {
            selector["bindings"] = nlohmann::json::object();
            for (auto const& [name, binding] : selector_.bindings_) {
                selector["bindings"][name] = bindingToJson(binding);
            }
        }
    }
    return nlohmann::json::object({
        {"tileId", tileKey_.toString()},
        {"selector", std::move(selector)},
    });
}

tl::expected<std::vector<model_ptr<Feature>>, simfil::Error>
resolveLocateCandidate(
    LocateCandidate const& candidate,
    TileFeatureLayer const& tile,
    FeatureLayerFilterCancellationCheck const& cancellationCheck)
{
    if (candidate.tileKey_ != tile.id()) {
        return tl::unexpected(simfil::Error{
            simfil::Error::InvalidArguments,
            "Locate candidate was applied to a different tile.",
        });
    }
    return tile.find(candidate.selector_, cancellationCheck);
}

}  // namespace mapget
