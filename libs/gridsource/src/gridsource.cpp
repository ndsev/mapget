// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "gridsource/gridsource.h"

#include <cmath>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <thread>

#include "mapget/log.h"
#include "fmt/format.h"
#include "glm/ext.hpp"

#ifdef GRIDSOURCE_WITH_DEVUI
#include "devui.h"
#endif

using namespace mapget;
using namespace mapget::gridsource;

namespace {

// Helper to parse enum from string
template<typename T>
T parseEnum(const std::string& str, const std::map<std::string, T>& mapping, T defaultValue) {
    auto it = mapping.find(str);
    return (it != mapping.end()) ? it->second : defaultValue;
}

// String replacement helper
std::string replaceAll(std::string str, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
    return str;
}

// ============================================================================
// Attribute Tree Profile: Name & Value Pools
// ============================================================================

static constexpr const char* kProfileLayerNames[] = {
    "SpeedRestrictions", "AccessControl", "RoadCharacteristics",
    "TrafficSigns", "LaneModel", "RoadGeometry",
    "RoutingAttributes", "EnvironmentalZones", "TollStructures",
    "ParkingFacilities", "ElectricVehicle", "NavigationAttributes"
};
static constexpr size_t kNumProfileLayerNames = 12;

static constexpr const char* kProfileAttrNames[] = {
    "speedLimit", "overtakingRestriction", "accessPermission",
    "vehicleClassRestriction", "surfaceType", "pavementCondition",
    "numberOfLanes", "laneWidth", "roadWidth",
    "functionalRoadClass", "formOfWay", "gradientPercent",
    "curvatureRadius", "trafficFlowDirection", "dividerType",
    "environmentalZone", "tollCategory", "heightRestriction",
    "weightRestriction", "hazmatRestriction"
};
static constexpr size_t kNumProfileAttrNames = 20;

static constexpr const char* kProfileFieldNames[] = {
    "value", "unit", "condition", "timeOfDay",
    "vehicleType", "source", "confidence", "validFrom",
    "validTo", "applicableDays", "weatherCondition", "seasonality",
    "priority", "overrideLevel", "verificationDate", "dataQuality",
    "measurementMethod", "referenceStandard", "toleranceRange", "exceptionalCase",
    "alternateValue", "displayUnit", "rawValue", "encodingVersion"
};
static constexpr size_t kNumProfileFieldNames = 24;

static constexpr const char* kProfileStringValues[] = {
    "concrete", "asphalt", "gravel", "cobblestone",
    "motorway", "primary", "secondary", "residential",
    "permitted", "forbidden", "restricted", "conditional"
};
static constexpr size_t kNumProfileStringValues = 12;

static constexpr int kProfileIntValues[] = {
    0, 10, 20, 30, 50, 60, 80, 100, 120, 130, 150, 200
};
static constexpr size_t kNumProfileIntValues = 12;

static constexpr double kProfileFloatValues[] = {
    0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 7.5, 10.0
};
static constexpr size_t kNumProfileFloatValues = 12;

// Deterministic hash for pool selection
inline uint32_t profileHash(uint32_t seed, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = seed;
    h ^= a * 2654435761u;
    h ^= b * 2246822519u;
    h ^= c * 3266489917u;
    h ^= h >> 16;
    h *= 0x45d9f3b;
    h ^= h >> 16;
    return h;
}

// Profile string-to-enum mapping
static const std::map<std::string, AttributeTreeProfile> kProfileMap = {
    {"none", AttributeTreeProfile::None},
    {"minimal", AttributeTreeProfile::Minimal},
    {"moderate", AttributeTreeProfile::Moderate},
    {"realistic", AttributeTreeProfile::Realistic},
    {"stress", AttributeTreeProfile::Stress}
};

std::string profileToString(AttributeTreeProfile p) {
    for (auto& [k, v] : kProfileMap) {
        if (v == p) return k;
    }
    return "none";
}

}  // anonymous namespace

// ============================================================================
// Config Parsing Implementation
// ============================================================================

AttributeConfig AttributeConfig::fromYAML(const YAML::Node& node) {
    AttributeConfig cfg;
    if (!node) return cfg;

    cfg.name = node["name"].as<std::string>("");

    // Parse data type
    static const std::map<std::string, DataType> dataTypeMap = {
        {"int", DataType::Int}, {"float", DataType::Float},
        {"string", DataType::String}, {"bool", DataType::Bool},
        {"uint16", DataType::UInt16}, {"uint32", DataType::UInt32},
        {"int64", DataType::Int64}
    };
    cfg.dataType = parseEnum(node["type"].as<std::string>("int"), dataTypeMap, DataType::Int);

    // Parse generator type
    static const std::map<std::string, GeneratorType> genTypeMap = {
        {"random", GeneratorType::Random}, {"sequential", GeneratorType::Sequential},
        {"computed", GeneratorType::Computed}, {"zoned", GeneratorType::Zoned},
        {"fixed", GeneratorType::Fixed}, {"markov", GeneratorType::Markov},
        {"spatial", GeneratorType::Spatial}
    };
    cfg.generator = parseEnum(node["generator"].as<std::string>("random"), genTypeMap, GeneratorType::Random);

    // Parse generator-specific fields
    if (node["min"]) cfg.min = node["min"].as<double>();
    if (node["max"]) cfg.max = node["max"].as<double>();

    if (node["values"]) {
        for (const auto& val : node["values"]) {
            cfg.stringValues.push_back(val.as<std::string>());
        }
    }

    if (node["weights"]) {
        for (const auto& w : node["weights"]) {
            cfg.weights.push_back(w.as<double>());
        }
    }

    // Parse distribution
    static const std::map<std::string, DistributionType> distMap = {
        {"uniform", DistributionType::Uniform},
        {"normal", DistributionType::Normal},
        {"exponential", DistributionType::Exponential}
    };
    cfg.distribution = parseEnum(node["distribution"].as<std::string>("uniform"), distMap, DistributionType::Uniform);

    if (node["mean"]) cfg.mean = node["mean"].as<double>();
    if (node["stddev"]) cfg.stddev = node["stddev"].as<double>();
    if (node["lambda"]) cfg.lambda = node["lambda"].as<double>();
    if (node["template"]) cfg.templateStr = node["template"].as<std::string>();
    if (node["formula"]) cfg.formula = node["formula"].as<std::string>();

    if (node["zones"]) {
        for (const auto& z : node["zones"]) {
            cfg.zones.push_back(z.as<double>());
        }
    }

    if (node["zoneDistances"]) {
        for (const auto& d : node["zoneDistances"]) {
            cfg.zoneDistances.push_back(d.as<double>());
        }
    }

    if (node["value"]) cfg.fixedValue = node["value"].as<std::string>();

    return cfg;
}

LayeredAttributeConfig LayeredAttributeConfig::fromYAML(const YAML::Node& node) {
    LayeredAttributeConfig cfg;
    if (!node) return cfg;

    cfg.name = node["name"].as<std::string>("");
    cfg.validityType = node["validityType"].as<std::string>("none");
    cfg.splitProbability = node["splitProbability"].as<double>(0.0);
    cfg.errorProbability = node["errorProbability"].as<double>(0.0);

    if (node["fields"]) {
        for (const auto& field : node["fields"]) {
            cfg.fields.push_back(AttributeConfig::fromYAML(field));
        }
    }

    return cfg;
}

AttributeLayerConfig AttributeLayerConfig::fromYAML(const YAML::Node& node) {
    AttributeLayerConfig cfg;
    if (!node) return cfg;

    cfg.layerName = node["layerName"].as<std::string>("");

    if (node["attributes"]) {
        for (const auto& attr : node["attributes"]) {
            cfg.attributes.push_back(LayeredAttributeConfig::fromYAML(attr));
        }
    }

    return cfg;
}

RelationConfig RelationConfig::fromYAML(const YAML::Node& node) {
    RelationConfig cfg;
    if (!node) return cfg;

    cfg.name = node["name"].as<std::string>("");
    cfg.targetLayer = node["targetLayer"].as<std::string>("");
    cfg.targetType = node["targetType"].as<std::string>("");
    cfg.maxDistance = node["maxDistance"].as<double>(100.0);
    cfg.cardinality = node["cardinality"].as<std::string>("one");
    cfg.optional = node["optional"].as<bool>(false);
    cfg.validityType = node["validityType"].as<std::string>("none");

    return cfg;
}

