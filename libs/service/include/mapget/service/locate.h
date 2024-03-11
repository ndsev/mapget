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
    virtual ~LocateRequest() = default;

    std::string mapId_;
    std::string typeId_;
    KeyValuePairs featureId_;

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
    LocateResponse(LocateRequest const& req);

    MapTileKey tileKey_;

    [[nodiscard]] nlohmann::json serialize() const override;
};

}
