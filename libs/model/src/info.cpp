#include "info.h"
#include "stream.h"
#include "mapget/log.h"

#include <tuple>
#include <random>
#include <sstream>
#include <charconv>

namespace mapget
{

namespace {

/** Generates a random 16-Byte UUID. Used to generate random DataSourceInfo node IDs. */
std::string generateUuid() {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 4; ++i)
        ss << std::setw(4) << std::setfill('0') << (rng() & 0xFFFF);
    return ss.str();
}

auto missing_field(std::string const& error, std::string const& context) {
    return std::runtime_error(
        fmt::format("{}::fromJson(): `{}`", context, error));
}

template <class T, class... Args>
std::optional<T> from_chars(std::string_view s, Args... args)
{
    const char *end = s.begin() + s.size();
    T number;
    auto result = std::from_chars(s.begin(), end, number, args...);
    if (result.ec != std::errc{} || result.ptr != end)
        return {};
    return number;
}

}

bool Version::isCompatible(const Version& other) const
{
    return other.major_ == major_ && other.minor_ == minor_;
}

std::string Version::toString() const
{
    return fmt::format("{}.{}.{}", major_, minor_, patch_);
}

bool Version::operator==(const Version& other) const
{
    return (
        std::tie(other.major_, other.minor_, other.patch_) ==
        std::tie(major_, minor_, patch_));
}

bool Version::operator<(const Version& other) const
{
    return (
        std::tie(other.major_, other.minor_, other.patch_) <
        std::tie(major_, minor_, patch_));
}

Version Version::fromJson(const nlohmann::json& j)
{
    try {
        return {
            j.at("major").get<uint16_t>(),
            j.at("minor").get<uint16_t>(),
            j.at("patch").get<uint16_t>()};
    }
    catch (nlohmann::json::out_of_range const& e) {
        throw missing_field(e.what(), "Version");
    }
}

nlohmann::json Version::toJson() const
{
    return nlohmann::json{{"major", major_}, {"minor", minor_}, {"patch", patch_}};
}

IdPart IdPart::fromJson(const nlohmann::json& j)
{
    try {
        return {
            j.at("partId").get<std::string>(),
            j.value("description", ""),
            j.value("datatype", IdPartDataType::I64),
            j.value("isSynthetic", false),
            j.value("isOptional", false)};
    }
    catch (nlohmann::json::out_of_range const& e) {
        throw missing_field(e.what(), "UniqueIdPart");
    }
}

nlohmann::json IdPart::toJson() const
{
    return nlohmann::json{
        {"partId", idPartLabel_},
        {"description", description_},
        {"datatype", datatype_},
        {"isSynthetic", isSynthetic_},
        {"isOptional", isOptional_}};
}

bool IdPart::idPartsMatchComposition(
    const std::vector<IdPart>& candidateComposition,
    uint32_t compositionMatchStartIdx,
    const KeyValueViewPairs& featureIdParts,
    size_t matchLength)
{
    auto featureIdIter = featureIdParts.begin();
    auto compositionIter = candidateComposition.begin();

    while (compositionMatchStartIdx > 0) {
        ++compositionIter;
        --compositionMatchStartIdx;
    }

    while (matchLength > 0 && compositionIter != candidateComposition.end()) {
        // Have we exhausted feature ID parts while there's still composition parts?
        if (featureIdIter == featureIdParts.end()) {
            return false;
        }

        auto& [idPartKey, idPartValue] = *featureIdIter;

        // Does this ID part's field name match?
        if (compositionIter->idPartLabel_ != idPartKey) {
            return false;
        }

        // Does the ID part's value match?
        auto& compositionDataType = compositionIter->datatype_;

        if (std::holds_alternative<int64_t>(idPartValue)) {
            auto value = std::get<int64_t>(idPartValue);
            switch (compositionDataType) {
            case IdPartDataType::I32:
                // Value must fit an I32.
                if (value < INT32_MIN || value > INT32_MAX) {
                    return false;
                }
                break;
            case IdPartDataType::U32:
                if (value < 0 || value > UINT32_MAX) {
                    return false;
                }
                break;
            case IdPartDataType::U64:
                if (value < 0 || value > UINT64_MAX) {
                    return false;
                }
                break;
            default:;
            }
        }
        else if (std::holds_alternative<std::string_view>(idPartValue)) {
            auto value = std::get<std::string_view>(idPartValue);
            // UUID128 should be a 128 bit sequence.
            if (compositionDataType == IdPartDataType::UUID128 && value.size() != 16) {
                return false;
            }
        }
        else {
            throw logRuntimeError("Id part data type not supported!");
        }

        ++featureIdIter;
        ++compositionIter;
        --matchLength;
    }

    // Match means we either checked the required length, or all the values.
    return matchLength == 0;
}

