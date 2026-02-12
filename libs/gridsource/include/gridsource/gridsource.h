// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <mapget/service/datasource.h>
#include <mapget/model/point.h>
#include <mapget/model/featureid.h>
#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>
#include <random>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <mutex>
#include <optional>
#include <map>

// Hash function for TileId to use in unordered_map
namespace std {
    template<>
    struct hash<mapget::TileId> {
        size_t operator()(const mapget::TileId& tid) const noexcept {
            return std::hash<uint64_t>{}(tid.value_);
        }
    };
}

/**
 * This data source is a procedural map data generator for testing and development.
 * It generates spatially-coherent, deterministic map data including buildings,
 * roads, and POIs with realistic spatial relationships.
 *
 * Key features:
 * - Tile-based deterministic generation (same tile ID = same data)
 * - Spatial coherence (roads avoid buildings, POIs align with roads)
 * - Highly configurable via YAML
 * - Flexible attribute generation system
 * - Relations between features
 */

namespace mapget::gridsource {

// Forward declarations
struct TileSpatialContext;
struct LayerConfig;
struct AttributeConfig;
struct AttributeLayerConfig;
struct RelationConfig;
struct GeometryConfig;

/**
 * Attribute tree profile for procedural attribute generation
 */
enum class AttributeTreeProfile {
    None,       // No profile attributes (default, backward compatible)
    Minimal,    // ~5 leaf nodes, 1 layer, flat scalars
    Moderate,   // ~40 leaf nodes, 3 layers, shallow nesting
    Realistic,  // ~150 leaf nodes, 6 layers, nested objects with validity
    Stress      // ~1000+ leaf nodes, 12 layers, deep nesting
};

/**
 * Parameters for attribute tree generation (overridable per-profile)
 */
struct AttributeTreeParams {
    std::optional<int> numLayers;
    std::optional<int> attrsPerLayer;
    std::optional<int> fieldsPerAttr;
    std::optional<int> maxNestingDepth;
    std::optional<int> maxArraySize;
    std::optional<double> nestingProbability;
    std::optional<double> directionalValidityProb;
    std::optional<double> rangeValidityProb;
    std::optional<int> topLevelExtraFields;

    static AttributeTreeParams fromYAML(const YAML::Node& node);
};

/**
 * Resolved (fully concrete) tree params after merging profile + overrides
 */
struct ResolvedTreeParams {
    int numLayers;
    int attrsPerLayer;
    int fieldsPerAttr;
    int maxNestingDepth;
    int maxArraySize;
    double nestingProbability;
    double directionalValidityProb;
    double rangeValidityProb;
    int topLevelExtraFields;

    static ResolvedTreeParams resolve(
        AttributeTreeProfile profile,
        const AttributeTreeParams& globalOverrides,
        const AttributeTreeParams& layerOverrides);
};

/**
 * Geometry type for generated features
 */
enum class GeometryType {
    Point,
    Line,
    Polygon,
    Mesh
};

/**
 * Attribute generator type
 */
enum class GeneratorType {
    Random,      // Random value from distribution
    Sequential,  // Sequential numbering with template
    Computed,    // Derived from other attributes/geometry
    Zoned,       // Distance-based zones
    Fixed,       // Fixed value
    Markov,      // Markov chain text generation
    Spatial      // Perlin noise-based
};

/**
 * Probability distribution type
 */
enum class DistributionType {
    Uniform,
    Normal,
    Exponential
};

/**
 * Data type for attributes
 */
enum class DataType {
    Int,
    Float,
    String,
    Bool,
    UInt16,
    UInt32,
    Int64
};

/**
 * Attribute configuration
 */
struct AttributeConfig {
    std::string name;
    DataType dataType = DataType::Int;
    GeneratorType generator = GeneratorType::Random;

    // For random generator
    double min = 0.0;
    double max = 100.0;
    std::vector<std::string> stringValues;
    std::vector<double> weights;
    DistributionType distribution = DistributionType::Uniform;
    double mean = 0.0;
    double stddev = 1.0;
    double lambda = 1.0;

    // For sequential generator
    std::string templateStr = "{id}";
    int startFrom = 1;

    // For computed generator
    std::string formula;

    // For zoned generator
    std::vector<double> zones;
    std::vector<double> zoneDistances;
    bool fuzzyBoundaries = true;
    double fuzziness = 0.05;

    // For fixed generator
    std::string fixedValue;

    static AttributeConfig fromYAML(const YAML::Node& node);
};

/**
 * Layered attribute configuration (with validity support)
 */
struct LayeredAttributeConfig {
    std::string name;
    std::string validityType = "none";  // none | geometric | directional
    double splitProbability = 0.0;
    double errorProbability = 0.0;
    std::vector<AttributeConfig> fields;