GeometryConfig GeometryConfig::fromYAML(const YAML::Node& node) {
    GeometryConfig cfg;
    if (!node) return cfg;

    // Parse geometry type
    static const std::map<std::string, GeometryType> geomTypeMap = {
        {"point", GeometryType::Point}, {"line", GeometryType::Line},
        {"polygon", GeometryType::Polygon}, {"mesh", GeometryType::Mesh}
    };
    cfg.type = parseEnum(node["type"].as<std::string>("line"), geomTypeMap, GeometryType::Line);

    cfg.density = node["density"].as<double>(0.05);
    cfg.complexity = node["complexity"].as<int>(6);
    cfg.curvature = node["curvature"].as<double>(0.08);

    if (node["sizeRange"] && node["sizeRange"].size() == 2) {
        cfg.sizeRange = {
            node["sizeRange"][0].as<double>(),
            node["sizeRange"][1].as<double>()
        };
    }

    if (node["aspectRatio"] && node["aspectRatio"].size() == 2) {
        cfg.aspectRatio = {
            node["aspectRatio"][0].as<double>(),
            node["aspectRatio"][1].as<double>()
        };
    }

    cfg.avoidBuildings = node["avoidBuildings"].as<bool>(true);
    cfg.minBuildingDistance = node["minBuildingDistance"].as<double>(2.0);

    return cfg;
}

LayerConfig LayerConfig::fromYAML(const YAML::Node& node) {
    LayerConfig cfg;
    if (!node) return cfg;

    cfg.name = node["name"].as<std::string>("");
    cfg.enabled = node["enabled"].as<bool>(true);
    cfg.featureType = node["featureType"].as<std::string>("");

    if (node["geometry"]) {
        cfg.geometry = GeometryConfig::fromYAML(node["geometry"]);
    }

    if (node["attributes"]) {
        if (node["attributes"]["top"]) {
            for (const auto& attr : node["attributes"]["top"]) {
                cfg.topAttributes.push_back(AttributeConfig::fromYAML(attr));
            }
        }
        if (node["attributes"]["layered"]) {
            for (const auto& layer : node["attributes"]["layered"]) {
                cfg.layeredAttributes.push_back(AttributeLayerConfig::fromYAML(layer));
            }
        }
    }

    if (node["relations"]) {
        for (const auto& rel : node["relations"]) {
            cfg.relations.push_back(RelationConfig::fromYAML(rel));
        }
    }

    // Per-layer attribute tree profile override
    if (node["attributeTreeProfile"]) {
        auto profileStr = node["attributeTreeProfile"].as<std::string>("none");
        cfg.attributeTreeProfile = parseEnum(profileStr, kProfileMap, AttributeTreeProfile::None);
    }
    if (node["attributeTreeParams"]) {
        cfg.attributeTreeParams = AttributeTreeParams::fromYAML(node["attributeTreeParams"]);
    }

    return cfg;
}

AttributeTreeParams AttributeTreeParams::fromYAML(const YAML::Node& node) {
    AttributeTreeParams p;
    if (!node) return p;
    if (node["numLayers"]) p.numLayers = node["numLayers"].as<int>();
    if (node["attrsPerLayer"]) p.attrsPerLayer = node["attrsPerLayer"].as<int>();
    if (node["fieldsPerAttr"]) p.fieldsPerAttr = node["fieldsPerAttr"].as<int>();
    if (node["maxNestingDepth"]) p.maxNestingDepth = node["maxNestingDepth"].as<int>();
    if (node["maxArraySize"]) p.maxArraySize = node["maxArraySize"].as<int>();
    if (node["nestingProbability"]) p.nestingProbability = node["nestingProbability"].as<double>();
    if (node["directionalValidityProb"]) p.directionalValidityProb = node["directionalValidityProb"].as<double>();
    if (node["rangeValidityProb"]) p.rangeValidityProb = node["rangeValidityProb"].as<double>();
    if (node["topLevelExtraFields"]) p.topLevelExtraFields = node["topLevelExtraFields"].as<int>();
    return p;
}

ResolvedTreeParams ResolvedTreeParams::resolve(
    AttributeTreeProfile profile,
    const AttributeTreeParams& global,
    const AttributeTreeParams& layer)
{
    // Profile base values
    //                        minimal  moderate  realistic  stress
    struct Preset { int nL; int aPL; int fPA; int mND; int mAS; double nP; double dVP; double rVP; int tLEF; };
    static const std::map<AttributeTreeProfile, Preset> presets = {
        {AttributeTreeProfile::Minimal,   { 1, 2, 2, 0, 0, 0.0,  0.0,  0.0, 1}},
        {AttributeTreeProfile::Moderate,  { 3, 4, 3, 1, 0, 0.15, 0.2,  0.0, 3}},
        {AttributeTreeProfile::Realistic, { 6, 5, 4, 2, 3, 0.25, 0.5,  0.3, 5}},
        {AttributeTreeProfile::Stress,    {12, 10, 8, 4, 6, 0.4,  0.8,  0.5, 10}},
    };

    auto it = presets.find(profile);
    Preset base = (it != presets.end()) ? it->second : Preset{1, 2, 2, 0, 0, 0.0, 0.0, 0.0, 1};

    // Apply overrides: layer > global > preset
    auto pick = [](std::optional<int> layerV, std::optional<int> globalV, int preset) {
        return layerV.value_or(globalV.value_or(preset));
    };
    auto pickD = [](std::optional<double> layerV, std::optional<double> globalV, double preset) {
        return layerV.value_or(globalV.value_or(preset));
    };

    ResolvedTreeParams r;
    r.numLayers = pick(layer.numLayers, global.numLayers, base.nL);
    r.attrsPerLayer = pick(layer.attrsPerLayer, global.attrsPerLayer, base.aPL);
    r.fieldsPerAttr = pick(layer.fieldsPerAttr, global.fieldsPerAttr, base.fPA);
    r.maxNestingDepth = pick(layer.maxNestingDepth, global.maxNestingDepth, base.mND);
    r.maxArraySize = pick(layer.maxArraySize, global.maxArraySize, base.mAS);
    r.nestingProbability = pickD(layer.nestingProbability, global.nestingProbability, base.nP);
    r.directionalValidityProb = pickD(layer.directionalValidityProb, global.directionalValidityProb, base.dVP);
    r.rangeValidityProb = pickD(layer.rangeValidityProb, global.rangeValidityProb, base.rVP);
    r.topLevelExtraFields = pick(layer.topLevelExtraFields, global.topLevelExtraFields, base.tLEF);
    return r;
}

Config Config::fromYAML(const YAML::Node& node) {
    Config cfg;
    if (!node) return cfg;

    cfg.mapId = node["mapId"].as<std::string>("GridDataSource");
    cfg.spatialCoherence = node["spatialCoherence"].as<bool>(true);
    cfg.collisionGridSize = node["collisionGridSize"].as<double>(10.0);

    // New delay fields
    cfg.sourceDownloadDelayMs = node["sourceDownloadDelayMs"].as<uint32_t>(0);
    cfg.sourceUnpackDelayMs = node["sourceUnpackDelayMs"].as<uint32_t>(0);
    cfg.sourceTransformDelayMs = node["sourceTransformDelayMs"].as<uint32_t>(0);
    cfg.devPort = node["devPort"].as<uint16_t>(0);

    // Legacy fallback: map old delayMs/delayMode to new fields
    if (node["delayMs"] && !node["sourceDownloadDelayMs"]
        && !node["sourceUnpackDelayMs"] && !node["sourceTransformDelayMs"])
    {
        auto legacyDelay = node["delayMs"].as<uint32_t>(0);
        std::string delayMode = node["delayMode"].as<std::string>("sleep");
        if (delayMode == "busyWait")
            cfg.sourceTransformDelayMs = legacyDelay;
        else
            cfg.sourceDownloadDelayMs = legacyDelay;
    }

    // Attribute tree profile
    if (node["attributeTreeProfile"]) {
        cfg.attributeTreeProfile = parseEnum(
            node["attributeTreeProfile"].as<std::string>("none"),
            kProfileMap, AttributeTreeProfile::None);
    }
    if (node["attributeTreeParams"]) {
        cfg.attributeTreeParams = AttributeTreeParams::fromYAML(node["attributeTreeParams"]);
    }

    if (node["layers"]) {
        for (const auto& layer : node["layers"]) {
            auto layerCfg = LayerConfig::fromYAML(layer);
            if (layerCfg.enabled) {
                cfg.layers.push_back(layerCfg);
            }
        }
    }

    return cfg;
}