FeatureTypeInfo FeatureTypeInfo::fromJson(const nlohmann::json& j)
{
    try {
        std::vector<std::vector<IdPart>> idCompositions;
        for (auto& item : j.at("uniqueIdCompositions")) {
            std::vector<IdPart> idParts;
            for (auto& idPart : item) {
                idParts.push_back(IdPart::fromJson(idPart));
            }
            idCompositions.push_back(idParts);
        }

        return {j.at("name").get<std::string>(), idCompositions};
    }
    catch (nlohmann::json::out_of_range const& e) {
        throw missing_field(e.what(), "FeatureTypeInfo");
    }
}

nlohmann::json FeatureTypeInfo::toJson() const
{
    std::vector<nlohmann::json> idCompositions;
    for (auto& item : uniqueIdCompositions_) {
        nlohmann::json idParts;
        for (auto& part : item) {
            idParts.push_back(part.toJson());
        }
        idCompositions.push_back(idParts);
    }

    return nlohmann::json{{"name", name_}, {"uniqueIdCompositions", idCompositions}};
}

Coverage Coverage::fromJson(const nlohmann::json& j)
{
    try {
        if (j.is_number_unsigned())
            return {
                j.get<uint64_t>(),
                j.get<uint64_t>(),
                std::vector<bool>()
            };
        return {
            TileId(j.at("min").get<uint64_t>()),
            TileId(j.at("max").get<uint64_t>()),
            j.value("filled", std::vector<bool>())};
    }
    catch (nlohmann::json::out_of_range const& e) {
        throw missing_field(e.what(), "Coverage");
    }
}

nlohmann::json Coverage::toJson() const
{
    if (min_ == max_ && filled_.empty())
        return min_.value_;
    return nlohmann::json{{"min", min_.value_}, {"max", max_.value_}, {"filled", filled_}};
}

std::shared_ptr<LayerInfo> LayerInfo::fromJson(const nlohmann::json& j, std::string const& layerId)
{
    try {
        std::vector<FeatureTypeInfo> featureTypes;
        for (auto& item : j.at("featureTypes")) {
            featureTypes.push_back(FeatureTypeInfo::fromJson(item));
        }

        std::vector<Coverage> coverages;
        if (j.contains("coverage"))
            for (auto& item : j.at("coverage")) {
                coverages.push_back(Coverage::fromJson(item));
            }

        return std::make_shared<LayerInfo>(LayerInfo{
            j.value("layerId", layerId),
            j.value("type", LayerType::Features),
            featureTypes,
            j.value("zoomLevels", std::vector<int>()),
            coverages,
            j.value("canRead", true),
            j.value("canWrite", false),
            Version::fromJson(j.value("version", Version().toJson()))});
    }
    catch (nlohmann::json::out_of_range const& e) {
        throw missing_field(e.what(), "LayerInfo");
    }
}

nlohmann::json LayerInfo::toJson() const
{
    std::vector<nlohmann::json> featureTypes;
    featureTypes.reserve(featureTypes_.size());
    for (const auto& item : featureTypes_) {
        featureTypes.push_back(item.toJson());
    }

    std::vector<nlohmann::json> coverages;
    coverages.reserve(coverage_.size());
    for (const auto& item : coverage_) {
        coverages.push_back(item.toJson());
    }

    return nlohmann::json{
        {"layerId", layerId_},
        {"type", type_},
        {"featureTypes", featureTypes},
        {"zoomLevels", zoomLevels_},
        {"coverage", coverages},
        {"canRead", canRead_},
        {"canWrite", canWrite_},
        {"version", version_.toJson()}};
}

FeatureTypeInfo const* LayerInfo::getTypeInfo(const std::string_view& sv, bool throwIfMissing)
{
    auto typeIt = std::find_if(
        featureTypes_.begin(),
        featureTypes_.end(),
        [&sv](auto&& tp) { return tp.name_ == sv; });
    if (typeIt != featureTypes_.end())
        return &*typeIt;
    if (throwIfMissing) {
        throw logRuntimeError(fmt::format("Could not find feature type {}", sv));
    }
    return nullptr;
}

