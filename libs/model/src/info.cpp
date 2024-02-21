#include "info.h"
#include "stream.h"
#include "mapget/log.h"
#include "stx/format.h"

#include <tuple>
#include <random>
#include <sstream>

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
        stx::format("{}::fromJson(): `{}`", context, error));
}

}

bool Version::isCompatible(const Version& other) const
{
    return other.major_ == major_ && other.minor_ == minor_;
}

std::string Version::toString() const
{
    return stx::format("{}.{}.{}", major_, minor_, patch_);
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

std::shared_ptr<LayerInfo> DataSourceInfo::getLayer(std::string const& layerId) const
{
    auto it = layers_.find(layerId);
    if (it == layers_.end()) {
        throw logRuntimeError(
            stx::format("Could not find layer '{}' in map '{}'", layerId, mapId_)
        );
    }
    return it->second;
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
            j.value("extraJsonAttachment", nlohmann::json::object()),
            Version::fromJson(
                j.value("protocolVersion", TileLayerStream::CurrentProtocolVersion.toJson()))};
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
        {"extraJsonAttachment", extraJsonAttachment_},
        {"protocolVersion", protocolVersion_.toJson()}};
}

}