nlohmann::json Config::toJson() const {
    nlohmann::json j;
    j["mapId"] = mapId;
    j["spatialCoherence"] = spatialCoherence;
    j["collisionGridSize"] = collisionGridSize;
    j["sourceDownloadDelayMs"] = sourceDownloadDelayMs;
    j["sourceUnpackDelayMs"] = sourceUnpackDelayMs;
    j["sourceTransformDelayMs"] = sourceTransformDelayMs;
    j["devPort"] = devPort;
    j["attributeTreeProfile"] = profileToString(attributeTreeProfile);

    nlohmann::json atp;
    if (attributeTreeParams.numLayers) atp["numLayers"] = *attributeTreeParams.numLayers;
    if (attributeTreeParams.attrsPerLayer) atp["attrsPerLayer"] = *attributeTreeParams.attrsPerLayer;
    if (attributeTreeParams.fieldsPerAttr) atp["fieldsPerAttr"] = *attributeTreeParams.fieldsPerAttr;
    if (attributeTreeParams.maxNestingDepth) atp["maxNestingDepth"] = *attributeTreeParams.maxNestingDepth;
    if (attributeTreeParams.maxArraySize) atp["maxArraySize"] = *attributeTreeParams.maxArraySize;
    if (attributeTreeParams.nestingProbability) atp["nestingProbability"] = *attributeTreeParams.nestingProbability;
    if (attributeTreeParams.directionalValidityProb) atp["directionalValidityProb"] = *attributeTreeParams.directionalValidityProb;
    if (attributeTreeParams.rangeValidityProb) atp["rangeValidityProb"] = *attributeTreeParams.rangeValidityProb;
    if (attributeTreeParams.topLevelExtraFields) atp["topLevelExtraFields"] = *attributeTreeParams.topLevelExtraFields;
    j["attributeTreeParams"] = atp;

    j["layers"] = nlohmann::json::array();
    for (const auto& layer : layers) {
        nlohmann::json lj;
        lj["name"] = layer.name;
        lj["enabled"] = layer.enabled;
        lj["featureType"] = layer.featureType;
        lj["geometry"] = nlohmann::json{
            {"type", layer.geometry.type == GeometryType::Point ? "point" :
                     layer.geometry.type == GeometryType::Line ? "line" :
                     layer.geometry.type == GeometryType::Polygon ? "polygon" : "mesh"},
            {"density", layer.geometry.density},
            {"complexity", layer.geometry.complexity},
            {"curvature", layer.geometry.curvature},
            {"sizeRange", {layer.geometry.sizeRange[0], layer.geometry.sizeRange[1]}},
            {"aspectRatio", {layer.geometry.aspectRatio[0], layer.geometry.aspectRatio[1]}},
            {"avoidBuildings", layer.geometry.avoidBuildings},
            {"minBuildingDistance", layer.geometry.minBuildingDistance}
        };
        if (layer.attributeTreeProfile) {
            lj["attributeTreeProfile"] = profileToString(*layer.attributeTreeProfile);
        }
        j["layers"].push_back(lj);
    }
    return j;
}

Config Config::fromJson(const nlohmann::json& j) {
    Config cfg;
    if (j.contains("mapId")) cfg.mapId = j["mapId"].get<std::string>();
    if (j.contains("spatialCoherence")) cfg.spatialCoherence = j["spatialCoherence"].get<bool>();
    if (j.contains("collisionGridSize")) cfg.collisionGridSize = j["collisionGridSize"].get<double>();
    if (j.contains("sourceDownloadDelayMs")) cfg.sourceDownloadDelayMs = j["sourceDownloadDelayMs"].get<uint32_t>();
    if (j.contains("sourceUnpackDelayMs")) cfg.sourceUnpackDelayMs = j["sourceUnpackDelayMs"].get<uint32_t>();
    if (j.contains("sourceTransformDelayMs")) cfg.sourceTransformDelayMs = j["sourceTransformDelayMs"].get<uint32_t>();
    if (j.contains("devPort")) cfg.devPort = j["devPort"].get<uint16_t>();
    if (j.contains("attributeTreeProfile")) {
        auto profileStr = j["attributeTreeProfile"].get<std::string>();
        auto it = kProfileMap.find(profileStr);
        cfg.attributeTreeProfile = (it != kProfileMap.end()) ? it->second : AttributeTreeProfile::None;
    }
    if (j.contains("attributeTreeParams")) {
        auto& atp = j["attributeTreeParams"];
        if (atp.contains("numLayers")) cfg.attributeTreeParams.numLayers = atp["numLayers"].get<int>();
        if (atp.contains("attrsPerLayer")) cfg.attributeTreeParams.attrsPerLayer = atp["attrsPerLayer"].get<int>();
        if (atp.contains("fieldsPerAttr")) cfg.attributeTreeParams.fieldsPerAttr = atp["fieldsPerAttr"].get<int>();
        if (atp.contains("maxNestingDepth")) cfg.attributeTreeParams.maxNestingDepth = atp["maxNestingDepth"].get<int>();
        if (atp.contains("maxArraySize")) cfg.attributeTreeParams.maxArraySize = atp["maxArraySize"].get<int>();
        if (atp.contains("nestingProbability")) cfg.attributeTreeParams.nestingProbability = atp["nestingProbability"].get<double>();
        if (atp.contains("directionalValidityProb")) cfg.attributeTreeParams.directionalValidityProb = atp["directionalValidityProb"].get<double>();
        if (atp.contains("rangeValidityProb")) cfg.attributeTreeParams.rangeValidityProb = atp["rangeValidityProb"].get<double>();
        if (atp.contains("topLevelExtraFields")) cfg.attributeTreeParams.topLevelExtraFields = atp["topLevelExtraFields"].get<int>();
    }
    if (j.contains("layers")) {
        for (const auto& lj : j["layers"]) {
            LayerConfig layer;
            if (lj.contains("name")) layer.name = lj["name"].get<std::string>();
            if (lj.contains("enabled")) layer.enabled = lj["enabled"].get<bool>();
            if (lj.contains("featureType")) layer.featureType = lj["featureType"].get<std::string>();
            if (lj.contains("geometry")) {
                auto& gj = lj["geometry"];
                if (gj.contains("type")) {
                    static const std::map<std::string, GeometryType> gmap = {
                        {"point", GeometryType::Point}, {"line", GeometryType::Line},
                        {"polygon", GeometryType::Polygon}, {"mesh", GeometryType::Mesh}
                    };
                    auto it = gmap.find(gj["type"].get<std::string>());
                    if (it != gmap.end()) layer.geometry.type = it->second;
                }
                if (gj.contains("density")) layer.geometry.density = gj["density"].get<double>();
                if (gj.contains("complexity")) layer.geometry.complexity = gj["complexity"].get<int>();
                if (gj.contains("curvature")) layer.geometry.curvature = gj["curvature"].get<double>();
            }
            if (lj.contains("attributeTreeProfile")) {
                auto profileStr = lj["attributeTreeProfile"].get<std::string>();
                auto it = kProfileMap.find(profileStr);
                layer.attributeTreeProfile = (it != kProfileMap.end()) ? it->second : AttributeTreeProfile::None;
            }
            cfg.layers.push_back(layer);
        }
    }
    return cfg;
}

// ============================================================================
// TileSpatialContext Implementation
// ============================================================================

TileSpatialContext::TileSpatialContext(TileId tid, double gridSize)
    : tileId(tid), cellSize(gridSize) {
    // Use a proper hash of the tile ID to ensure different tiles get different seeds
    // This prevents tiles at the same latitude from having similar patterns
    std::hash<uint64_t> hasher;
    seed = static_cast<uint32_t>(hasher(tid.value_));
}

uint64_t TileSpatialContext::cellKey(int x, int y) {
    return (static_cast<uint64_t>(x) << 32) | static_cast<uint64_t>(y & 0xFFFFFFFF);
}

bool TileSpatialContext::isCellOccupied(int gridX, int gridY) const {
    return occupiedCells.count(cellKey(gridX, gridY)) > 0;
}

void TileSpatialContext::markBuildingCells(const Building& building) {
    int minCellX = static_cast<int>(std::floor(building.minX / cellSize));
    int maxCellX = static_cast<int>(std::floor(building.maxX / cellSize));
    int minCellY = static_cast<int>(std::floor(building.minY / cellSize));
    int maxCellY = static_cast<int>(std::floor(building.maxY / cellSize));

    for (int x = minCellX; x <= maxCellX; ++x) {
        for (int y = minCellY; y <= maxCellY; ++y) {
            uint64_t key = cellKey(x, y);
            occupiedCells.insert(key);
            cellToBuilding[key] = building.id;
        }
    }
}