    static LayeredAttributeConfig fromYAML(const YAML::Node& node);
};

/**
 * Attribute layer configuration
 */
struct AttributeLayerConfig {
    std::string layerName;
    std::vector<LayeredAttributeConfig> attributes;

    static AttributeLayerConfig fromYAML(const YAML::Node& node);
};

/**
 * Relation configuration
 */
struct RelationConfig {
    std::string name;
    std::string targetLayer;
    std::string targetType;
    double maxDistance = 100.0;
    std::string cardinality = "one";  // one | many
    bool optional = false;
    std::string validityType = "none";  // none | point | range

    static RelationConfig fromYAML(const YAML::Node& node);
};

/**
 * Geometry configuration
 */
struct GeometryConfig {
    GeometryType type = GeometryType::Line;
    double density = 0.05;
    int complexity = 6;
    double curvature = 0.08;

    // For buildings
    std::vector<double> sizeRange = {15.0, 50.0};
    std::vector<double> aspectRatio = {1.2, 3.0};

    // For roads
    bool avoidBuildings = true;
    double minBuildingDistance = 2.0;

    static GeometryConfig fromYAML(const YAML::Node& node);
};

/**
 * Layer configuration
 */
struct LayerConfig {
    std::string name;
    bool enabled = true;
    std::string featureType;

    GeometryConfig geometry;
    std::vector<AttributeConfig> topAttributes;
    std::vector<AttributeLayerConfig> layeredAttributes;
    std::vector<RelationConfig> relations;

    // Per-layer attribute tree profile override (nullopt = use global)
    std::optional<AttributeTreeProfile> attributeTreeProfile;
    AttributeTreeParams attributeTreeParams;

    static LayerConfig fromYAML(const YAML::Node& node);
};

/**
 * GridDataSource configuration
 */
struct Config {
    std::string mapId = "GridDataSource";
    bool spatialCoherence = true;
    double collisionGridSize = 10.0;
    uint32_t sourceDownloadDelayMs = 0;  // Sleep-wait (simulates IO: downloading from server)
    uint32_t sourceUnpackDelayMs = 0;    // Busy-wait (simulates CPU: decompression/parsing)
    uint32_t sourceTransformDelayMs = 0; // Busy-wait (simulates CPU: conversion to features)

    // Attribute tree profile (global default)
    AttributeTreeProfile attributeTreeProfile = AttributeTreeProfile::None;
    AttributeTreeParams attributeTreeParams;

    std::vector<LayerConfig> layers;

    static Config fromYAML(const YAML::Node& node);
    nlohmann::json toJson() const;
    static Config fromJson(const nlohmann::json& j);
};

/**
 * Building data structure
 */
struct Building {
    double minX, minY, maxX, maxY;
    uint32_t id;
    std::string buildingType;
};

/**
 * Road segment data structure
 */
struct RoadSegment {
    mapget::Point start;
    mapget::Point end;
    std::vector<mapget::Point> intermediatePoints;
    uint32_t id;
    uint16_t speedLimit;
    uint32_t startIntersectionId;
    uint32_t endIntersectionId;
};

/**
 * Intersection data structure
 */
struct Intersection {
    mapget::Point position;
    uint32_t id;
    std::vector<uint32_t> connectedRoadIds;
};

/**
 * Rectangle structure for blocks between roads
 */
struct Block {
    double minX, minY, maxX, maxY;
};

/**
 * Spatial context for a tile
 * This structure maintains the spatial state of generated features
 * to ensure coherence across layer requests for the same tile.
 */
struct TileSpatialContext {
    mapget::TileId tileId;
    uint32_t seed;

    // Generated features
    std::vector<Building> buildings;
    std::vector<RoadSegment> roads;
    std::vector<Intersection> intersections;

    // Road grid structure (horizontal and vertical grid lines)
    std::vector<double> horizontalRoadY;  // Y coordinates of horizontal roads
    std::vector<double> verticalRoadX;    // X coordinates of vertical roads
    std::vector<Block> blocks;            // Rectangular blocks between roads
    std::once_flag gridGeneratedOnce;     // Ensure grid is only generated once (thread-safe)

    // Grid-based occupancy map for fast collision detection
    double cellSize;
    std::unordered_set<uint64_t> occupiedCells;
    std::unordered_map<uint64_t, uint32_t> cellToBuilding;

    explicit TileSpatialContext(mapget::TileId tid, double gridSize = 10.0);

