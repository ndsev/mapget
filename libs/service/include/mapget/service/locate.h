#pragma once

#include "cache.h"
#include "mapget/model/featurelayer-filter.h"

namespace mapget
{

/**
 * Identity lookup accepted by Service::locate. Named parts can be a complete
 * identity or provider-specific lookup keys; a canonical string is resolved
 * against the destination layer before datasource dispatch.
 */
class LocateRequest
{
public:
    /**
     * Read canonical featureId strings, legacy alternating key/value arrays, or
     * named {mapId, typeId, id: {part: value}} references. Invalid ID shapes throw.
     */
    explicit LocateRequest(nlohmann::json const& j);
    LocateRequest(std::string mapId, std::string typeId, KeyValuePairs featureId);

    virtual ~LocateRequest() = default;

    /** Optional owning-layer/partition context disambiguates tile and object feature IDs. */
    std::optional<std::string> layerId_;
    std::optional<PartitionId> partition_;
    std::string mapId_;
    std::string typeId_;
    KeyValuePairs featureId_;

    /** Canonical FeatureId::toString() form resolved by Service before datasource dispatch. */
    std::optional<std::string> canonicalFeatureId_;

    /** Copy borrowed name/value pairs into this request; canonicalFeatureId_ is unchanged. */
    void setFeatureId(KeyValueViewPairs const& kvp);

    /** Return a matching integer part; absent or differently typed parts return nullopt. */
    [[nodiscard]] std::optional<int64_t> getIntIdPart(std::string_view const& idPart) const;

    /** Borrow a matching string part; the view expires when featureId_ is modified or destroyed. */
    [[nodiscard]] std::optional<std::string_view>
    getStrIdPart(std::string_view const& idPart) const;

    /** Emit the canonical-string form when set, otherwise an alternating key/value array. */
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
    /** Parse partitionKey (legacy tileId also accepted) and validate the selector shape. */
    explicit LocateCandidate(nlohmann::json const& j);

    /** Require either an exact ID or one typed predicate/ID expression; invalid mixtures throw. */
    LocateCandidate(MapPartitionKey tileKey, FeatureLayerSelector selector);

    /** Select one exact canonical identity in the candidate partition. */
    LocateCandidate(MapPartitionKey tileKey, std::string canonicalFeatureId);

    /** Select matching features of typeId with scalar bindings evaluated in that partition. */
    LocateCandidate(
        MapPartitionKey tileKey,
        std::string typeId,
        std::string featureFilter,
        std::map<std::string, FeatureLayerFilterBinding> bindings = {});

    /**
     * Construct a selector which computes canonical IDs once per candidate tile.
     *
     * The expression may use `$features` and scalar bindings and must return
     * canonical feature-id strings or FeatureId nodes for `typeId`.
     */
    [[nodiscard]] static LocateCandidate fromFeatureIdExpression(
        MapPartitionKey tileKey,
        std::string typeId,
        std::string featureIdExpression,
        std::map<std::string, FeatureLayerFilterBinding> bindings = {});

    MapPartitionKey tileKey_;
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

    MapPartitionKey tileKey_;
    std::optional<std::string> resolvedCanonicalFeatureId_;

    [[nodiscard]] nlohmann::json serialize() const override;
};

/**
 * Evaluate a candidate in its exact loaded partition. A different partition key
 * returns InvalidArguments; selector compilation/evaluation errors propagate.
 */
tl::expected<std::vector<model_ptr<Feature>>, simfil::Error> resolveLocateCandidate(
    LocateCandidate const& candidate,
    PartitionFeatureLayer const& tile,
    FeatureLayerFilterCancellationCheck const& cancellationCheck = {});
}