bool TileSpatialContext::doesLineIntersectBox(Point a, Point b,
                                               double minX, double minY,
                                               double maxX, double maxY) const {
    // Liang-Barsky line clipping algorithm
    double t0 = 0.0, t1 = 1.0;
    double dx = b.x - a.x;
    double dy = b.y - a.y;

    auto clipTest = [&](double p, double q) {
        if (std::abs(p) < 1e-10) {
            return q >= 0.0;
        }
        double r = q / p;
        if (p < 0.0) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
        return true;
    };

    if (!clipTest(-dx, a.x - minX)) return false;
    if (!clipTest(dx, maxX - a.x)) return false;
    if (!clipTest(-dy, a.y - minY)) return false;
    if (!clipTest(dy, maxY - a.y)) return false;

    return t0 < t1;
}

bool TileSpatialContext::doesLineIntersectBuilding(Point a, Point b) const {
    for (const auto& building : buildings) {
        if (doesLineIntersectBox(a, b, building.minX, building.minY,
                                 building.maxX, building.maxY)) {
            return true;
        }
    }
    return false;
}

Point TileSpatialContext::closestPointOnSegment(Point p, Point a, Point b) const {
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    double lengthSq = dx * dx + dy * dy;

    if (lengthSq < 1e-10) {
        return a;
    }

    double t = std::max(0.0, std::min(1.0, ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSq));
    return Point(a.x + t * dx, a.y + t * dy, 0.0);
}

Point TileSpatialContext::findNearestRoadPoint(Point p, double* outDistance) const {
    Point nearest = p;
    double minDist = std::numeric_limits<double>::max();

    for (const auto& road : roads) {
        std::vector<Point> allPoints;
        allPoints.push_back(road.start);
        allPoints.insert(allPoints.end(), road.intermediatePoints.begin(), road.intermediatePoints.end());
        allPoints.push_back(road.end);

        for (size_t i = 1; i < allPoints.size(); ++i) {
            Point closest = closestPointOnSegment(p, allPoints[i-1], allPoints[i]);
            double dx = p.x - closest.x;
            double dy = p.y - closest.y;
            double dist = std::sqrt(dx * dx + dy * dy);

            if (dist < minDist) {
                minDist = dist;
                nearest = closest;
            }
        }
    }

    if (outDistance) *outDistance = minDist;
    return nearest;
}

std::vector<uint32_t> TileSpatialContext::findBuildingsNearPoint(Point p, double radius) const {
    std::vector<uint32_t> result;
    double radiusSq = radius * radius;

    for (const auto& building : buildings) {
        // Find closest point on building to p
        double closestX = std::clamp(p.x, building.minX, building.maxX);
        double closestY = std::clamp(p.y, building.minY, building.maxY);
        double dx = p.x - closestX;
        double dy = p.y - closestY;
        double distSq = dx * dx + dy * dy;

        if (distSq <= radiusSq) {
            result.push_back(building.id);
        }
    }

    return result;
}

uint32_t TileSpatialContext::findRoadAtPoint(Point p, double tolerance) const {
    double minDist = std::numeric_limits<double>::max();
    uint32_t roadId = 0;

    for (const auto& road : roads) {
        std::vector<Point> allPoints;
        allPoints.push_back(road.start);
        allPoints.insert(allPoints.end(), road.intermediatePoints.begin(), road.intermediatePoints.end());
        allPoints.push_back(road.end);

        for (size_t i = 1; i < allPoints.size(); ++i) {
            Point closest = closestPointOnSegment(p, allPoints[i-1], allPoints[i]);
            double dx = p.x - closest.x;
            double dy = p.y - closest.y;
            double dist = std::sqrt(dx * dx + dy * dy);

            if (dist < minDist) {
                minDist = dist;
                roadId = road.id;
            }
        }
    }

    return (minDist <= tolerance) ? roadId : 0;
}

// ============================================================================
// GridDataSource Implementation
// ============================================================================

// Static registry members
std::mutex GridDataSource::registryMutex_;
std::vector<std::weak_ptr<GridDataSource>> GridDataSource::registry_;

void GridDataSource::registerInstance(std::shared_ptr<GridDataSource> instance) {
    std::lock_guard<std::mutex> lock(registryMutex_);
    // Prune expired entries
    registry_.erase(
        std::remove_if(registry_.begin(), registry_.end(),
            [](const auto& wp) { return wp.expired(); }),
        registry_.end());
    registry_.push_back(instance);
}

std::vector<std::shared_ptr<GridDataSource>> GridDataSource::getInstances() {
    std::lock_guard<std::mutex> lock(registryMutex_);
    std::vector<std::shared_ptr<GridDataSource>> result;
    auto it = registry_.begin();
    while (it != registry_.end()) {
        if (auto sp = it->lock()) {
            result.push_back(sp);
            ++it;
        } else {
            it = registry_.erase(it);
        }
    }
    return result;
}

void GridDataSource::setConfig(gridsource::Config newConfig) {
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_ = std::make_shared<const gridsource::Config>(std::move(newConfig));
    }
    clearContextCache();
}

gridsource::Config GridDataSource::getConfig() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return *config_;
}

void GridDataSource::clearContextCache() {
    std::lock_guard<std::mutex> lock(contextMutex_);
    contextCache_.clear();
}

void GridDataSource::setTileCacheInvalidationCallback(std::function<void()> callback) {
    tileCacheInvalidationCallback_ = std::move(callback);
}

void GridDataSource::invokeTileCacheInvalidationCallback() {
    if (tileCacheInvalidationCallback_)
        tileCacheInvalidationCallback_();
}

GridDataSource::GridDataSource(const YAML::Node& config) {
    auto cfg = std::make_shared<gridsource::Config>();
    if (config && config.IsMap()) {
        *cfg = Config::fromYAML(config);
    } else {
        // Default configuration with roads and buildings
        cfg->mapId = "GridDataSource";
        cfg->spatialCoherence = true;
        cfg->collisionGridSize = 10.0;

        // Default building layer
        LayerConfig buildingLayer;
        buildingLayer.name = "DevSrc-BuildingLayer";
        buildingLayer.enabled = true;
        buildingLayer.featureType = "DevSrc-Building";
        buildingLayer.geometry.type = GeometryType::Polygon;
        buildingLayer.geometry.density = 0.03;
        buildingLayer.geometry.complexity = 4;
        buildingLayer.geometry.sizeRange = {15.0, 50.0};
        buildingLayer.geometry.aspectRatio = {1.2, 3.0};
        cfg->layers.push_back(buildingLayer);

        // Default road layer
        LayerConfig roadLayer;
        roadLayer.name = "DevSrc-RoadLayer";
        roadLayer.enabled = true;
        roadLayer.featureType = "DevSrc-Road";
        roadLayer.geometry.type = GeometryType::Line;
        roadLayer.geometry.density = 0.08;
        roadLayer.geometry.complexity = 6;
        roadLayer.geometry.curvature = 0.08;
        roadLayer.geometry.avoidBuildings = true;
        roadLayer.geometry.minBuildingDistance = 2.0;
        cfg->layers.push_back(roadLayer);

        // Default intersection layer
        LayerConfig intersectionLayer;
        intersectionLayer.name = "DevSrc-IntersectionLayer";
        intersectionLayer.enabled = true;
        intersectionLayer.featureType = "DevSrc-Intersection";
        intersectionLayer.geometry.type = GeometryType::Point;
        cfg->layers.push_back(intersectionLayer);
    }
    config_ = std::move(cfg);

#ifdef GRIDSOURCE_WITH_DEVUI
    if (config_->devPort > 0)
        gridsource::startDevUIServer(config_->devPort);
#endif
}

DataSourceInfo GridDataSource::info() {
    auto cfg = [&] {
        std::lock_guard<std::mutex> lock(configMutex_);
        return config_;
    }();

    nlohmann::json info;
    info["mapId"] = cfg->mapId;
    info["layers"] = nlohmann::json::object();

    mapget::log().info("GridDataSource registering {} layers", cfg->layers.size());

    // Collect all unique feature types across all layers
    std::set<std::string> allFeatureTypes;
    for (const auto& layer : cfg->layers) {
        allFeatureTypes.insert(layer.featureType);
    }

    // Register each layer with ALL feature types (for cross-layer relations)
    for (const auto& layer : cfg->layers) {
        nlohmann::json layerInfo;
        layerInfo["featureTypes"] = nlohmann::json::array();

        // Register all feature types in this layer
        for (const auto& typeName : allFeatureTypes) {
            nlohmann::json featureType;
            featureType["name"] = typeName;

            nlohmann::json idComp = nlohmann::json::array();
            idComp.push_back({
                {"partId", "tileId"},
                {"description", "Tile identifier"},
                {"datatype", "I64"}
            });
            idComp.push_back({
                {"partId", typeName + "Id"},
                {"description", "Per-tile unique ID"},
                {"datatype", "U32"}
            });

            featureType["uniqueIdCompositions"] = nlohmann::json::array();
            featureType["uniqueIdCompositions"].push_back(idComp);

            layerInfo["featureTypes"].push_back(featureType);
        }

        info["layers"][layer.name] = layerInfo;
        mapget::log().info("  Layer '{}' with {} feature types", layer.name, allFeatureTypes.size());
    }

    return DataSourceInfo::fromJson(info);
}