    // Helper methods
    static uint64_t cellKey(int x, int y);
    bool isCellOccupied(int gridX, int gridY) const;
    void markBuildingCells(const Building& building);
    bool doesLineIntersectBuilding(mapget::Point a, mapget::Point b) const;
    mapget::Point findNearestRoadPoint(mapget::Point p, double* outDistance = nullptr) const;
    std::vector<uint32_t> findBuildingsNearPoint(mapget::Point p, double radius) const;
    uint32_t findRoadAtPoint(mapget::Point p, double tolerance = 1.0) const;

private:
    bool doesLineIntersectBox(mapget::Point a, mapget::Point b,
                             double minX, double minY, double maxX, double maxY) const;
    mapget::Point closestPointOnSegment(mapget::Point p, mapget::Point a, mapget::Point b) const;
};

/**
 * Development data source with procedural generation
 */
class GridDataSource : public mapget::DataSource
{
public:
    explicit GridDataSource(const YAML::Node& config = YAML::Node());

    mapget::DataSourceInfo info() override;
    void fill(mapget::TileFeatureLayer::Ptr const& tile) override;
    void fill(mapget::TileSourceDataLayer::Ptr const& tile) override {
        throw std::runtime_error("SourceDataLayer not supported by GridDataSource");
    }
    std::vector<mapget::LocateResponse> locate(mapget::LocateRequest const& req) override;

    // Live config mutation (for dev UI)
    void setConfig(gridsource::Config newConfig);
    gridsource::Config getConfig() const;
    void clearContextCache();

    // Static instance registry (for dev UI REST API)
    static void registerInstance(std::shared_ptr<GridDataSource> instance);
    static std::vector<std::shared_ptr<GridDataSource>> getInstances();

private:
    std::shared_ptr<const gridsource::Config> config_;
    mutable std::mutex configMutex_;
    mutable std::mutex contextMutex_;
    mutable std::unordered_map<mapget::TileId, std::shared_ptr<gridsource::TileSpatialContext>> contextCache_;
    static constexpr size_t MAX_CACHED_CONTEXTS = 1000;

    // Static registry
    static std::mutex registryMutex_;
    static std::vector<std::weak_ptr<GridDataSource>> registry_;

    // Get or create spatial context for a tile
    std::shared_ptr<gridsource::TileSpatialContext> getOrCreateContext(
        mapget::TileId tileId,
        std::shared_ptr<const gridsource::Config> const& cfg) const;

    // Layer generation methods
    void generateRoadGrid(gridsource::TileSpatialContext& ctx,
                         const gridsource::LayerConfig& config,
                         mapget::TileFeatureLayer::Ptr const& tile);

    void generateBuildings(gridsource::TileSpatialContext& ctx,
                          const gridsource::LayerConfig& layerCfg,
                          const gridsource::Config& cfg,
                          mapget::TileFeatureLayer::Ptr const& tile);

    void generateRoads(gridsource::TileSpatialContext& ctx,
                      const gridsource::LayerConfig& layerCfg,
                      const gridsource::Config& cfg,
                      mapget::TileFeatureLayer::Ptr const& tile);

    void generateIntersections(gridsource::TileSpatialContext& ctx,
                              const gridsource::LayerConfig& layerCfg,
                              const gridsource::Config& cfg,
                              mapget::TileFeatureLayer::Ptr const& tile);

    // Attribute generation
    void generateAttributes(mapget::model_ptr<mapget::Feature> feature,
                           const std::vector<gridsource::AttributeConfig>& attrs,
                           std::mt19937& gen,
                           uint32_t featureId);

    void generateLayeredAttributes(mapget::model_ptr<mapget::Feature> feature,
                                   const std::vector<gridsource::AttributeLayerConfig>& layers,
                                   std::mt19937& gen,
                                   uint32_t featureId);

    // Profile-based attribute tree generation
    void generateProfileAttributes(mapget::model_ptr<mapget::Feature> feature,
                                   mapget::TileFeatureLayer::Ptr const& tile,
                                   const gridsource::ResolvedTreeParams& params,
                                   std::mt19937& gen,
                                   uint32_t featureId);

    // Relation generation
    void generateRelations(mapget::model_ptr<mapget::Feature> feature,
                          const gridsource::TileSpatialContext& ctx,
                          const std::vector<gridsource::RelationConfig>& relations,
                          mapget::Point featurePoint);

    // Helper for attribute value generation
    std::string generateAttributeValue(const gridsource::AttributeConfig& attr,
                                       std::mt19937& gen,
                                       uint32_t featureId,
                                       const std::map<std::string, std::string>& computedValues = {});
};

}  // namespace mapget
