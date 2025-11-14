// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <mapget/service/datasource.h>
#include <mapget/model/point.h>
#include <mapget/model/featureid.h>
#include <yaml-cpp/yaml.h>
#include <random>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <mutex>
#include <optional>

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

namespace mapget {

// Forward declarations
struct TileSpatialContext;
struct LayerConfig;
struct AttributeConfig;
struct AttributeLayerConfig;
struct RelationConfig;
struct GeometryConfig;

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

    static LayerConfig fromYAML(const YAML::Node& node);
};

/**
 * GridDataSource configuration
 */
struct Config {
    std::string mapId = "GridDataSource";
    bool spatialCoherence = true;
    double collisionGridSize = 10.0;
    std::vector<LayerConfig> layers;

    static Config fromYAML(const YAML::Node& node);
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

}  // namespace devds

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

private:
    devds::Config config_;
    mutable std::mutex contextMutex_;
    mutable std::unordered_map<mapget::TileId, std::shared_ptr<devds::TileSpatialContext>> contextCache_;
    static constexpr size_t MAX_CACHED_CONTEXTS = 1000;

    // Get or create spatial context for a tile
    std::shared_ptr<devds::TileSpatialContext> getOrCreateContext(mapget::TileId tileId) const;

    // Layer generation methods
    void generateRoadGrid(devds::TileSpatialContext& ctx,
                         const devds::LayerConfig& config,
                         mapget::TileFeatureLayer::Ptr const& tile);

    void generateBuildings(devds::TileSpatialContext& ctx,
                          const devds::LayerConfig& config,
                          mapget::TileFeatureLayer::Ptr const& tile);

    void generateRoads(devds::TileSpatialContext& ctx,
                      const devds::LayerConfig& config,
                      mapget::TileFeatureLayer::Ptr const& tile);

    void generateIntersections(devds::TileSpatialContext& ctx,
                              const devds::LayerConfig& config,
                              mapget::TileFeatureLayer::Ptr const& tile);

    // Attribute generation
    void generateAttributes(mapget::model_ptr<mapget::Feature> feature,
                           const std::vector<devds::AttributeConfig>& attrs,
                           std::mt19937& gen,
                           uint32_t featureId);

    void generateLayeredAttributes(mapget::model_ptr<mapget::Feature> feature,
                                   const std::vector<devds::AttributeLayerConfig>& layers,
                                   std::mt19937& gen,
                                   uint32_t featureId);

    // Relation generation
    void generateRelations(mapget::model_ptr<mapget::Feature> feature,
                          const devds::TileSpatialContext& ctx,
                          const std::vector<devds::RelationConfig>& relations,
                          mapget::Point featurePoint);

    // Helper for attribute value generation
    std::string generateAttributeValue(const devds::AttributeConfig& attr,
                                       std::mt19937& gen,
                                       uint32_t featureId,
                                       const std::map<std::string, std::string>& computedValues = {});
};