bool LayerInfo::validFeatureId(
    const std::string_view& typeId,
    KeyValueViewPairs const& featureIdParts,
    bool validateForNewFeature,
    uint32_t compositionMatchStartIndex)
{
    auto typeInfo = getTypeInfo(typeId);

    for (auto& candidateComposition : typeInfo->uniqueIdCompositions_) {
        if (IdPart::idPartsMatchComposition(
            candidateComposition,
            compositionMatchStartIndex,
            featureIdParts,
            featureIdParts.size()))
        {
            return true;
        }

        // References may use alternative ID compositions,
        // but the feature itself must always use the first (primary) one.
        if (validateForNewFeature)
            return false;
    }

    return false;
}

std::optional<std::pair<std::string_view, KeyValueViewPairs>>
LayerInfo::decodeFeatureId(const std::string_view& featureIdString)
{
    // Split the input string by dots
    using namespace std::ranges;
    auto valuesV = featureIdString | views::split('.') | views::transform([](auto&& s){return std::string_view(&*s.begin(), distance(s));});
    auto values = std::vector<std::string_view>(valuesV.begin(), valuesV.end());
    if (values.empty())
        return {};  // Not enough values to form a valid feature ID

    auto&& typeId = values[0];
    auto typeInfo = getTypeInfo(typeId, false);
    if (!typeInfo)
        return {};

    for (auto withOptionalParts : {false, true})
    {
        for (const auto& composition : typeInfo->uniqueIdCompositions_) {
            KeyValueViewPairs result;
            result.reserve(composition.size());

            auto numCheckedValues = 0;
            auto numRequiredValues = 0;
            for (auto&& part : composition) {
                if (part.isOptional_ && !withOptionalParts)
                    continue;

                ++numRequiredValues;
                if (numCheckedValues +1 >= values.size()) {
                    break;
                }
                auto&& rawValue = values[numCheckedValues +1];
                std::optional<std::variant<int64_t, std::string_view>> parsedValue;

                switch (part.datatype_) {
                case IdPartDataType::I32:
                case IdPartDataType::U32:
                case IdPartDataType::I64:
                case IdPartDataType::U64:
                    if (auto intValue = from_chars<int64_t>(rawValue))
                        parsedValue = *intValue;
                    break;
                case IdPartDataType::STR:
                case IdPartDataType::UUID128:
                    parsedValue = rawValue;
                    break;
                }

                if (parsedValue) {
                    result.emplace_back(part.idPartLabel_, *parsedValue);
                    ++numCheckedValues;
                }
                else
                    break;
            }

            if (numCheckedValues != numRequiredValues || numCheckedValues != values.size())
                continue;

            if (validFeatureId(typeId, result, false)) {
                return std::make_pair(typeId, result);
            }
        }
    }

    return {}; // No valid composition found
}

std::shared_ptr<LayerInfo> DataSourceInfo::getLayer(std::string const& layerId, bool throwIfMissing) const
{
    auto it = layers_.find(layerId);
    if (it != layers_.end())
        return it->second;
    if (throwIfMissing) {
        throw logRuntimeError(
            fmt::format("Could not find layer '{}' in map '{}'", layerId, mapId_)
        );
    }
    return {};
}

DataSourceInfo DataSourceInfo::fromJson(const nlohmann::json& j)
{
    try {
        std::map<std::string, std::shared_ptr<LayerInfo>> layers;
        for (auto& item : j.at("layers").items()) {
            layers[item.key()] = LayerInfo::fromJson(item.value(), item.key());
        }

        std::string nodeId;
        if (j.contains("nodeId"))
            nodeId = j.at("nodeId").get<std::string>();
        else
            nodeId = generateUuid();

        return {
            nodeId,
            j.at("mapId").get<std::string>(),
            layers,
            j.value("maxParallelJobs", 8),
            j.value("addOn", false),
            j.value("extraJsonAttachment", nlohmann::json::object()),
            Version::fromJson(
                j.value("protocolVersion", TileLayerStream::CurrentProtocolVersion.toJson()))
        };
    }
    catch (nlohmann::json::out_of_range const& e) {
        throw missing_field(e.what(), "DataSourceInfo");
    }
}

nlohmann::json DataSourceInfo::toJson() const
{
    nlohmann::json layersJson;
    for (const auto& [layerName, layerPtr] : layers_) {
        layersJson[layerName] = layerPtr->toJson();
    }

    return nlohmann::json{
        {"nodeId", nodeId_},
        {"mapId", mapId_},
        {"layers", layersJson},
        {"maxParallelJobs", maxParallelJobs_},
        {"addOn", isAddOn_},
        {"extraJsonAttachment", extraJsonAttachment_},
        {"protocolVersion", protocolVersion_.toJson()}};
}

}