std::shared_ptr<TileSpatialContext> GridDataSource::getOrCreateContext(
    TileId tileId,
    std::shared_ptr<const gridsource::Config> const& cfg) const
{
    try {
        std::lock_guard<std::mutex> lock(contextMutex_);

        auto it = contextCache_.find(tileId);
        if (it != contextCache_.end()) {
            return it->second;
        }

        // Create new context
        auto ctx = std::make_shared<TileSpatialContext>(tileId, cfg->collisionGridSize);

        // LRU eviction if cache is full
        if (contextCache_.size() >= MAX_CACHED_CONTEXTS) {
            contextCache_.erase(contextCache_.begin());
        }

        contextCache_[tileId] = ctx;
        return ctx;
    } catch (const std::system_error&) {
        return std::make_shared<TileSpatialContext>(tileId, cfg->collisionGridSize);
    }
}

void GridDataSource::fill(TileFeatureLayer::Ptr const& tile) {
    // Snapshot config at entry for consistent use throughout fill()
    auto cfg = [&] {
        std::lock_guard<std::mutex> lock(configMutex_);
        return config_;
    }();

    std::string layerName = tile->layerInfo()->layerId_;

    mapget::log().info("GridDataSource::fill() called for layer '{}' tile {}", layerName, tile->tileId().value_);

    // Phase 1: Simulated source download (sleep-wait, IO)
    if (cfg->sourceDownloadDelayMs > 0) {
        auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg->sourceDownloadDelayMs));
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        tile->setInfo("source-download-ms", elapsed);
    }

    // Phase 2: Simulated source unpack (busy-wait, CPU)
    if (cfg->sourceUnpackDelayMs > 0) {
        auto t0 = std::chrono::steady_clock::now();
        auto deadline = t0 + std::chrono::milliseconds(cfg->sourceUnpackDelayMs);
        while (std::chrono::steady_clock::now() < deadline) {}
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        tile->setInfo("source-unpack-ms", elapsed);
    }

    // Get or create spatial context for this tile
    auto ctx = getOrCreateContext(tile->tileId(), cfg);

    // Set ID prefix
    tile->setIdPrefix({{"tileId", static_cast<int64_t>(tile->tileId().value_)}});

    // Phase 3: Feature generation (actual work)
    auto genStart = std::chrono::steady_clock::now();
    for (const auto& layerCfg : cfg->layers) {
        if (layerCfg.name == layerName) {
            mapget::log().info("  Found matching layer config, geometry type: {}", static_cast<int>(layerCfg.geometry.type));
            if (layerCfg.geometry.type == GeometryType::Polygon || layerCfg.geometry.type == GeometryType::Mesh) {
                mapget::log().info("  Generating buildings...");
                generateBuildings(*ctx, layerCfg, *cfg, tile);
                mapget::log().info("  Generated {} buildings", ctx->buildings.size());
            } else if (layerCfg.geometry.type == GeometryType::Line) {
                mapget::log().info("  Generating roads...");
                generateRoads(*ctx, layerCfg, *cfg, tile);
                mapget::log().info("  Generated {} roads", ctx->roads.size());
            } else if (layerCfg.geometry.type == GeometryType::Point) {
                mapget::log().info("  Generating intersections...");
                generateIntersections(*ctx, layerCfg, *cfg, tile);
                mapget::log().info("  Generated {} intersections", ctx->intersections.size());
            }

            auto genElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - genStart).count();
            tile->setInfo("feature-gen-ms", genElapsed);

            // Phase 4: Simulated source transform (busy-wait, CPU)
            if (cfg->sourceTransformDelayMs > 0) {
                auto t0 = std::chrono::steady_clock::now();
                auto deadline = t0 + std::chrono::milliseconds(cfg->sourceTransformDelayMs);
                while (std::chrono::steady_clock::now() < deadline) {}
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                tile->setInfo("source-transform-ms", elapsed);
            }

            return;
        }
    }
    mapget::log().warn("  No matching layer configuration found for '{}'", layerName);
}

std::vector<LocateResponse> GridDataSource::locate(const LocateRequest& req) {
    auto cfg = [&] {
        std::lock_guard<std::mutex> lock(configMutex_);
        return config_;
    }();

    // Extract tileId from the feature ID parts
    std::optional<int64_t> tileId = req.getIntIdPart("tileId");
    if (!tileId) {
        mapget::log().warn("GridDataSource::locate() - tileId not found in feature ID");
        return {};
    }

    // Find the layer that contains this feature type
    std::string layerId;
    for (const auto& layer : cfg->layers) {
        if (layer.featureType == req.typeId_) {
            layerId = layer.name;
            break;
        }
    }

    if (layerId.empty()) {
        mapget::log().warn("GridDataSource::locate() - layer not found for feature type '{}'", req.typeId_);
        return {};
    }

    // Create the MapTileKey
    MapTileKey mapTileKey;
    mapTileKey.layer_ = LayerType::Features;
    mapTileKey.mapId_ = req.mapId_;
    mapTileKey.layerId_ = layerId;
    mapTileKey.tileId_ = TileId(*tileId);

    // Create and return the LocateResponse
    LocateResponse locateResponse(req);
    locateResponse.tileKey_ = mapTileKey;

    mapget::log().debug("GridDataSource::locate() - Found feature '{}' in tile {} layer '{}'",
                       req.typeId_, *tileId, layerId);

    return {locateResponse};
}

