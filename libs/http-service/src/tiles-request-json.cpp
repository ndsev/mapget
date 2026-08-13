#include "tiles-request-json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mapget::detail
{
namespace
{

constexpr std::array<std::string_view, 6> FilterFieldNames = {
    "filterId",
    "generation",
    "deliveryEpoch",
    "deliveryEpochs",
    "channels",
    "bindings",
};

bool isMetadataSourceDataLayer(std::string_view layerId)
{
    return layerId.starts_with("Metadata-");
}

TileId parseRequestTileId(
    nlohmann::json const& tileIdJson,
    std::string_view layerId)
{
    if (!tileIdJson.is_number_integer() &&
        !tileIdJson.is_number_unsigned())
    {
        throw std::runtime_error(
            "tile IDs must be signed 32-bit integers");
    }
    int64_t rawValue = 0;
    if (tileIdJson.is_number_unsigned()) {
        auto const unsignedValue =
            tileIdJson.get<uint64_t>();
        if (unsignedValue >
            static_cast<uint64_t>(
                std::numeric_limits<int32_t>::max()))
        {
            throw std::runtime_error(
                "tile IDs must be signed 32-bit integers");
        }
        rawValue = static_cast<int64_t>(
            unsignedValue);
    }
    else {
        rawValue = tileIdJson.get<int64_t>();
    }
    if (rawValue <
            std::numeric_limits<int32_t>::min() ||
        rawValue >
            std::numeric_limits<int32_t>::max())
    {
        throw std::runtime_error(
            "tile IDs must be signed 32-bit integers");
    }
    auto const rawTileId =
        static_cast<int32_t>(rawValue);
    if (rawTileId == 0 && isMetadataSourceDataLayer(layerId)) {
        return TileId();
    }
    return TileId::fromValue(rawTileId);
}

std::string requireString(
    nlohmann::json const& object,
    std::string_view key,
    bool allowEmpty = false)
{
    auto const keyString = std::string(key);
    auto found = object.find(keyString);
    if (found == object.end() || !found->is_string()) {
        throw std::runtime_error(keyString + " must be a string");
    }
    auto result = found->get<std::string>();
    if (!allowEmpty && result.empty()) {
        throw std::runtime_error(keyString + " must not be empty");
    }
    return result;
}

std::optional<std::string> optionalString(
    nlohmann::json const& object,
    std::string_view key,
    bool allowEmpty = false)
{
    auto found = object.find(std::string(key));
    if (found == object.end()) {
        return std::nullopt;
    }
    if (!found->is_string()) {
        throw std::runtime_error(std::string(key) + " must be a string");
    }
    auto result = found->get<std::string>();
    if (!allowEmpty && result.empty()) {
        throw std::runtime_error(std::string(key) + " must not be empty");
    }
    return result;
}

std::vector<std::string> stringArray(
    nlohmann::json const& object,
    std::string_view key)
{
    auto found = object.find(std::string(key));
    if (found == object.end()) {
        return {};
    }
    if (!found->is_array()) {
        throw std::runtime_error(std::string(key) + " must be an array");
    }
    std::vector<std::string> result;
    result.reserve(found->size());
    for (auto const& value : *found) {
        if (!value.is_string()) {
            throw std::runtime_error(
                std::string(key) + " entries must be strings");
        }
        auto item = value.get<std::string>();
        if (item.empty()) {
            throw std::runtime_error(
                std::string(key) + " entries must not be empty");
        }
        result.push_back(std::move(item));
    }
    return result;
}

FeatureLayerFilterScope parseScope(nlohmann::json const& channel)
{
    auto found = channel.find("scope");
    if (found == channel.end()) {
        return FeatureLayerFilterScope::Feature;
    }
    if (!found->is_string()) {
        throw std::runtime_error(
            "channel.scope must be 'feature', 'attribute', 'relation', or 'auto'");
    }
    auto const value = found->get<std::string>();
    if (value == "feature") return FeatureLayerFilterScope::Feature;
    if (value == "attribute") return FeatureLayerFilterScope::Attribute;
    if (value == "relation") return FeatureLayerFilterScope::Relation;
    if (value == "auto") return FeatureLayerFilterScope::Auto;
    throw std::runtime_error(
        "channel.scope must be 'feature', 'attribute', 'relation', or 'auto'");
}

glm::dvec3 parseVector3(
    nlohmann::json const& object,
    std::string_view key,
    glm::dvec3 defaultValue,
    bool required)
{
    auto found = object.find(std::string(key));
    if (found == object.end()) {
        if (required) {
            throw std::runtime_error(
                std::string(key) + " is required");
        }
        return defaultValue;
    }
    if (!found->is_array() || found->size() != 3) {
        throw std::runtime_error(
            std::string(key) + " must be a three-number array");
    }
    glm::dvec3 result;
    for (size_t index = 0; index < 3; ++index) {
        if (!found->at(index).is_number()) {
            throw std::runtime_error(
                std::string(key) + " must be a three-number array");
        }
        result[index] = found->at(index).get<double>();
        if (!std::isfinite(result[index])) {
            throw std::runtime_error(
                std::string(key) + " components must be finite");
        }
    }
    return result;
}

FeatureLayerPointGridGroup parseGroup(nlohmann::json const& group)
{
    if (!group.is_object()) {
        throw std::runtime_error("channel.group must be an object");
    }
    auto const kind = requireString(group, "kind");
    if (kind != "point-grid") {
        throw std::runtime_error(
            "channel.group.kind must be 'point-grid'");
    }
    return FeatureLayerPointGridGroup{
        .origin_ = parseVector3(
            group,
            "origin",
            glm::dvec3{0.0},
            false),
        .cellSize_ = parseVector3(
            group,
            "cellSize",
            glm::dvec3{1.0},
            true),
    };
}

FeatureLayerStoredRelationOptions parseRelation(
    nlohmann::json const& relation)
{
    if (!relation.is_object()) {
        throw std::runtime_error("channel.relation must be an object");
    }
    FeatureLayerStoredRelationOptions result;
    result.relationNamePattern_ =
        optionalString(relation, "namePattern");
    if (auto found = relation.find("recursive");
        found != relation.end())
    {
        if (!found->is_boolean()) {
            throw std::runtime_error(
                "channel.relation.recursive must be a boolean");
        }
        result.recursive_ = found->get<bool>();
    }
    if (auto found = relation.find("mergeTwoway");
        found != relation.end())
    {
        if (!found->is_boolean()) {
            throw std::runtime_error(
                "channel.relation.mergeTwoway must be a boolean");
        }
        result.mergeTwoway_ = found->get<bool>();
    }
    return result;
}

FeatureLayerFilterChannel parseChannel(
    nlohmann::json const& channelJson)
{
    if (!channelJson.is_object()) {
        throw std::runtime_error("channels entries must be objects");
    }
    FeatureLayerFilterChannel channel;
    channel.channelId_ =
        requireString(channelJson, "channelId");
    channel.featureFilter_ =
        optionalString(channelJson, "featureFilter", true);
    channel.entryFilter_ =
        optionalString(channelJson, "entryFilter", true);
    channel.scope_ = parseScope(channelJson);
    channel.featureTypes_ =
        stringArray(channelJson, "featureTypes");
    channel.featureFields_ =
        stringArray(channelJson, "featureFields");
    channel.entryFields_ =
        stringArray(channelJson, "entryFields");

    if (auto found = channelJson.find("rewrite");
        found != channelJson.end())
    {
        if (!found->is_boolean()) {
            throw std::runtime_error(
                "channel.rewrite must be a boolean");
        }
        channel.rewrite_ = found->get<bool>();
    }
    if (auto found = channelJson.find("geometryTypes");
        found != channelJson.end())
    {
        if (!found->is_number_unsigned() &&
            !found->is_number_integer())
        {
            throw std::runtime_error(
                "channel.geometryTypes must be an unsigned 32-bit integer");
        }
        auto const value = found->get<int64_t>();
        if (value < 0 ||
            static_cast<uint64_t>(value) >
                std::numeric_limits<uint32_t>::max())
        {
            throw std::runtime_error(
                "channel.geometryTypes must be an unsigned 32-bit integer");
        }
        channel.geometryTypes_ = static_cast<uint32_t>(value);
    }
    if (auto geometryName =
            optionalString(channelJson, "geometryName");
        geometryName)
    {
        if (*geometryName != "*") {
            channel.geometryName_ = std::move(*geometryName);
        }
    }
    if (auto found = channelJson.find("group");
        found != channelJson.end())
    {
        channel.group_ = parseGroup(*found);
    }
    if (auto found = channelJson.find("relation");
        found != channelJson.end())
    {
        channel.relation_ = parseRelation(*found);
    }
    return channel;
}

FeatureLayerFilterBinding parseBinding(
    nlohmann::json const& value,
    std::string_view name)
{
    if (value.is_null()) return std::monostate{};
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number_integer()) return value.get<int64_t>();
    if (value.is_number_unsigned()) {
        auto const raw = value.get<uint64_t>();
        if (raw >
            static_cast<uint64_t>(
                std::numeric_limits<int64_t>::max()))
        {
            throw std::runtime_error(
                "binding '" + std::string(name) +
                "' exceeds signed 64-bit range");
        }
        return static_cast<int64_t>(raw);
    }
    if (value.is_number_float()) {
        auto const number = value.get<double>();
        if (!std::isfinite(number)) {
            throw std::runtime_error(
                "binding '" + std::string(name) +
                "' must be finite");
        }
        return number;
    }
    if (value.is_string()) return value.get<std::string>();
    throw std::runtime_error(
        "binding '" + std::string(name) +
        "' must be null, boolean, integer, float, or string");
}

FeatureLayerFilterRequest parseFilterDefinition(
    nlohmann::json const& object,
    bool requireIdentity)
{
    FeatureLayerFilterRequest result;
    if (auto id = optionalString(object, "filterId", true)) {
        result.filterId_ = std::move(*id);
    }
    else if (requireIdentity) {
        throw std::runtime_error(
            "interactive filter requests require filterId");
    }

    if (auto generation = object.find("generation");
        generation != object.end())
    {
        if (!(generation->is_number_unsigned() ||
              generation->is_number_integer()))
        {
            throw std::runtime_error(
                "generation must be a non-negative integer");
        }
        if (generation->is_number_integer() &&
            generation->get<int64_t>() < 0)
        {
            throw std::runtime_error(
                "generation must be a non-negative integer");
        }
        result.generation_ = generation->get<uint64_t>();
    }
    else if (requireIdentity) {
        throw std::runtime_error(
            "interactive filter requests require generation");
    }

    if (auto deliveryEpoch = object.find("deliveryEpoch");
        deliveryEpoch != object.end())
    {
        if (!(deliveryEpoch->is_number_unsigned() ||
              deliveryEpoch->is_number_integer()) ||
            (deliveryEpoch->is_number_integer() &&
             deliveryEpoch->get<int64_t>() < 0))
        {
            throw std::runtime_error(
                "deliveryEpoch must be a non-negative integer");
        }
        result.deliveryEpoch_ = deliveryEpoch->get<uint64_t>();
    }

    auto channels = object.find("channels");
    if (channels == object.end() || !channels->is_array() ||
        channels->empty())
    {
        throw std::runtime_error(
            "channels must be a non-empty array");
    }
    result.channels_.reserve(channels->size());
    for (auto const& channel : *channels) {
        result.channels_.push_back(parseChannel(channel));
    }

    if (auto bindings = object.find("bindings");
        bindings != object.end())
    {
        if (!bindings->is_object()) {
            throw std::runtime_error("bindings must be an object");
        }
        for (auto const& item : bindings->items()) {
            if (item.key().empty()) {
                throw std::runtime_error(
                    "binding names must not be empty");
            }
            result.bindings_.emplace(
                item.key(),
                parseBinding(item.value(), item.key()));
        }
    }
    return result;
}

void parsePlainTileIdsInto(
    ParsedLayerTilesRequest& result,
    nlohmann::json const& requestJson,
    std::string_view layerId)
{
    if (requestJson.contains("tileIdsByNextStage")) {
        throw std::runtime_error(
            "tileIdsByNextStage is not supported; use tileIds");
    }
    auto tileIds = requestJson.find("tileIds");
    if (tileIds == requestJson.end() || !tileIds->is_array()) {
        throw std::runtime_error("tileIds must be an array");
    }
    result.tileIds.reserve(tileIds->size());
    for (auto const& tileId : *tileIds) {
        result.tileIds.emplace_back(
            parseRequestTileId(tileId, layerId));
    }
}

void parseDeliveryEpochsInto(
    ParsedLayerTilesRequest& result,
    nlohmann::json const& requestJson)
{
    auto overrides = requestJson.find("deliveryEpochs");
    if (overrides == requestJson.end()) {
        return;
    }
    if (!overrides->is_array()) {
        throw std::runtime_error("deliveryEpochs must be an array");
    }
    auto const requested = std::set<TileId>(
        result.tileIds.begin(),
        result.tileIds.end());
    for (auto const& overrideJson : *overrides) {
        if (!overrideJson.is_object() ||
            !overrideJson.contains("tileId") ||
            !overrideJson.contains("epoch"))
        {
            throw std::runtime_error(
                "deliveryEpochs entries require tileId and epoch");
        }
        auto const tileId = parseRequestTileId(
            overrideJson.at("tileId"),
            result.layerId);
        if (!requested.contains(tileId)) {
            throw std::runtime_error(
                "deliveryEpochs tileId values must be contained in tileIds");
        }
        auto const& epochJson = overrideJson.at("epoch");
        if (!(epochJson.is_number_unsigned() ||
              epochJson.is_number_integer()) ||
            (epochJson.is_number_integer() &&
             epochJson.get<int64_t>() < 0))
        {
            throw std::runtime_error(
                "deliveryEpochs epoch values must be non-negative integers");
        }
        if (!result.deliveryEpochs.emplace(
                tileId,
                epochJson.get<uint64_t>()).second)
        {
            throw std::runtime_error(
                "deliveryEpochs must not repeat tileId values");
        }
    }
}

void parseRequestBase(
    ParsedLayerTilesRequest& result,
    nlohmann::json const& requestJson)
{
    result.mapId = requireString(requestJson, "mapId");
    result.layerId = requireString(requestJson, "layerId");
    result.sourceId = optionalString(requestJson, "sourceId");
    parsePlainTileIdsInto(
        result,
        requestJson,
        result.layerId);
    parseDeliveryEpochsInto(result, requestJson);

    if (auto priorities = requestJson.find("priorityTileIds");
        priorities != requestJson.end())
    {
        if (!priorities->is_array()) {
            throw std::runtime_error(
                "priorityTileIds must be an array");
        }
        result.priorityTileIds.reserve(priorities->size());
        for (auto const& tileId : *priorities) {
            result.priorityTileIds.emplace_back(
                parseRequestTileId(tileId, result.layerId));
        }
        auto const requested = std::set<TileId>(
            result.tileIds.begin(),
            result.tileIds.end());
        if (std::ranges::any_of(
                result.priorityTileIds,
                [&](auto const& tileId) {
                    return !requested.contains(tileId);
                }))
        {
            throw std::runtime_error(
                "priorityTileIds must be contained in tileIds");
        }
    }

    if (auto restrictions = requestJson.find("featureIds");
        restrictions != requestJson.end())
    {
        if (!restrictions->is_array()) {
            throw std::runtime_error(
                "featureIds must be an array of tile/id groups");
        }
        size_t totalIds = 0;
        auto const requested = std::set<TileId>(
            result.tileIds.begin(),
            result.tileIds.end());
        for (auto const& restriction : *restrictions) {
            if (!restriction.is_object() ||
                !restriction.contains("tileId") ||
                !restriction.contains("ids") ||
                !restriction.at("ids").is_array())
            {
                throw std::runtime_error(
                    "featureIds entries require tileId and an ids array");
            }
            auto const tileId = parseRequestTileId(
                restriction.at("tileId"),
                result.layerId);
            if (!requested.contains(tileId)) {
                throw std::runtime_error(
                    "featureIds tileId values must be contained in tileIds");
            }
            auto& ids = result.featureIdsByTile[tileId];
            for (auto const& id : restriction.at("ids")) {
                if (!id.is_string() ||
                    id.get_ref<std::string const&>().empty())
                {
                    throw std::runtime_error(
                        "featureIds ids must be non-empty canonical strings");
                }
                auto value = id.get<std::string>();
                if (std::ranges::find(ids, value) == ids.end()) {
                    ids.push_back(std::move(value));
                    if (++totalIds > 4096) {
                        throw std::runtime_error(
                            "featureIds exceeds the 4096-feature request limit");
                    }
                }
            }
        }
    }

    if (auto roots = requestJson.find("roots");
        roots != requestJson.end())
    {
        if (!roots->is_array()) {
            throw std::runtime_error(
                "roots must be an array");
        }
        result.exactRoots.reserve(roots->size());
        for (size_t rootIndex = 0;
             rootIndex < roots->size();
             ++rootIndex)
        {
            auto const& root = roots->at(rootIndex);
            if (!root.is_object()) {
                throw std::runtime_error(
                    "roots entries must be objects");
            }
            auto tile = root.find("tileId");
            if (tile == root.end()) {
                throw std::runtime_error(
                    "roots entries require tileId");
            }
            auto featureId =
                root.find("featureId");
            if (featureId == root.end() ||
                (!featureId->is_string() &&
                 (!featureId->is_array() ||
                  featureId->size() % 2 != 0)))
            {
                throw std::runtime_error(
                    "roots featureId must be a canonical string or an alternating key/value array");
            }
            if (featureId->is_string()) {
                auto canonicalFeatureId =
                    featureId->get<std::string>();
                if (canonicalFeatureId.empty()) {
                    throw std::runtime_error(
                        "roots canonical featureId must not be empty");
                }
                result.exactRoots.push_back(
                    FeatureLayerFilterRoot{
                        parseRequestTileId(
                            *tile,
                            result.layerId),
                        {},
                        {},
                        rootIndex,
                        std::move(canonicalFeatureId),
                    });
                continue;
            }
            KeyValuePairs idParts;
            idParts.reserve(
                featureId->size() / 2);
            for (size_t index = 0;
                 index < featureId->size();
                 index += 2)
            {
                if (!featureId->at(index)
                         .is_string())
                {
                    throw std::runtime_error(
                        "roots featureId keys must be strings");
                }
                auto key =
                    featureId->at(index)
                        .get<std::string>();
                auto const& value =
                    featureId->at(index + 1);
                if (value.is_number_unsigned()) {
                    auto unsignedValue =
                        value.get<uint64_t>();
                    if (unsignedValue >
                        static_cast<uint64_t>(
                            std::numeric_limits<
                                int64_t>::max()))
                    {
                        throw std::runtime_error(
                            "roots featureId integer values exceed the signed 64-bit model domain");
                    }
                    idParts.emplace_back(
                        std::move(key),
                        static_cast<int64_t>(
                            unsignedValue));
                }
                else if (value.is_number_integer()) {
                    idParts.emplace_back(
                        std::move(key),
                        value.get<int64_t>());
                }
                else if (value.is_string()) {
                    idParts.emplace_back(
                        std::move(key),
                        value.get<std::string>());
                }
                else {
                    throw std::runtime_error(
                        "roots featureId values must be signed integers or strings");
                }
            }
            result.exactRoots.push_back(
                FeatureLayerFilterRoot{
                    parseRequestTileId(
                        *tile,
                        result.layerId),
                    requireString(
                        root,
                        "typeId"),
                    std::move(idParts),
                    rootIndex,
                });
        }
        auto const requested = std::set<TileId>(
            result.tileIds.begin(),
            result.tileIds.end());
        if (std::ranges::any_of(
                result.exactRoots,
                [&](auto const& root) {
                    return !requested.contains(
                        root.tileId_);
                }))
        {
            throw std::runtime_error(
                "roots tileId values must be contained in tileIds");
        }
    }
}

nlohmann::json bindingToJson(
    FeatureLayerFilterBinding const& binding)
{
    return std::visit(
        [](auto const& value) -> nlohmann::json {
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

std::string scopeToString(FeatureLayerFilterScope scope)
{
    switch (scope) {
    case FeatureLayerFilterScope::Feature: return "feature";
    case FeatureLayerFilterScope::Attribute: return "attribute";
    case FeatureLayerFilterScope::Relation: return "relation";
    case FeatureLayerFilterScope::Auto: return "auto";
    }
    return "feature";
}

} // namespace

void inheritFilterFields(
    nlohmann::json& requestJson,
    nlohmann::json const& envelopeJson)
{
    for (auto const field : FilterFieldNames) {
        if (!requestJson.contains(field) &&
            envelopeJson.contains(field))
        {
            requestJson[std::string(field)] =
                envelopeJson.at(field);
        }
    }
}

bool containsFilterFields(nlohmann::json const& requestJson)
{
    return requestJson.is_object() &&
        std::any_of(
            FilterFieldNames.begin(),
            FilterFieldNames.end(),
            [&](std::string_view field) {
                return requestJson.contains(field);
            });
}

ParsedLayerTilesRequest parseLayerTilesRequestJson(
    nlohmann::json const& requestJson)
{
    ParsedLayerTilesRequest result;
    parseRequestBase(result, requestJson);
    if (containsFilterFields(requestJson)) {
        if (!result.featureIdsByTile.empty()) {
            throw std::runtime_error(
                "featureIds is supported only by plain /tiles requests");
        }
        result.filterRequest =
            parseFilterDefinition(requestJson, true);
    }
    return result;
}

FeatureLayerFilterRequest parseRestFilterEnvelopeJson(
    nlohmann::json const& envelopeJson)
{
    return parseFilterDefinition(envelopeJson, false);
}

ParsedLayerTilesRequest parseRestFilterLayerRequestJson(
    nlohmann::json const& requestJson,
    FeatureLayerFilterRequest const& filterTemplate)
{
    if (containsFilterFields(requestJson)) {
        throw std::runtime_error(
            "REST /filter definition fields belong on the request envelope");
    }
    ParsedLayerTilesRequest result;
    parseRequestBase(result, requestJson);
    if (!result.featureIdsByTile.empty()) {
        throw std::runtime_error(
            "featureIds is supported only by plain /tiles requests");
    }
    result.filterRequest = filterTemplate;
    return result;
}

TileLayerStream::StringPoolOffsetMap parseStringPoolOffsetsJson(
    nlohmann::json const& offsetsJson)
{
    if (!offsetsJson.is_object()) {
        throw std::runtime_error(
            "stringPoolOffsets must be an object");
    }
    TileLayerStream::StringPoolOffsetMap result;
    for (auto const& item : offsetsJson.items()) {
        result[item.key()] =
            item.value().get<simfil::StringId>();
    }
    return result;
}

std::vector<TileId> collectFilterTileIds(
    ParsedLayerTilesRequest const& request)
{
    std::set<TileId> seen;
    std::vector<TileId> result;
    for (auto const& tileId : request.tileIds) {
        if (seen.insert(tileId).second) {
            result.push_back(tileId);
        }
    }
    return result;
}

std::vector<MapTileKey> expandLayerTilesRequestKeys(
    ParsedLayerTilesRequest const& request,
    LayerType layerType)
{
    std::vector<MapTileKey> result;
    std::set<MapTileKey> seen;
    std::set<TileId> priorityTileIds(
        request.priorityTileIds.begin(),
        request.priorityTileIds.end());
    auto isPriority = [&](TileId const& tileId) {
        return priorityTileIds.contains(tileId);
    };
    auto append = [&](std::optional<bool> priorityFilter) {
        for (auto const& tileId : request.tileIds) {
            if (priorityFilter &&
                isPriority(tileId) != *priorityFilter)
            {
                continue;
            }
            MapTileKey key(
                layerType,
                request.mapId,
                request.layerId,
                tileId);
            if (seen.insert(key).second) {
                result.push_back(std::move(key));
            }
        }
    };
    if (priorityTileIds.empty()) {
        append(std::nullopt);
    }
    else {
        append(true);
        append(false);
    }
    return result;
}

nlohmann::json filterRequestToJson(
    FeatureLayerFilterRequest const& request,
    bool includeIdentity)
{
    auto result = nlohmann::json::object();
    if (includeIdentity) {
        result["filterId"] = request.filterId_;
        result["generation"] = request.generation_;
        result["deliveryEpoch"] = request.deliveryEpoch_;
    }
    result["channels"] = nlohmann::json::array();
    for (auto const& channel : request.channels_) {
        auto json = nlohmann::json::object({
            {"channelId", channel.channelId_},
            {"scope", scopeToString(channel.scope_)},
            {"rewrite", channel.rewrite_},
            {"featureTypes", channel.featureTypes_},
            {"featureFields", channel.featureFields_},
            {"entryFields", channel.entryFields_},
            {"geometryTypes", channel.geometryTypes_},
            {"geometryName",
             channel.geometryName_
                 ? nlohmann::json(*channel.geometryName_)
                 : nlohmann::json("*")},
        });
        if (channel.featureFilter_) {
            json["featureFilter"] = *channel.featureFilter_;
        }
        if (channel.entryFilter_) {
            json["entryFilter"] = *channel.entryFilter_;
        }
        if (channel.group_) {
            json["group"] = {
                {"kind", "point-grid"},
                {"origin", {
                    channel.group_->origin_.x,
                    channel.group_->origin_.y,
                    channel.group_->origin_.z,
                }},
                {"cellSize", {
                    channel.group_->cellSize_.x,
                    channel.group_->cellSize_.y,
                    channel.group_->cellSize_.z,
                }},
            };
        }
        if (channel.relation_) {
            json["relation"] = {
                {"recursive", channel.relation_->recursive_},
                {"mergeTwoway", channel.relation_->mergeTwoway_},
            };
            if (channel.relation_->relationNamePattern_) {
                json["relation"]["namePattern"] =
                    *channel.relation_->relationNamePattern_;
            }
        }
        result["channels"].push_back(std::move(json));
    }
    result["bindings"] = nlohmann::json::object();
    for (auto const& [name, value] : request.bindings_) {
        result["bindings"][name] = bindingToJson(value);
    }
    return result;
}

} // namespace mapget::detail
