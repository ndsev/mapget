#pragma once

#pragma once

#include "cache.h"

namespace mapget
{

/**
 * Class which models a request object that is passed into the
 * Service::Locate function.
 */
class LocateRequest
{
public:
    explicit LocateRequest(nlohmann::json const& j);
    LocateRequest(std::string mapId, std::string typeId, KeyValuePairs featureId);
    virtual ~LocateRequest() = default;

    std::string mapId_;
    std::string typeId_;
    KeyValuePairs featureId_;

    void setFeatureId(KeyValueViewPairs const& kvp);

    [[nodiscard]] std::optional<int64_t> getIntIdPart(std::string_view const& idPart) const;
    [[nodiscard]] std::optional<std::string_view> getStrIdPart(std::string_view const& idPart) const;

    [[nodiscard]] virtual nlohmann::json serialize() const;
};

/**
 * Class which models a response object that is returned from
 * the Service::Locate function.
 */
class LocateResponse : public LocateRequest
{
public:
    explicit LocateResponse(nlohmann::json const& j);
    LocateResponse(LocateResponse const& resp) = default;
    explicit LocateResponse(LocateRequest const& req);

    MapTileKey tileKey_;

    [[nodiscard]] nlohmann::json serialize() const override;
};

}