void GridDataSource::generateBuildings(TileSpatialContext& ctx,
                                       const LayerConfig& layerCfg,
                                       const gridsource::Config& cfg,
                                       TileFeatureLayer::Ptr const& tile) {
    // Lazily generate road grid first (ensures roads are always generated before buildings)
    generateRoadGrid(ctx, layerCfg, tile);

    // Only generate buildings once for this tile
    if (!ctx.buildings.empty()) {
        // Buildings already generated, just recreate features
        for (const auto& building : ctx.buildings) {
            auto feature = tile->newFeature(layerCfg.featureType,
                {{layerCfg.featureType + "Id", building.id}});

            // Create axis-aligned rectangle as mesh (two triangles)
            feature->addMesh({
                Point(building.minX, building.minY, 0.0),
                Point(building.maxX, building.minY, 0.0),
                Point(building.maxX, building.maxY, 0.0)
            });
            feature->addMesh({
                Point(building.minX, building.minY, 0.0),
                Point(building.maxX, building.maxY, 0.0),
                Point(building.minX, building.maxY, 0.0)
            });

            // Generate attributes
            std::mt19937 gen(ctx.seed + building.id);
            generateAttributes(feature, layerCfg.topAttributes, gen, building.id);
            generateLayeredAttributes(feature, layerCfg.layeredAttributes, gen, building.id);

            // Profile-based attribute tree generation
            auto profile = layerCfg.attributeTreeProfile.value_or(cfg.attributeTreeProfile);
            if (profile != AttributeTreeProfile::None) {
                auto params = ResolvedTreeParams::resolve(profile, cfg.attributeTreeParams, layerCfg.attributeTreeParams);
                generateProfileAttributes(feature, tile, params, gen, building.id);
            }
        }
        return;
    }

    // Check if we have blocks to fill
    if (ctx.blocks.empty()) {
        mapget::log().warn("  No blocks available for building generation");
        return;
    }

    // Convert meters to degrees
    const auto lowerLeft = tile->tileId().sw();
    const auto upperRight = tile->tileId().ne();
    double metersPerDegree = 111320.0;
    double avgLat = (lowerLeft.y + upperRight.y) / 2.0;
    double metersPerDegreeLon = metersPerDegree * std::cos(avgLat * glm::pi<double>() / 180.0);

    const double setbackMeters = 5.0;  // Building setback from block edge
    const double gapMeters = 3.0;      // Gap between buildings
    const double setback = setbackMeters / metersPerDegree;
    const double gap = gapMeters / metersPerDegree;

    std::mt19937 gen(ctx.seed + 1000);
    std::uniform_real_distribution<> sizeDist(layerCfg.geometry.sizeRange[0], layerCfg.geometry.sizeRange[1]);
    std::uniform_real_distribution<> aspectDist(layerCfg.geometry.aspectRatio[0], layerCfg.geometry.aspectRatio[1]);

    uint32_t buildingId = 100;
    int totalBuildings = 0;

    mapget::log().info("  Building generation: filling {} blocks", ctx.blocks.size());

    // Fill each block with buildings
    for (const auto& block : ctx.blocks) {
        // Apply setback to get usable area
        double usableMinX = block.minX + setback;
        double usableMaxX = block.maxX - setback;
        double usableMinY = block.minY + setback;
        double usableMaxY = block.maxY - setback;

        if (usableMaxX <= usableMinX || usableMaxY <= usableMinY) {
            continue;  // Block too small after setback
        }

        // Fill block with buildings in rows
        double row_y = usableMinY;
        while (row_y < usableMaxY) {
            double col_x = usableMinX;
            double maxHeightInRow = 0.0;

            while (col_x < usableMaxX) {
                // Generate building size
                double buildingWidthMeters = sizeDist(gen);
                double aspect = aspectDist(gen);
                double buildingHeightMeters = buildingWidthMeters * aspect;

                double buildingWidth = buildingWidthMeters / metersPerDegreeLon;
                double buildingHeight = buildingHeightMeters / metersPerDegree;

                // Check if building fits
                if (col_x + buildingWidth > usableMaxX || row_y + buildingHeight > usableMaxY) {
                    break;
                }

                // Create building
                Building building;
                building.minX = col_x;
                building.maxX = col_x + buildingWidth;
                building.minY = row_y;
                building.maxY = row_y + buildingHeight;
                building.id = buildingId++;

                ctx.buildings.push_back(building);
                totalBuildings++;

                // Create feature
                auto feature = tile->newFeature(layerCfg.featureType,
                    {{layerCfg.featureType + "Id", building.id}});

                feature->addMesh({
                    Point(building.minX, building.minY, 0.0),
                    Point(building.maxX, building.minY, 0.0),
                    Point(building.maxX, building.maxY, 0.0)
                });
                feature->addMesh({
                    Point(building.minX, building.minY, 0.0),
                    Point(building.maxX, building.maxY, 0.0),
                    Point(building.minX, building.maxY, 0.0)
                });

                // Generate attributes
                std::mt19937 attrGen(ctx.seed + building.id);
                generateAttributes(feature, layerCfg.topAttributes, attrGen, building.id);
                generateLayeredAttributes(feature, layerCfg.layeredAttributes, attrGen, building.id);

                // Profile-based attribute tree generation
                auto profile = layerCfg.attributeTreeProfile.value_or(cfg.attributeTreeProfile);
                if (profile != AttributeTreeProfile::None) {
                    auto params = ResolvedTreeParams::resolve(profile, cfg.attributeTreeParams, layerCfg.attributeTreeParams);
                    generateProfileAttributes(feature, tile, params, attrGen, building.id);
                }

                // Generate relations
                Point buildingCenter((building.minX + building.maxX) / 2.0,
                                    (building.minY + building.maxY) / 2.0, 0.0);
                generateRelations(feature, ctx, layerCfg.relations, buildingCenter);

                // Move to next column
                col_x += buildingWidth + gap;
                maxHeightInRow = std::max(maxHeightInRow, buildingHeight);
            }

            // Move to next row
            row_y += maxHeightInRow + gap;
        }
    }

    mapget::log().info("  Building generation complete: created {} buildings in {} blocks",
                       totalBuildings, ctx.blocks.size());
}

void GridDataSource::generateRoadGrid(TileSpatialContext& ctx,
                                      const LayerConfig& config,
                                      TileFeatureLayer::Ptr const& tile) {
    // Use std::call_once for thread-safe, exception-safe one-time initialization
    // This handles shutdown edge cases better than manual mutex + flag
    std::call_once(ctx.gridGeneratedOnce, [&]() {
        const auto lowerLeft = tile->tileId().sw();
        const auto upperRight = tile->tileId().ne();

        // Convert meters to degrees
        double metersPerDegree = 111320.0;
        double avgLat = (lowerLeft.y + upperRight.y) / 2.0;
        double metersPerDegreeLon = metersPerDegree * std::cos(avgLat * glm::pi<double>() / 180.0);

        // Block size in meters (from config or default)
        const double blockSizeMeters = 80.0;  // ~80m blocks (typical US city block)
        const double roadWidthMeters = 10.0;  // ~10m road width
        const double skipProbability = 0.20;  // 20% chance to skip a road

        double blockSize = blockSizeMeters / metersPerDegree;
        double roadWidth = roadWidthMeters / metersPerDegree;
        double spacing = blockSize + roadWidth;

        mapget::log().info("  Road grid generation: block size {}m, road width {}m, skip probability {}%",
                           blockSizeMeters, roadWidthMeters, static_cast<int>(skipProbability * 100));

        std::mt19937 gen(ctx.seed);
        std::uniform_real_distribution<> skipDist(0.0, 1.0);

        // Generate horizontal road lines (constant Y) with skip probability
        double y = lowerLeft.y;
        while (y <= upperRight.y) {
            if (skipDist(gen) >= skipProbability) {
                ctx.horizontalRoadY.push_back(y);
            }
            y += spacing;
        }

        // Generate vertical road lines (constant X) with skip probability
        double x = lowerLeft.x;
        while (x <= upperRight.x) {
            if (skipDist(gen) >= skipProbability) {
                ctx.verticalRoadX.push_back(x);
            }
            x += spacing;
        }

        mapget::log().info("  Road grid: {} horizontal roads, {} vertical roads",
                           ctx.horizontalRoadY.size(), ctx.verticalRoadX.size());

        // Create intersections at all crossing points
        uint32_t intersectionId = 100;
        std::map<std::pair<size_t, size_t>, uint32_t> intersectionMap;  // (hIdx, vIdx) -> intersectionId

        for (size_t i = 0; i < ctx.horizontalRoadY.size(); ++i) {
            for (size_t j = 0; j < ctx.verticalRoadX.size(); ++j) {
                Intersection intersection;
                intersection.position = Point(ctx.verticalRoadX[j], ctx.horizontalRoadY[i], 0.0);
                intersection.id = intersectionId;
                intersectionMap[{i, j}] = intersectionId;
                ctx.intersections.push_back(intersection);
                intersectionId++;
            }
        }

        // Generate road segments between intersections
        uint32_t roadId = 1000;

        // Horizontal road segments (along each horizontal line, between vertical intersections)
        for (size_t i = 0; i < ctx.horizontalRoadY.size(); ++i) {
            for (size_t j = 0; j + 1 < ctx.verticalRoadX.size(); ++j) {
                RoadSegment road;
                road.start = Point(ctx.verticalRoadX[j], ctx.horizontalRoadY[i], 0.0);
                road.end = Point(ctx.verticalRoadX[j + 1], ctx.horizontalRoadY[i], 0.0);
                road.id = roadId++;
                road.startIntersectionId = intersectionMap[{i, j}];
                road.endIntersectionId = intersectionMap[{i, j + 1}];

                // Add road to intersection's connected roads
                for (auto& intersection : ctx.intersections) {
                    if (intersection.id == road.startIntersectionId || intersection.id == road.endIntersectionId) {
                        intersection.connectedRoadIds.push_back(road.id);
                    }
                }

                ctx.roads.push_back(road);
            }
        }

        // Vertical road segments (along each vertical line, between horizontal intersections)
        for (size_t j = 0; j < ctx.verticalRoadX.size(); ++j) {
            for (size_t i = 0; i + 1 < ctx.horizontalRoadY.size(); ++i) {
                RoadSegment road;
                road.start = Point(ctx.verticalRoadX[j], ctx.horizontalRoadY[i], 0.0);
                road.end = Point(ctx.verticalRoadX[j], ctx.horizontalRoadY[i + 1], 0.0);
                road.id = roadId++;
                road.startIntersectionId = intersectionMap[{i, j}];
                road.endIntersectionId = intersectionMap[{i + 1, j}];

                // Add road to intersection's connected roads
                for (auto& intersection : ctx.intersections) {
                    if (intersection.id == road.startIntersectionId || intersection.id == road.endIntersectionId) {
                        intersection.connectedRoadIds.push_back(road.id);
                    }
                }

                ctx.roads.push_back(road);
            }
        }

        // Extract blocks between roads
        for (size_t i = 0; i + 1 < ctx.horizontalRoadY.size(); ++i) {
            for (size_t j = 0; j + 1 < ctx.verticalRoadX.size(); ++j) {
                Block block;
                block.minX = ctx.verticalRoadX[j] + roadWidth / 2.0;
                block.maxX = ctx.verticalRoadX[j + 1] - roadWidth / 2.0;
                block.minY = ctx.horizontalRoadY[i] + roadWidth / 2.0;
                block.maxY = ctx.horizontalRoadY[i + 1] - roadWidth / 2.0;
                ctx.blocks.push_back(block);
            }
        }

        mapget::log().info("  Generated {} intersections, {} road segments, {} blocks",
                           ctx.intersections.size(), ctx.roads.size(), ctx.blocks.size());
    });
}

