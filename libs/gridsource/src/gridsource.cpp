// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "gridsource/gridsource.h"

#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <limits>
#include <set>

#include "mapget/log.h"
#include "fmt/format.h"
#include "glm/ext.hpp"

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

[[noreturn]] void trafficConfigError(const std::string& message) {
    throw std::runtime_error("Invalid Grid traffic configuration: " + message);
}

/** SplitMix64 finalizer used by the stable Grid traffic sampling contract. */
uint64_t stableMix64(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

uint64_t stableCombine(uint64_t state, uint64_t value) {
    return stableMix64(state ^ stableMix64(value));
}

struct TrafficSample {
    uint32_t roadId;
    const char* flow;
    uint32_t freeFlowSpeedKph;
    uint32_t estimatedAverageSpeedKph;
    uint32_t relativeSpeedPercent;
};

TrafficSample makeTrafficSample(
    uint32_t seed,
    uint32_t packedTileId,
    uint32_t roadId,
    int64_t epoch) {
    uint64_t roadBase = stableCombine(seed, packedTileId);
    roadBase = stableCombine(roadBase, roadId);
    const auto sample = stableCombine(roadBase, static_cast<uint64_t>(epoch));
    const auto percentile = static_cast<uint32_t>(sample % 1000U);

    const char* flow = "UNKNOWN";
    uint32_t relativeMin = 40;
    uint32_t relativeSpan = 21;
    if (percentile < 10) {
        flow = "NO_TRAFFIC_FLOW";
        relativeMin = 0;
        relativeSpan = 1;
    } else if (percentile < 30) {
        flow = "STATIONARY_TRAFFIC";
        relativeMin = 1;
        relativeSpan = 5;
    } else if (percentile < 90) {
        flow = "QUEUING_TRAFFIC";
        relativeMin = 6;
        relativeSpan = 15;
    } else if (percentile < 200) {
        flow = "SLOW_TRAFFIC";
        relativeMin = 21;
        relativeSpan = 25;
    } else if (percentile < 400) {
        flow = "HEAVY_TRAFFIC";
        relativeMin = 46;
        relativeSpan = 30;
    } else if (percentile < 980) {
        flow = "FREE_TRAFFIC";
        relativeMin = 76;
        relativeSpan = 25;
    }

    const auto freeFlow = 40U + static_cast<uint32_t>(roadBase % 91U);
    const auto relative = relativeMin + static_cast<uint32_t>((sample >> 16U) % relativeSpan);
    return {
        roadId,
        flow,
        freeFlow,
        static_cast<uint32_t>((static_cast<uint64_t>(freeFlow) * relative) / 100U),
        relative};
}

nlohmann::json trafficFeatureSchema(const LayerConfig& layer) {
    const auto& traffic = *layer.traffic;
    nlohmann::json schema = {
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"type", "object"},
        {"required", nlohmann::json::array({"type", "typeId", "properties"})},
        {"additionalProperties", true}};
    schema["properties"]["type"] = {{"const", "Feature"}};
    schema["properties"]["typeId"] = {{"const", layer.featureType}};
    schema["properties"]["properties"] = {
        {"type", "object"},
        {"required", nlohmann::json::array({
            "trafficFlow",
            "estimatedAverageSpeedKph",
            "freeFlowSpeedKph",
            "relativeSpeedPercent",
            "trafficEpoch"})},
        {"additionalProperties", true}};
    auto& fields = schema["properties"]["properties"]["properties"];
    fields["trafficFlow"] = {
        {"type", "string"},
        {"enum", nlohmann::json::array({
            "UNKNOWN", "FREE_TRAFFIC", "HEAVY_TRAFFIC", "SLOW_TRAFFIC",
            "QUEUING_TRAFFIC", "STATIONARY_TRAFFIC", "NO_TRAFFIC_FLOW"})}};
    fields["estimatedAverageSpeedKph"] = {{"type", "integer"}, {"minimum", 0}};
    fields["freeFlowSpeedKph"] = {{"type", "integer"}, {"minimum", 1}};
    fields["relativeSpeedPercent"] = {
        {"type", "integer"}, {"minimum", 0}, {"maximum", 100}};
    fields["trafficEpoch"] = {{"type", "integer"}};
    schema["properties"]["relations"] = {
        {"type", "array"},
        {"description", "Contains a 'road' relation to " + traffic.roadFeatureType + "."},
        {"items", {{"type", "object"}, {"additionalProperties", true}}}};
    return schema;
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

TrafficConfig TrafficConfig::fromYAML(const YAML::Node& node) {
    if (!node || !node.IsMap()) {
        trafficConfigError("'traffic' must be an object.");
    }

    static const std::set<std::string> allowedKeys = {
        "roadLayer", "tileLevel", "updateIntervalSeconds", "seed"};
    for (const auto& entry : node) {
        const auto key = entry.first.as<std::string>();
        if (!allowedKeys.contains(key)) {
            trafficConfigError("unknown traffic field '" + key + "'.");
        }
    }

    TrafficConfig cfg;
    if (!node["roadLayer"] || !node["roadLayer"].IsScalar()) {
        trafficConfigError("'traffic.roadLayer' is required and must be a string.");
    }
    cfg.roadLayer = node["roadLayer"].as<std::string>();
    if (cfg.roadLayer.empty()) {
        trafficConfigError("'traffic.roadLayer' must not be empty.");
    }

    try {
        cfg.tileLevel = node["tileLevel"].as<int>(13);
        cfg.updateIntervalSeconds = node["updateIntervalSeconds"].as<uint32_t>(5);
        if (node["seed"]) {
            const auto seed = node["seed"].as<uint64_t>();
            if (seed > std::numeric_limits<uint32_t>::max()) {
                trafficConfigError("'traffic.seed' must fit in an unsigned 32-bit integer.");
            }
            cfg.seed = static_cast<uint32_t>(seed);
        }
    } catch (const YAML::Exception& error) {
        trafficConfigError(std::string("traffic numeric fields must be integers: ") + error.what());
    }

    if (cfg.tileLevel < 13 || cfg.tileLevel > 15) {
        trafficConfigError("'traffic.tileLevel' must be between 13 and 15.");
    }
    if (cfg.updateIntervalSeconds < 1 || cfg.updateIntervalSeconds > 60) {
        trafficConfigError("'traffic.updateIntervalSeconds' must be between 1 and 60.");
    }
    return cfg;
}

LayerConfig LayerConfig::fromYAML(const YAML::Node& node) {
    LayerConfig cfg;
    if (!node) return cfg;

    cfg.name = node["name"].as<std::string>("");
    cfg.enabled = node["enabled"].as<bool>(true);
    cfg.featureType = node["featureType"].as<std::string>("");

    const auto kind = node["kind"].as<std::string>("auto");
    if (kind == "auto") {
        cfg.kind = LayerKind::Auto;
        if (node["traffic"].IsDefined()) {
            trafficConfigError("layer '" + cfg.name + "' uses 'traffic' with kind 'auto'.");
        }
    } else if (kind == "traffic") {
        cfg.kind = LayerKind::Traffic;
        if (!node["traffic"].IsDefined()) {
            trafficConfigError("layer '" + cfg.name + "' requires a 'traffic' object.");
        }
        if (node["attributes"].IsDefined() || node["relations"].IsDefined()) {
            trafficConfigError(
                "layer '" + cfg.name + "' cannot define generic attributes or relations.");
        }
        if (node["geometry"].IsDefined()) {
            if (!node["geometry"].IsMap()) {
                trafficConfigError("traffic geometry must be an object.");
            }
            for (const auto& entry : node["geometry"]) {
                if (entry.first.as<std::string>() != "type") {
                    trafficConfigError(
                        "traffic geometry supports only the untuned 'type: line' field.");
                }
            }
            if (node["geometry"]["type"].as<std::string>("line") != "line") {
                trafficConfigError("traffic geometry must be 'line'.");
            }
        }
        cfg.traffic = TrafficConfig::fromYAML(node["traffic"]);
    } else {
        trafficConfigError("unknown layer kind '" + kind + "'.");
    }

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

    return cfg;
}

Config Config::fromYAML(const YAML::Node& node) {
    Config cfg;
    if (!node) return cfg;

    cfg.mapId = node["mapId"].as<std::string>("GridDataSource");
    cfg.spatialCoherence = node["spatialCoherence"].as<bool>(true);
    cfg.collisionGridSize = node["collisionGridSize"].as<double>(10.0);

    if (node["layers"]) {
        std::vector<LayerConfig> parsedLayers;
        for (const auto& layer : node["layers"]) {
            parsedLayers.push_back(LayerConfig::fromYAML(layer));
        }

        for (auto& layer : parsedLayers) {
            if (layer.kind != LayerKind::Traffic) {
                continue;
            }
            if (layer.name.empty() || layer.featureType.empty()) {
                trafficConfigError("traffic layer name and featureType must not be empty.");
            }

            size_t nameMatches = 0;
            size_t typeMatches = 0;
            for (const auto& candidate : parsedLayers) {
                nameMatches += candidate.name == layer.name ? 1U : 0U;
                typeMatches += candidate.featureType == layer.featureType ? 1U : 0U;
            }
            if (nameMatches != 1) {
                trafficConfigError("traffic layer name '" + layer.name + "' must be unique.");
            }
            if (typeMatches != 1) {
                trafficConfigError(
                    "traffic featureType '" + layer.featureType + "' must be unique.");
            }

            std::vector<LayerConfig*> targets;
            for (auto& candidate : parsedLayers) {
                if (candidate.name == layer.traffic->roadLayer) {
                    targets.push_back(&candidate);
                }
            }
            if (targets.size() != 1) {
                trafficConfigError(
                    "traffic roadLayer '" + layer.traffic->roadLayer + "' must resolve exactly once.");
            }
            auto& target = *targets.front();
            if (&target == &layer) {
                trafficConfigError("traffic roadLayer cannot reference itself.");
            }
            if (!target.enabled) {
                trafficConfigError("traffic roadLayer '" + target.name + "' is disabled.");
            }
            if (target.kind == LayerKind::Traffic || target.geometry.type != GeometryType::Line) {
                trafficConfigError(
                    "traffic roadLayer '" + target.name + "' must be a non-traffic line layer.");
            }
            if (target.featureType.empty()) {
                trafficConfigError("traffic roadLayer featureType must not be empty.");
            }
            layer.traffic->roadFeatureType = target.featureType;
            layer.traffic->roadFeatureIdPart = target.featureType + "Id";
        }

        for (auto& layer : parsedLayers) {
            if (layer.enabled) {
                cfg.layers.push_back(std::move(layer));
            }
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
    seed = static_cast<uint32_t>(hasher(tid.value()));
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

GridDataSource::GridDataSource(const YAML::Node& config, Clock clock)
    : clock_(std::move(clock)) {
    if (!clock_) {
        throw std::invalid_argument("GridDataSource requires a valid clock provider.");
    }
    if (config && config.IsMap()) {
        config_ = Config::fromYAML(config);
    } else {
        // Default configuration with roads and buildings
        config_.mapId = "GridDataSource";
        config_.spatialCoherence = true;
        config_.collisionGridSize = 10.0;

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
        config_.layers.push_back(buildingLayer);

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
        config_.layers.push_back(roadLayer);

        // Default intersection layer
        LayerConfig intersectionLayer;
        intersectionLayer.name = "DevSrc-IntersectionLayer";
        intersectionLayer.enabled = true;
        intersectionLayer.featureType = "DevSrc-Intersection";
        intersectionLayer.geometry.type = GeometryType::Point;
        config_.layers.push_back(intersectionLayer);
    }
    staticRetainedMemoryBytes_ = computeStaticRetainedMemoryBytes();
}

DataSourceInfo GridDataSource::info() {
    nlohmann::json info;
    info["mapId"] = config_.mapId;
    info["layers"] = nlohmann::json::object();

    mapget::log().debug("GridDataSource registering {} layers", config_.layers.size());

    // Collect all unique feature types across all layers
    std::set<std::string> allFeatureTypes;
    for (const auto& layer : config_.layers) {
        allFeatureTypes.insert(layer.featureType);
    }

    // Register each layer with ALL feature types (for cross-layer relations)
    for (const auto& layer : config_.layers) {
        nlohmann::json layerInfo;
        layerInfo["featureTypes"] = nlohmann::json::array();
        if (layer.kind == LayerKind::Traffic) {
            layerInfo["zoomLevels"] = nlohmann::json::array({layer.traffic->tileLevel});
            layerInfo["featureModelSchema"] = trafficFeatureSchema(layer);
        }

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
        mapget::log().debug("  Layer '{}' with {} feature types", layer.name, allFeatureTypes.size());
    }

    return DataSourceInfo::fromJson(info);
}

std::optional<uint64_t> GridDataSource::estimatedRetainedMemoryBytes() const
{
    MemoryUsageBreakdown dynamicMemory;
    std::lock_guard lock(contextMutex_);
    dynamicMemory.add("context-index", {
        contextCache_.size() * sizeof(decltype(contextCache_)::value_type),
        contextCache_.bucket_count() * sizeof(void*) +
            contextCache_.size() *
                (sizeof(decltype(contextCache_)::value_type) + 2 * sizeof(void*)),
    });
    dynamicMemory.add("context-objects", {
        contextCache_.size() * sizeof(TileSpatialContext),
        contextCache_.size() * sizeof(TileSpatialContext),
    });
    // Context vectors are populated outside contextMutex_. Reading their
    // capacities here would race generation; process RSS keeps that mutable
    // payload visible in the unattributed remainder without making /status unsafe.
    return staticRetainedMemoryBytes_ + dynamicMemory.total().allocatedBytes;
}

uint64_t GridDataSource::computeStaticRetainedMemoryBytes() const
{
    MemoryUsageBreakdown memory;
    memory.add("object", {sizeof(GridDataSource), sizeof(GridDataSource)});
    memory.add("config-map-id", stringMemoryUsage(config_.mapId));
    memory.add("config-layers", vectorMemoryUsage(config_.layers));

    auto addAttribute = [&](AttributeConfig const& attribute) {
        memory.add("config-attribute-strings", stringMemoryUsage(attribute.name));
        memory.add("config-attribute-strings", stringMemoryUsage(attribute.templateStr));
        memory.add("config-attribute-strings", stringMemoryUsage(attribute.formula));
        memory.add("config-attribute-strings", stringMemoryUsage(attribute.fixedValue));
        memory.add("config-attribute-string-values", stringVectorMemoryUsage(attribute.stringValues));
        memory.add("config-attribute-weights", vectorMemoryUsage(attribute.weights));
        memory.add("config-attribute-zones", vectorMemoryUsage(attribute.zones));
        memory.add("config-attribute-zone-distances", vectorMemoryUsage(attribute.zoneDistances));
    };
    for (auto const& layer : config_.layers) {
        memory.add("config-layer-strings", stringMemoryUsage(layer.name));
        memory.add("config-layer-strings", stringMemoryUsage(layer.featureType));
        if (layer.traffic) {
            memory.add("config-traffic-strings", stringMemoryUsage(layer.traffic->roadLayer));
            memory.add("config-traffic-strings", stringMemoryUsage(layer.traffic->roadFeatureType));
            memory.add("config-traffic-strings", stringMemoryUsage(layer.traffic->roadFeatureIdPart));
        }
        memory.add("config-geometry-size-range", vectorMemoryUsage(layer.geometry.sizeRange));
        memory.add("config-geometry-aspect-ratio", vectorMemoryUsage(layer.geometry.aspectRatio));
        memory.add("config-top-attributes", vectorMemoryUsage(layer.topAttributes));
        for (auto const& attribute : layer.topAttributes) {
            addAttribute(attribute);
        }
        memory.add("config-attribute-layers", vectorMemoryUsage(layer.layeredAttributes));
        for (auto const& attributeLayer : layer.layeredAttributes) {
            memory.add("config-attribute-layer-names", stringMemoryUsage(attributeLayer.layerName));
            memory.add("config-layered-attributes", vectorMemoryUsage(attributeLayer.attributes));
            for (auto const& attribute : attributeLayer.attributes) {
                memory.add("config-layered-attribute-strings", stringMemoryUsage(attribute.name));
                memory.add("config-layered-attribute-strings", stringMemoryUsage(attribute.validityType));
                memory.add("config-layered-attribute-fields", vectorMemoryUsage(attribute.fields));
                for (auto const& field : attribute.fields) {
                    addAttribute(field);
                }
            }
        }
        memory.add("config-relations", vectorMemoryUsage(layer.relations));
        for (auto const& relation : layer.relations) {
            memory.add("config-relation-strings", stringMemoryUsage(relation.name));
            memory.add("config-relation-strings", stringMemoryUsage(relation.targetLayer));
            memory.add("config-relation-strings", stringMemoryUsage(relation.targetType));
            memory.add("config-relation-strings", stringMemoryUsage(relation.cardinality));
            memory.add("config-relation-strings", stringMemoryUsage(relation.validityType));
        }
    }

    return memory.total().allocatedBytes;
}

std::shared_ptr<TileSpatialContext> GridDataSource::getOrCreateContext(TileId tileId) const {
    try {
        std::lock_guard<std::mutex> lock(contextMutex_);

        auto it = contextCache_.find(tileId);
        if (it != contextCache_.end()) {
            return it->second;
        }

        // Create new context
        auto ctx = std::make_shared<TileSpatialContext>(tileId, config_.collisionGridSize);

        // LRU eviction if cache is full
        if (contextCache_.size() >= MAX_CACHED_CONTEXTS) {
            // Simple FIFO for now (could be improved with proper LRU)
            contextCache_.erase(contextCache_.begin());
        }

        contextCache_[tileId] = ctx;
        return ctx;
    } catch (const std::system_error&) {
        // Handle mutex errors during shutdown - return empty context
        return std::make_shared<TileSpatialContext>(tileId, config_.collisionGridSize);
    }
}

void GridDataSource::fill(TileFeatureLayer::Ptr const& tile) {
    const std::string layerName = tile->layerInfo()->layerId_;
    mapget::log().debug(
        "GridDataSource::fill() called for layer '{}' tile {}",
        layerName,
        tile->tileId().value());

    // Set ID prefix
    tile->setIdPrefix({{"tileId", static_cast<int64_t>(tile->tileId().value())}});

    const auto layerIt = std::find_if(
        config_.layers.begin(),
        config_.layers.end(),
        [&](const auto& layer) { return layer.name == layerName; });
    if (layerIt == config_.layers.end()) {
        mapget::log().warn("No matching Grid layer configuration found for '{}'", layerName);
        return;
    }
    const auto& layerCfg = *layerIt;

    if (layerCfg.kind == LayerKind::Traffic &&
        tile->tileId().level() != layerCfg.traffic->tileLevel) {
        tile->setError(fmt::format(
            "Grid traffic layer '{}' supports only tile level {} (requested {}).",
            layerName,
            layerCfg.traffic->tileLevel,
            tile->tileId().level()));
        tile->setTtl(std::chrono::milliseconds{0});
        return;
    }

    // Get or create spatial context only after terminal request checks.
    auto ctx = getOrCreateContext(tile->tileId());
    if (layerCfg.kind == LayerKind::Traffic) {
        generateTraffic(*ctx, layerCfg, tile);
    } else if (
        layerCfg.geometry.type == GeometryType::Polygon ||
        layerCfg.geometry.type == GeometryType::Mesh) {
        generateBuildings(*ctx, layerCfg, tile);
    } else if (layerCfg.geometry.type == GeometryType::Line) {
        generateRoads(*ctx, layerCfg, tile);
    } else if (layerCfg.geometry.type == GeometryType::Point) {
        generateIntersections(*ctx, layerCfg, tile);
    }
}

std::vector<LocateCandidate> GridDataSource::locate(const LocateRequest& req) {
    // Extract tileId from the feature ID parts
    std::optional<int64_t> tileId = req.getIntIdPart("tileId");
    if (!tileId) {
        mapget::log().warn("GridDataSource::locate() - tileId not found in feature ID");
        return {};
    }

    // Find the layer that contains this feature type
    std::string layerId;
    for (const auto& layer : config_.layers) {
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
    mapTileKey.tileId_ = TileId::fromValue(static_cast<int32_t>(*tileId));

    mapget::log().debug("GridDataSource::locate() - Found feature '{}' in tile {} layer '{}'",
                       req.typeId_, *tileId, layerId);

    return {LocateCandidate(
        std::move(mapTileKey),
        formatFeatureIdString(
            req.typeId_,
            req.featureId_))};
}

void GridDataSource::generateBuildings(TileSpatialContext& ctx,
                                       const LayerConfig& config,
                                       TileFeatureLayer::Ptr const& tile) {
    // Lazily generate road grid first (ensures roads are always generated before buildings)
    generateRoadGrid(ctx, config, tile);

    // Only generate buildings once for this tile
    if (!ctx.buildings.empty()) {
        // Buildings already generated, just recreate features
        for (const auto& building : ctx.buildings) {
            auto feature = tile->newFeature(config.featureType,
                {{config.featureType + "Id", building.id}});

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
            generateAttributes(feature, config.topAttributes, gen, building.id);
            generateLayeredAttributes(feature, config.layeredAttributes, gen, building.id);
        }
        return;
    }

    // Check if we have blocks to fill
    if (ctx.blocks.empty()) {
        mapget::log().warn("  No blocks available for building generation");
        return;
    }

    // Convert meters to degrees
    const auto lowerLeft = Point(tile->tileId().southWestWgs84());
    const auto upperRight = Point(tile->tileId().northEastWgs84());
    double metersPerDegree = 111320.0;
    double avgLat = (lowerLeft.y + upperRight.y) / 2.0;
    double metersPerDegreeLon = metersPerDegree * std::cos(avgLat * glm::pi<double>() / 180.0);

    const double setbackMeters = 5.0;  // Building setback from block edge
    const double gapMeters = 3.0;      // Gap between buildings
    const double setback = setbackMeters / metersPerDegree;
    const double gap = gapMeters / metersPerDegree;

    std::mt19937 gen(ctx.seed + 1000);
    std::uniform_real_distribution<> sizeDist(config.geometry.sizeRange[0], config.geometry.sizeRange[1]);
    std::uniform_real_distribution<> aspectDist(config.geometry.aspectRatio[0], config.geometry.aspectRatio[1]);

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
                auto feature = tile->newFeature(config.featureType,
                    {{config.featureType + "Id", building.id}});

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
                generateAttributes(feature, config.topAttributes, attrGen, building.id);
                generateLayeredAttributes(feature, config.layeredAttributes, attrGen, building.id);

                // Generate relations
                Point buildingCenter((building.minX + building.maxX) / 2.0,
                                    (building.minY + building.maxY) / 2.0, 0.0);
                generateRelations(feature, ctx, config.relations, buildingCenter);

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
        const auto lowerLeft = Point(tile->tileId().southWestWgs84());
        const auto upperRight = Point(tile->tileId().northEastWgs84());

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

        mapget::log().debug("Road grid generation: block size {}m, road width {}m, skip probability {}%",
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

        mapget::log().debug("Road grid: {} horizontal roads, {} vertical roads",
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

        mapget::log().debug("Generated {} intersections, {} road segments, {} blocks",
                           ctx.intersections.size(), ctx.roads.size(), ctx.blocks.size());
    });
}

void GridDataSource::generateRoads(TileSpatialContext& ctx,
                                   const LayerConfig& config,
                                   TileFeatureLayer::Ptr const& tile) {
    // Lazily generate road grid structure first
    generateRoadGrid(ctx, config, tile);

    // Create road features (only if not already done)
    if (ctx.roads.empty()) {
        mapget::log().error("  Road grid generated but no roads created!");
        return;
    }

    mapget::log().info("  Creating {} road features with type '{}'", ctx.roads.size(), config.featureType);

    // Recreate features for all roads
    for (const auto& road : ctx.roads) {
        auto feature = tile->newFeature(config.featureType,
            {{config.featureType + "Id", road.id}});

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
                {"tileId", static_cast<int64_t>(tile->tileId().value())},
                {"DevSrc-IntersectionId", static_cast<int64_t>(road.startIntersectionId)}
            });
        }
        if (road.endIntersectionId > 0) {
            feature->addRelation("endIntersection", "DevSrc-Intersection", {
                {"tileId", static_cast<int64_t>(tile->tileId().value())},
                {"DevSrc-IntersectionId", static_cast<int64_t>(road.endIntersectionId)}
            });
        }

        // Generate attributes
        std::mt19937 gen(ctx.seed + road.id);
        generateAttributes(feature, config.topAttributes, gen, road.id);
        generateLayeredAttributes(feature, config.layeredAttributes, gen, road.id);

        // Generate relations
        Point roadMidpoint((road.start.x + road.end.x) / 2.0,
                          (road.start.y + road.end.y) / 2.0, 0.0);
        generateRelations(feature, ctx, config.relations, roadMidpoint);
    }
}

void GridDataSource::generateTraffic(TileSpatialContext& ctx,
                                     const LayerConfig& config,
                                     TileFeatureLayer::Ptr const& tile) {
    constexpr size_t maxTrafficFeatures = 10'000;
    const auto& traffic = *config.traffic;

    // Publish the stable topology before choosing the snapshot bucket. The topology
    // can be expensive on a cold context and must not consume most of a traffic epoch.
    generateRoadGrid(ctx, config, tile);
    if (ctx.roads.size() > maxTrafficFeatures) {
        tile->setError(fmt::format(
            "Grid traffic layer '{}' exceeds the {} feature safety cap ({} roads).",
            config.name,
            maxTrafficFeatures,
            ctx.roads.size()));
        tile->setTtl(std::chrono::milliseconds{0});
        return;
    }

    const auto period = std::chrono::seconds{traffic.updateIntervalSeconds};
    const auto periodMs = std::chrono::duration_cast<std::chrono::milliseconds>(period).count();
    const auto packedTileId = static_cast<uint32_t>(tile->tileId().value());

    auto bucketFor = [&](std::chrono::system_clock::time_point now) {
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        auto epoch = millis / periodMs;
        if (millis < 0 && millis % periodMs != 0) {
            --epoch;
        }
        return epoch;
    };
    auto calculateSamples = [&](int64_t epoch) {
        std::vector<TrafficSample> samples;
        samples.reserve(ctx.roads.size());
        for (const auto& road : ctx.roads) {
            samples.push_back(makeTrafficSample(
                traffic.seed,
                packedTileId,
                road.id,
                epoch));
        }
        return samples;
    };

    auto epoch = bucketFor(clock_());
    auto samples = calculateSamples(epoch);

    // Recheck once immediately before tile mutation. Crossing while calculating must
    // never publish a tile whose declared lifetime already ended.
    const auto finalEpoch = bucketFor(clock_());
    if (finalEpoch != epoch) {
        epoch = finalEpoch;
        samples = calculateSamples(epoch);
    }

    for (size_t index = 0; index < ctx.roads.size(); ++index) {
        const auto& road = ctx.roads[index];
        const auto& sample = samples[index];
        auto feature = tile->newFeature(
            config.featureType,
            {{config.featureType + "Id", sample.roadId}});

        auto line = feature->geom()->newGeometry(
            GeomType::Line,
            static_cast<uint32_t>(road.intermediatePoints.size() + 2U));
        line->append(road.start);
        for (const auto& point : road.intermediatePoints) {
            line->append(point);
        }
        line->append(road.end);

        feature->attributes()->addField("trafficFlow", sample.flow);
        feature->attributes()->addField(
            "estimatedAverageSpeedKph",
            static_cast<int64_t>(sample.estimatedAverageSpeedKph));
        feature->attributes()->addField(
            "freeFlowSpeedKph",
            static_cast<int64_t>(sample.freeFlowSpeedKph));
        feature->attributes()->addField(
            "relativeSpeedPercent",
            static_cast<int64_t>(sample.relativeSpeedPercent));
        feature->attributes()->addField("trafficEpoch", epoch);
        feature->addRelation("road", traffic.roadFeatureType, {
            {"tileId", static_cast<int64_t>(tile->tileId().value())},
            {traffic.roadFeatureIdPart, static_cast<int64_t>(road.id)}
        });
    }

    tile->setTimestamp(std::chrono::system_clock::time_point{
        std::chrono::milliseconds{epoch * periodMs}});
    tile->setTtl(std::chrono::duration_cast<std::chrono::milliseconds>(period));
    mapget::log().debug(
        "Generated {} traffic features for epoch {} on tile {}",
        samples.size(),
        epoch,
        tile->tileId().value());
}

void GridDataSource::generateIntersections(TileSpatialContext& ctx,
                                          const LayerConfig& config,
                                          TileFeatureLayer::Ptr const& tile) {
    // Lazily generate road grid first (which creates intersections)
    generateRoadGrid(ctx, config, tile);

    if (ctx.intersections.empty()) {
        mapget::log().warn("  No intersections to generate");
        return;
    }

    // Create intersection features
    for (const auto& intersection : ctx.intersections) {
        auto feature = tile->newFeature(config.featureType,
            {{config.featureType + "Id", intersection.id}});

        // Create point geometry (Points type with single point)
        auto points = feature->geom()->newGeometry(GeomType::Points, 1, true);
        points->append(intersection.position);

        // Add relations to connected roads
        for (uint32_t roadId : intersection.connectedRoadIds) {
            feature->addRelation("connectedRoad", "DevSrc-Road", {
                {"tileId", static_cast<int64_t>(tile->tileId().value())},
                {"DevSrc-RoadId", static_cast<int64_t>(roadId)}
            });
        }

        // Generate attributes
        std::mt19937 gen(ctx.seed + intersection.id);
        generateAttributes(feature, config.topAttributes, gen, intersection.id);
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
