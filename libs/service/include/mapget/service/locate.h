#pragma once

#include "cache.h"
#include "mapget/model/featurelayer-filter.h"

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
    /** Canonical FeatureId::toString() form resolved by Service before datasource dispatch. */
    std::optional<std::string> canonicalFeatureId_;

    void setFeatureId(KeyValueViewPairs const& kvp);

    [[nodiscard]] std::optional<int64_t> getIntIdPart(std::string_view const& idPart) const;
    [[nodiscard]] std::optional<std::string_view> getStrIdPart(std::string_view const& idPart) const;

    [[nodiscard]] virtual nlohmann::json serialize() const;
};

/**
 * Cheap datasource-produced candidate for a locate request.
 *
 * The datasource identifies a possible tile and supplies a portable selector
 * which mapget applies only after that tile has been loaded through the
 * ordinary service/cache path. Producing a candidate must not fetch, fill, or
 * convert tile data.
 */
class LocateCandidate
{
public:
    explicit LocateCandidate(nlohmann::json const& j);
    LocateCandidate(
        MapTileKey tileKey,
        FeatureLayerSelector selector);
    LocateCandidate(
        MapTileKey tileKey,
        std::string canonicalFeatureId);
    LocateCandidate(
        MapTileKey tileKey,
        std::string typeId,
        std::string featureFilter,
        std::map<
            std::string,
            FeatureLayerFilterBinding> bindings = {});

    MapTileKey tileKey_;
    FeatureLayerSelector selector_;

    [[nodiscard]] nlohmann::json serialize() const;
};

/**
 * Canonical response returned by the complete Service::locate operation.
 */
class LocateResponse : public LocateRequest
{
public:
    explicit LocateResponse(nlohmann::json const& j);
    LocateResponse(LocateResponse const& resp) = default;
    explicit LocateResponse(LocateRequest const& req);

    MapTileKey tileKey_;
    std::optional<std::string> resolvedCanonicalFeatureId_;

    [[nodiscard]] nlohmann::json serialize() const override;
};

/** Apply a datasource candidate to an already loaded complete feature tile. */
tl::expected<std::vector<model_ptr<Feature>>, simfil::Error>
resolveLocateCandidate(
    LocateCandidate const& candidate,
    TileFeatureLayer const& tile);

}