void GridDataSource::generateRoads(TileSpatialContext& ctx,
                                   const LayerConfig& layerCfg,
                                   const gridsource::Config& cfg,
                                   TileFeatureLayer::Ptr const& tile) {
    // Lazily generate road grid structure first
    generateRoadGrid(ctx, layerCfg, tile);

    // Create road features (only if not already done)
    if (ctx.roads.empty()) {
        mapget::log().error("  Road grid generated but no roads created!");
        return;
    }

    mapget::log().info("  Creating {} road features with type '{}'", ctx.roads.size(), layerCfg.featureType);

    // Recreate features for all roads
    for (const auto& road : ctx.roads) {
        auto feature = tile->newFeature(layerCfg.featureType,
            {{layerCfg.featureType + "Id", road.id}});

        // Create straight line (no jitter for grid roads)
        auto line = feature->geom()->newGeometry(GeomType::Line, 2);
        line->append(road.start);
        if (!road.intermediatePoints.empty()) {
            for (const auto& pt : road.intermediatePoints) {
                line->append(pt);
            }
        }
        line->append(road.end);

        // Add relations to start and end intersections
        if (road.startIntersectionId > 0) {
            feature->addRelation("startIntersection", "DevSrc-Intersection", {
                {"tileId", static_cast<int64_t>(tile->tileId().value_)},
                {"DevSrc-IntersectionId", static_cast<int64_t>(road.startIntersectionId)}
            });
        }
        if (road.endIntersectionId > 0) {
            feature->addRelation("endIntersection", "DevSrc-Intersection", {
                {"tileId", static_cast<int64_t>(tile->tileId().value_)},
                {"DevSrc-IntersectionId", static_cast<int64_t>(road.endIntersectionId)}
            });
        }

        // Generate attributes
        std::mt19937 gen(ctx.seed + road.id);
        generateAttributes(feature, layerCfg.topAttributes, gen, road.id);
        generateLayeredAttributes(feature, layerCfg.layeredAttributes, gen, road.id);

        // Profile-based attribute tree generation
        auto profile = layerCfg.attributeTreeProfile.value_or(cfg.attributeTreeProfile);
        if (profile != AttributeTreeProfile::None) {
            auto params = ResolvedTreeParams::resolve(profile, cfg.attributeTreeParams, layerCfg.attributeTreeParams);
            generateProfileAttributes(feature, tile, params, gen, road.id);
        }

        // Generate relations
        Point roadMidpoint((road.start.x + road.end.x) / 2.0,
                          (road.start.y + road.end.y) / 2.0, 0.0);
        generateRelations(feature, ctx, layerCfg.relations, roadMidpoint);
    }
}

void GridDataSource::generateIntersections(TileSpatialContext& ctx,
                                          const LayerConfig& layerCfg,
                                          const gridsource::Config& cfg,
                                          TileFeatureLayer::Ptr const& tile) {
    // Lazily generate road grid first (which creates intersections)
    generateRoadGrid(ctx, layerCfg, tile);

    if (ctx.intersections.empty()) {
        mapget::log().warn("  No intersections to generate");
        return;
    }

    // Create intersection features
    for (const auto& intersection : ctx.intersections) {
        auto feature = tile->newFeature(layerCfg.featureType,
            {{layerCfg.featureType + "Id", intersection.id}});

        // Create point geometry (Points type with single point)
        auto points = feature->geom()->newGeometry(GeomType::Points, 1);
        points->append(intersection.position);

        // Add relations to connected roads
        for (uint32_t roadId : intersection.connectedRoadIds) {
            feature->addRelation("connectedRoad", "DevSrc-Road", {
                {"tileId", static_cast<int64_t>(tile->tileId().value_)},
                {"DevSrc-RoadId", static_cast<int64_t>(roadId)}
            });
        }

        // Generate attributes
        std::mt19937 gen(ctx.seed + intersection.id);
        generateAttributes(feature, layerCfg.topAttributes, gen, intersection.id);

        // Profile-based attribute tree generation
        auto profile = layerCfg.attributeTreeProfile.value_or(cfg.attributeTreeProfile);
        if (profile != AttributeTreeProfile::None) {
            auto params = ResolvedTreeParams::resolve(profile, cfg.attributeTreeParams, layerCfg.attributeTreeParams);
            generateProfileAttributes(feature, tile, params, gen, intersection.id);
        }
    }

    mapget::log().info("  Created {} intersection features", ctx.intersections.size());
}

void GridDataSource::generateAttributes(model_ptr<Feature> feature,
                                        const std::vector<AttributeConfig>& attrs,
                                        std::mt19937& gen,
                                        uint32_t featureId) {
    if (attrs.empty()) return;

    std::map<std::string, std::string> computedValues;

    for (const auto& attr : attrs) {
        std::string value = generateAttributeValue(attr, gen, featureId, computedValues);
        computedValues[attr.name] = value;

        // Add to feature based on data type
        switch (attr.dataType) {
            case DataType::Int:
                feature->attributes()->addField(attr.name, static_cast<int64_t>(std::stoll(value)));
                break;
            case DataType::Int64:
                feature->attributes()->addField(attr.name, static_cast<int64_t>(std::stoll(value)));
                break;
            case DataType::UInt16:
                feature->attributes()->addField(attr.name, static_cast<uint16_t>(std::stoul(value)));
                break;
            case DataType::UInt32:
                feature->attributes()->addField(attr.name, static_cast<int64_t>(std::stoul(value)));
                break;
            case DataType::Float:
                feature->attributes()->addField(attr.name, std::stod(value));
                break;
            case DataType::Bool:
                feature->attributes()->addField(attr.name, static_cast<uint16_t>(value == "true" || value == "1" ? 1 : 0));
                break;
            case DataType::String:
                feature->attributes()->addField(attr.name, value);
                break;
        }
    }
}

void GridDataSource::generateLayeredAttributes(model_ptr<Feature> feature,
                                               const std::vector<AttributeLayerConfig>& layers,
                                               std::mt19937& gen,
                                               uint32_t featureId) {
    // Placeholder: Full implementation would generate layered attributes with validity
    // For now, just create the layers without validity
    for (const auto& layerCfg : layers) {
        auto attrLayer = feature->attributeLayers()->newLayer(layerCfg.layerName);

        for (const auto& attrCfg : layerCfg.attributes) {
            auto attr = attrLayer->newAttribute(attrCfg.name);

            // Generate fields
            std::map<std::string, std::string> computedValues;
            for (const auto& field : attrCfg.fields) {
                std::string value = generateAttributeValue(field, gen, featureId, computedValues);
                computedValues[field.name] = value;

                switch (field.dataType) {
                    case DataType::Int:
                        attr->addField(field.name, static_cast<int64_t>(std::stoll(value)));
                        break;
                    case DataType::Int64:
                        attr->addField(field.name, static_cast<int64_t>(std::stoll(value)));
                        break;
                    case DataType::UInt16:
                        attr->addField(field.name, static_cast<uint16_t>(std::stoul(value)));
                        break;
                    case DataType::UInt32:
                        attr->addField(field.name, static_cast<int64_t>(std::stoul(value)));
                        break;
                    case DataType::Float:
                        attr->addField(field.name, std::stod(value));
                        break;
                    case DataType::String:
                        attr->addField(field.name, value);
                        break;
                    case DataType::Bool:
                        attr->addField(field.name, static_cast<uint16_t>(value == "true" || value == "1" ? 1 : 0));
                        break;
                }
            }
        }
    }
}

void GridDataSource::generateRelations(model_ptr<Feature> feature,
                                       const TileSpatialContext& ctx,
                                       const std::vector<RelationConfig>& relations,
                                       Point featurePoint) {
    // TODO: Relations API needs investigation - relations() is protected
    // Commenting out for initial implementation
    (void)feature;
    (void)ctx;
    (void)relations;
    (void)featurePoint;
}

