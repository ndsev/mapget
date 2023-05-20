#pragma once

#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

#include "tileid.h"

namespace mapget
{

/// Enum to represent the possible data types
enum class DataType {I32, U32, I64, U64, UUID128};

/// Enum to represent the possible layer types
enum class LayerType {Features, Heightmap, OrthoImage, GLTF};

/// Structure to represent the unique id composition
struct UniqueIdPart
{
    std::string partId;            // Unique identifier part
    std::string description;       // Description of the unique identifier
    DataType datatype;             // Datatype of the unique identifier
    bool isSynthetic = false;      // Is the identifier synthetic or part of a map specification?
    bool isOptional = false;       // Is the identifier actually optional?
};

/// Structure to represent the feature type info
struct FeatureTypeInfo {
    std::string name;              // Name of the feature type

    // List of allowed unique id compositions (each id composition is a list of id parts)
    // A single id composition must never have more than 16 parts.
    std::vector<std::vector<UniqueIdPart>> uniqueIdCompositions;
};

/// Structure to represent the coverage
struct Coverage {
    // Minimum tile id
    TileId min;
    // Maximum tile id. Must have some zoom level as min.
    TileId max;
    // Bitset indicating where the associated layer is filled.
    // Must have size (max.x() - min.x() + 1) * (max.y() - min.y() + 1).
    // Bits are stored row-major: [ y0x0..., y0xn, y1x0..., y1xn, ... ]
    std::vector<bool> filled;
};

/// Structure to represent the layer info
struct LayerInfo
{
    // Unique identifier of the layer
    std::string layerId;
    // Type of the layer
    LayerType type = LayerType::Features;
    // List of feature types
    std::vector<FeatureTypeInfo> featureTypes;
    // List of zoom levels
    std::vector<int> zoomLevels;
    // List of coverages
    std::vector<Coverage> coverage;
    // Can this layer be read from?
    bool canRead = true;
    // Can this layer be written to?
    bool canWrite = false;
};

/// Structure to represent the data source info
struct DataSourceInfo
{
    // Unique identifier of the node
    std::string nodeId;
    // Unique identifier of the map
    std::string mapId;
    // List of layers
    std::map<std::string, std::shared_ptr<LayerInfo>> layers;
    // Maximum number of parallel jobs
    int maxParallelJobs = 8;
    // Version of the protocol
    int protocolVersion = 0;
    // Extra JSON attachment. May also be used to store style-sheets.
    nlohmann::json extraJsonAttachment;
};

}