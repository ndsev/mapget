#include "tiles-request-json.h"

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace mapget::detail
{
namespace
{

constexpr std::array<std::string_view, 6> SearchFieldNames = {
    "searchId",
    "refresh",
    "searchQuery",
    "searchScope",
    "withFields",
    "rewrite",
};

constexpr std::array<std::string_view, 4> LegacyOnlySearchFieldNames = {
    "searchId",
    "refresh",
    "searchQuery",
    "searchScope",
};

constexpr std::array<std::string_view, 4> RestSearchFieldNames = {
    "query",
    "scope",
    "withFields",
    "rewrite",
};

/** Detect whether a layer request/envelope uses any search-as-map fields. */
[[nodiscard]] bool hasAnySearchField(const nlohmann::json& requestJson)
{
    return std::any_of(SearchFieldNames.begin(), SearchFieldNames.end(), [&requestJson](std::string_view field) {
        return requestJson.contains(field);
    });
}

/** Detect interactive-only search fields which REST `/search` intentionally hides. */
[[nodiscard]] bool hasAnyLegacyOnlySearchField(const nlohmann::json& requestJson)
{
    return std::any_of(
        LegacyOnlySearchFieldNames.begin(),
        LegacyOnlySearchFieldNames.end(),
        [&requestJson](std::string_view field) {
            return requestJson.contains(field);
        });
}

/** Detect REST `/search` fields which are not valid in plain tile requests. */
[[nodiscard]] bool hasAnyRestSearchField(const nlohmann::json& requestJson)
{
    return std::any_of(RestSearchFieldNames.begin(), RestSearchFieldNames.end(), [&requestJson](std::string_view field) {
        return requestJson.contains(field);
    });
}

/** Convert search scope into the stable text token used by request-key hashing. */
[[nodiscard]] std::string searchScopeToString(FeatureLayerSearchScope scope)
{
    switch (scope) {
    case FeatureLayerSearchScope::Feature:
        return "feature";
    case FeatureLayerSearchScope::Attribute:
        return "attribute";
    case FeatureLayerSearchScope::Auto:
        return "auto";
    }
    return "feature";
}

/** Build a stable in-session key separating concurrent searches on the same source layer. */
[[nodiscard]] std::string makeSearchRequestKey(FeatureLayerSearchRequest const& search)
{
    std::ostringstream fingerprint;
    fingerprint << search.searchId_ << '\n';
    if (search.refresh_) {
        fingerprint << "refresh:" << *search.refresh_;
        return fingerprint.str();
    }

    fingerprint << searchScopeToString(search.scope_) << '\n';
    fingerprint << (search.rewriteQuery_ ? "rewrite\n" : "plain\n");
    fingerprint << search.query_ << '\n';
    for (auto const& field : search.withFields_) {
        fingerprint << field << '\n';
    }
    return search.searchId_ + ":" + std::to_string(std::hash<std::string>{}(fingerprint.str()));
}

/** Parse search scope, defaulting to feature scope for backwards compatibility. */
[[nodiscard]] FeatureLayerSearchScope parseSearchScopeField(
    const nlohmann::json& requestJson,
    std::string_view key)
{
    auto scope = requestJson.value(std::string(key), std::string("feature"));
    if (scope == "feature") {
        return FeatureLayerSearchScope::Feature;
    }
    if (scope == "attribute") {
        return FeatureLayerSearchScope::Attribute;
    }
    if (scope == "auto") {
        return FeatureLayerSearchScope::Auto;
    }
    throw std::runtime_error(std::string(key) + " must be 'feature', 'attribute', or 'auto'");
}

/** Parse optional schema rewrite flag from either search request shape. */
void parseRewriteField(const nlohmann::json& requestJson, FeatureLayerSearchRequest& search)
{
    auto rewriteIt = requestJson.find("rewrite");
    if (rewriteIt == requestJson.end()) {
        search.rewriteQuery_ = search.scope_ == FeatureLayerSearchScope::Auto;
        return;
    }
    if (!rewriteIt->is_boolean()) {
        throw std::runtime_error("rewrite must be a boolean");
    }
    search.rewriteQuery_ = rewriteIt->get<bool>() || search.scope_ == FeatureLayerSearchScope::Auto;
}

/** Parse optional withFields expression array from either search request shape. */
void parseWithFields(const nlohmann::json& requestJson, FeatureLayerSearchRequest& search)
{
    auto withFieldsIt = requestJson.find("withFields");
    if (withFieldsIt == requestJson.end()) {
        return;
    }
    if (!withFieldsIt->is_array()) {
        throw std::runtime_error("withFields must be an array");
    }
    search.withFields_.reserve(withFieldsIt->size());
    for (auto const& fieldJson : *withFieldsIt) {
        if (!fieldJson.is_string()) {
            throw std::runtime_error("withFields entries must be strings");
        }
        search.withFields_.push_back(fieldJson.get<std::string>());
    }
}

/** Parse the common non-staged source request fields used by REST search. */
void parsePlainTileIdsInto(ParsedLayerTilesRequest& result, const nlohmann::json& requestJson)
{
    if (requestJson.contains("tileIdsByNextStage")) {
        throw std::runtime_error("search requests must use tileIds; tileIdsByNextStage is not supported");
    }

    auto const& tileIdsJson = requestJson.at("tileIds");
    if (!tileIdsJson.is_array()) {
        throw std::runtime_error("tileIds must be an array");
    }

    std::vector<TileId> tileIds;
    tileIds.reserve(tileIdsJson.size());
    for (auto const& tileIdJson : tileIdsJson) {
        tileIds.emplace_back(TileId::fromValue(tileIdJson.get<int32_t>()));
    }
    result.tileIdsByNextStage.push_back(std::move(tileIds));
}

/** Parse optional search-as-map fields from the shared layer request JSON shape. */
[[nodiscard]] std::optional<FeatureLayerSearchRequest> parseSearchRequestJson(const nlohmann::json& requestJson)
{
    if (!hasAnySearchField(requestJson)) {
        return std::nullopt;
    }
    if (!requestJson.contains("searchQuery") || !requestJson.at("searchQuery").is_string()) {
        throw std::runtime_error("searchQuery must be a string when search fields are present");
    }
    if (!requestJson.contains("searchId") || !requestJson.at("searchId").is_string()) {
        throw std::runtime_error("searchId must be a string when search fields are present");
    }

    FeatureLayerSearchRequest search;
    search.searchId_ = requestJson.at("searchId").get<std::string>();
    search.query_ = requestJson.at("searchQuery").get<std::string>();
    search.scope_ = parseSearchScopeField(requestJson, "searchScope");
    parseRewriteField(requestJson, search);

    if (auto refreshIt = requestJson.find("refresh"); refreshIt != requestJson.end()) {
        if (!(refreshIt->is_number_integer() || refreshIt->is_number_unsigned())) {
            throw std::runtime_error("refresh must be an integer");
        }
        search.refresh_ = refreshIt->get<int64_t>();
    }

    parseWithFields(requestJson, search);
    search.requestKey_ = makeSearchRequestKey(search);
    return search;
}

}  // namespace

bool containsInteractiveSearchFields(const nlohmann::json& requestJson)
{
    return requestJson.is_object() && hasAnySearchField(requestJson);
}

bool containsRestSearchFields(const nlohmann::json& requestJson)
{
    return requestJson.is_object() && hasAnyRestSearchField(requestJson);
}

/** Copy envelope-level search parameters into a layer request when omitted locally. */
void inheritSearchFields(nlohmann::json& requestJson, const nlohmann::json& envelopeJson)
{
    for (auto const field : SearchFieldNames) {
        if (!requestJson.contains(field) && envelopeJson.contains(field)) {
            requestJson[std::string(field)] = envelopeJson.at(field);
        }
    }
}

/** Parse the common HTTP/WS layer-tile request shape into the service request model. */
ParsedLayerTilesRequest parseLayerTilesRequestJson(const nlohmann::json& requestJson)
{
    ParsedLayerTilesRequest result;
    result.mapId = requestJson.at("mapId").get<std::string>();
    result.layerId = requestJson.at("layerId").get<std::string>();
    result.searchRequest = parseSearchRequestJson(requestJson);

    if (auto priorityIt = requestJson.find("priorityTileIds");
        priorityIt != requestJson.end())
    {
        if (!priorityIt->is_array()) {
            throw std::runtime_error("priorityTileIds must be an array");
        }
        result.priorityTileIds.reserve(priorityIt->size());
        for (auto const& tileIdJson : *priorityIt) {
            result.priorityTileIds.emplace_back(TileId::fromValue(tileIdJson.get<int32_t>()));
        }
    }

    if (auto stagedIt = requestJson.find("tileIdsByNextStage");
        stagedIt != requestJson.end())
    {
        if (result.searchRequest) {
            throw std::runtime_error("search requests must use tileIds; tileIdsByNextStage is not supported");
        }
        // Staged requests use bucket index as the first missing stage for each tile.
        result.usesStageBuckets = true;
        if (!stagedIt->is_array()) {
            throw std::runtime_error("tileIdsByNextStage must be an array");
        }
        result.tileIdsByNextStage.reserve(stagedIt->size());
        for (auto const& bucketJson : *stagedIt) {
            if (!bucketJson.is_array()) {
                throw std::runtime_error("tileIdsByNextStage entries must be arrays");
            }
            std::vector<TileId> bucket;
            bucket.reserve(bucketJson.size());
            for (auto const& tileIdJson : bucketJson) {
                bucket.emplace_back(TileId::fromValue(tileIdJson.get<int32_t>()));
            }
            result.tileIdsByNextStage.push_back(std::move(bucket));
        }
        return result;
    }

    parsePlainTileIdsInto(result, requestJson);
    return result;
}

FeatureLayerSearchRequest parseRestSearchEnvelopeJson(const nlohmann::json& envelopeJson)
{
    if (!envelopeJson.contains("query") || !envelopeJson.at("query").is_string()) {
        throw std::runtime_error("query must be a string");
    }
    if (hasAnyLegacyOnlySearchField(envelopeJson)) {
        throw std::runtime_error("REST /search uses query/scope; searchId/searchQuery/refresh are WebSocket-only fields");
    }

    FeatureLayerSearchRequest search;
    search.query_ = envelopeJson.at("query").get<std::string>();
    search.scope_ = parseSearchScopeField(envelopeJson, "scope");
    parseRewriteField(envelopeJson, search);
    parseWithFields(envelopeJson, search);
    return search;
}

ParsedLayerTilesRequest parseRestSearchLayerRequestJson(
    const nlohmann::json& requestJson,
    const FeatureLayerSearchRequest& searchTemplate)
{
    if (containsInteractiveSearchFields(requestJson) || containsRestSearchFields(requestJson)) {
        throw std::runtime_error("REST /search query/scope/withFields must be specified on the request envelope");
    }

    ParsedLayerTilesRequest result;
    result.mapId = requestJson.at("mapId").get<std::string>();
    result.layerId = requestJson.at("layerId").get<std::string>();
    result.searchRequest = searchTemplate;

    if (auto priorityIt = requestJson.find("priorityTileIds");
        priorityIt != requestJson.end())
    {
        if (!priorityIt->is_array()) {
            throw std::runtime_error("priorityTileIds must be an array");
        }
        result.priorityTileIds.reserve(priorityIt->size());
        for (auto const& tileIdJson : *priorityIt) {
            result.priorityTileIds.emplace_back(TileId::fromValue(tileIdJson.get<int32_t>()));
        }
    }

    parsePlainTileIdsInto(result, requestJson);
    return result;
}

/** Parse client-known string-pool offsets used to suppress already-seen string data. */
TileLayerStream::StringPoolOffsetMap parseStringPoolOffsetsJson(const nlohmann::json& offsetsJson)
{
    if (!offsetsJson.is_object()) {
        throw std::runtime_error("stringPoolOffsets must be an object");
    }

    TileLayerStream::StringPoolOffsetMap result;
    for (auto const& item : offsetsJson.items()) {
        result[item.key()] = item.value().get<simfil::StringId>();
    }
    return result;
}

/** Deduplicate all tile ids from staged/unstaged buckets while preserving first-seen order. */
std::vector<TileId> collectSearchTileIds(const ParsedLayerTilesRequest& request)
{
    std::set<TileId> seen;
    std::vector<TileId> result;
    for (auto const& bucket : request.tileIdsByNextStage) {
        for (auto const& tileId : bucket) {
            if (seen.insert(tileId).second) {
                result.push_back(tileId);
            }
        }
    }
    return result;
}

/** Expand parsed tile ids into concrete tile-stage keys in the order they should be fetched. */
std::vector<MapTileKey> expandLayerTilesRequestKeys(
    const ParsedLayerTilesRequest& request,
    LayerType layerType,
    uint32_t stageCount)
{
    std::vector<MapTileKey> result;
    std::set<MapTileKey> seen;
    const std::set<TileId> priorityTileIds(
        request.priorityTileIds.begin(),
        request.priorityTileIds.end());
    const auto isPriorityTile = [&priorityTileIds](TileId const& tileId) {
        return priorityTileIds.find(tileId) != priorityTileIds.end();
    };

    if (!request.usesStageBuckets) {
        const auto appendUnstagedTiles = [&](std::optional<bool> priorityFilter) {
            if (request.tileIdsByNextStage.empty()) {
                return;
            }
            for (auto const& tileId : request.tileIdsByNextStage.front()) {
                if (priorityFilter && isPriorityTile(tileId) != *priorityFilter) {
                    continue;
                }
                MapTileKey key(
                    layerType,
                    request.mapId,
                    request.layerId,
                    tileId,
                    UnspecifiedStage);
                if (seen.insert(key).second) {
                    result.push_back(std::move(key));
                }
            }
        };

        if (priorityTileIds.empty()) {
            appendUnstagedTiles(std::nullopt);
        } else {
            appendUnstagedTiles(true);
            appendUnstagedTiles(false);
        }
        return result;
    }

    auto const normalizedStageCount = std::max<uint32_t>(1U, stageCount);
    const auto appendStagedTiles = [&](std::optional<bool> priorityFilter) {
        for (uint32_t stage = 0; stage < normalizedStageCount; ++stage) {
            for (size_t bucketIndex = 0; bucketIndex < request.tileIdsByNextStage.size(); ++bucketIndex) {
                auto const nextMissingStage = static_cast<uint32_t>(bucketIndex);
                if (nextMissingStage > stage || nextMissingStage >= normalizedStageCount) {
                    continue;
                }
                for (auto const& tileId : request.tileIdsByNextStage[bucketIndex]) {
                    if (priorityFilter && isPriorityTile(tileId) != *priorityFilter) {
                        continue;
                    }
                    MapTileKey key(layerType, request.mapId, request.layerId, tileId, stage);
                    if (seen.insert(key).second) {
                        result.push_back(std::move(key));
                    }
                }
            }
        }
    };

    if (priorityTileIds.empty()) {
        appendStagedTiles(std::nullopt);
    } else {
        appendStagedTiles(true);
        appendStagedTiles(false);
    }

    return result;
}

}  // namespace mapget::detail