std::string GridDataSource::generateAttributeValue(const AttributeConfig& attr,
                                                   std::mt19937& gen,
                                                   uint32_t featureId,
                                                   const std::map<std::string, std::string>& computedValues) {
    std::ostringstream oss;

    switch (attr.generator) {
        case GeneratorType::Fixed:
            return attr.fixedValue;

        case GeneratorType::Sequential: {
            std::string result = attr.templateStr;
            result = replaceAll(result, "{id}", std::to_string(attr.startFrom + featureId));
            return result;
        }

        case GeneratorType::Random: {
            if (!attr.stringValues.empty()) {
                // Random selection from string values
                std::discrete_distribution<> dist(attr.weights.begin(), attr.weights.end());
                size_t idx = attr.weights.empty() ?
                    std::uniform_int_distribution<size_t>(0, attr.stringValues.size() - 1)(gen) :
                    dist(gen);
                return attr.stringValues[idx];
            } else {
                // Numeric random generation
                double value;
                switch (attr.distribution) {
                    case DistributionType::Normal:
                        value = std::normal_distribution<>(attr.mean, attr.stddev)(gen);
                        value = std::clamp(value, attr.min, attr.max);
                        break;
                    case DistributionType::Exponential:
                        value = std::exponential_distribution<>(attr.lambda)(gen);
                        value = attr.min + std::fmod(value, attr.max - attr.min);
                        break;
                    default:
                        value = std::uniform_real_distribution<>(attr.min, attr.max)(gen);
                        break;
                }

                if (attr.dataType == DataType::Int || attr.dataType == DataType::UInt16 ||
                    attr.dataType == DataType::UInt32 || attr.dataType == DataType::Int64) {
                    oss << static_cast<int>(value);
                } else {
                    oss << std::fixed << std::setprecision(2) << value;
                }
                return oss.str();
            }
        }

        case GeneratorType::Computed: {
            // Simple formula evaluation
            if (attr.formula == "geometryLength") {
                // This would need actual geometry - return placeholder
                return "100";
            }
            // For other formulas, try to evaluate from computedValues
            // This is a simplified implementation
            return "0";
        }

        case GeneratorType::Zoned: {
            // Placeholder for zoned generation
            if (!attr.zones.empty()) {
                size_t idx = std::uniform_int_distribution<size_t>(0, attr.zones.size() - 1)(gen);
                oss << attr.zones[idx];
                return oss.str();
            }
            return "0";
        }

        default:
            return "0";
    }
}

// ============================================================================
// Profile-based Attribute Tree Generation
// ============================================================================

namespace {

// Recursive helper to generate a nested value for attribute tree profiles
simfil::ModelNode::Ptr generateNestedValue(
    TileFeatureLayer::Ptr const& tile,
    const ResolvedTreeParams& params,
    uint32_t seed,
    int depth,
    int fieldIdx)
{
    uint32_t h = profileHash(seed, static_cast<uint32_t>(depth), static_cast<uint32_t>(fieldIdx), 0);

    // Check if we should nest deeper
    bool shouldNest = (depth < params.maxNestingDepth) &&
                      ((h % 1000) < static_cast<uint32_t>(params.nestingProbability * 1000));

    if (shouldNest) {
        // Check if we should create an array instead of object
        bool shouldArray = (params.maxArraySize > 0) && ((h % 3) == 0);
        if (shouldArray) {
            int arraySize = 1 + static_cast<int>(h % static_cast<uint32_t>(params.maxArraySize));
            auto arr = tile->newArray(static_cast<size_t>(arraySize));
            for (int i = 0; i < arraySize; ++i) {
                // Array elements are scalars
                uint32_t elemH = profileHash(seed, static_cast<uint32_t>(depth), static_cast<uint32_t>(fieldIdx), static_cast<uint32_t>(i));
                int typeRotation = static_cast<int>(elemH % 4);
                switch (typeRotation) {
                    case 0: arr->append(tile->newValue(std::string(kProfileStringValues[elemH % kNumProfileStringValues]))); break;
                    case 1: arr->append(tile->newValue(static_cast<int64_t>(kProfileIntValues[elemH % kNumProfileIntValues]))); break;
                    case 2: arr->append(tile->newValue(kProfileFloatValues[elemH % kNumProfileFloatValues])); break;
                    case 3: arr->append(tile->newSmallValue((elemH & 1) != 0)); break;
                }
            }
            return arr;
        } else {
            // Nested object
            int numSubFields = std::min(params.fieldsPerAttr, 4);
            auto obj = tile->newObject(static_cast<size_t>(numSubFields));
            for (int sf = 0; sf < numSubFields; ++sf) {
                uint32_t subH = profileHash(seed, static_cast<uint32_t>(depth + 1), static_cast<uint32_t>(sf), 0);
                auto fieldName = kProfileFieldNames[subH % kNumProfileFieldNames];
                auto val = generateNestedValue(tile, params, seed ^ static_cast<uint32_t>(sf * 7919), depth + 1, sf);
                if (val)
                    obj->addField(fieldName, val);
            }
            return obj;
        }
    }

    // Scalar value: rotate type based on hash
    int typeRotation = static_cast<int>(h % 4);
    switch (typeRotation) {
        case 0: return tile->newValue(std::string(kProfileStringValues[h % kNumProfileStringValues]));
        case 1: return tile->newValue(static_cast<int64_t>(kProfileIntValues[h % kNumProfileIntValues]));
        case 2: return tile->newValue(kProfileFloatValues[h % kNumProfileFloatValues]);
        case 3: return tile->newSmallValue((h & 1) != 0);
    }
    return tile->newValue(static_cast<int64_t>(0));
}

}  // anonymous namespace

void GridDataSource::generateProfileAttributes(
    model_ptr<Feature> feature,
    TileFeatureLayer::Ptr const& tile,
    const ResolvedTreeParams& params,
    std::mt19937& gen,
    uint32_t featureId)
{
    uint32_t baseSeed = static_cast<uint32_t>(gen());

    // Step 1: Add top-level extra scalar fields to feature->attributes()
    for (int i = 0; i < params.topLevelExtraFields; ++i) {
        uint32_t h = profileHash(baseSeed, 0, static_cast<uint32_t>(i), 0);
        auto fieldName = kProfileFieldNames[h % kNumProfileFieldNames];
        int typeRotation = static_cast<int>(h % 4);
        switch (typeRotation) {
            case 0: feature->attributes()->addField(fieldName, std::string(kProfileStringValues[h % kNumProfileStringValues])); break;
            case 1: feature->attributes()->addField(fieldName, static_cast<int64_t>(kProfileIntValues[h % kNumProfileIntValues])); break;
            case 2: feature->attributes()->addField(fieldName, kProfileFloatValues[h % kNumProfileFloatValues]); break;
            case 3: feature->attributes()->addField(fieldName, static_cast<uint16_t>((h & 1) ? 1 : 0)); break;
        }
    }

    // Step 2: Create attribute layers with semantic names
    for (int layerIdx = 0; layerIdx < params.numLayers; ++layerIdx) {
        auto layerName = kProfileLayerNames[layerIdx % kNumProfileLayerNames];
        auto attrLayer = feature->attributeLayers()->newLayer(layerName);

        // Step 3: Create attributes per layer
        for (int attrIdx = 0; attrIdx < params.attrsPerLayer; ++attrIdx) {
            uint32_t attrSeed = profileHash(baseSeed, static_cast<uint32_t>(layerIdx), static_cast<uint32_t>(attrIdx), featureId);
            auto attrName = kProfileAttrNames[attrSeed % kNumProfileAttrNames];
            auto attr = attrLayer->newAttribute(attrName);

            // Step 4: Roll validity
            uint32_t validH = profileHash(attrSeed, 999, 0, 0);
            double validRoll = (validH % 1000) / 1000.0;
            if (validRoll < params.directionalValidityProb) {
                auto direction = ((validH >> 10) & 1) ? Validity::Positive : Validity::Negative;
                attr->validity()->newDirection(direction);
            } else if (validRoll < params.directionalValidityProb + params.rangeValidityProb) {
                double startFrac = (validH % 100) / 100.0;
                double endFrac = startFrac + ((validH >> 8) % 50) / 100.0;
                if (endFrac > 1.0) endFrac = 1.0;
                attr->validity()->newRange(
                    Validity::RelativeLengthOffset,
                    startFrac, endFrac, {},
                    Validity::Both);
            }

            // Step 5: Generate fields per attribute
            for (int fieldIdx = 0; fieldIdx < params.fieldsPerAttr; ++fieldIdx) {
                uint32_t fieldSeed = profileHash(attrSeed, static_cast<uint32_t>(fieldIdx), featureId, 0);
                auto fieldName = kProfileFieldNames[fieldSeed % kNumProfileFieldNames];
                auto val = generateNestedValue(tile, params, fieldSeed, 0, fieldIdx);
                if (val)
                    attr->addField(fieldName, val);
            }
        }
    }
}
