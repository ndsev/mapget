#pragma once

#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

#include "tileid.h"

namespace mapget
{

/**
 * Version Definition - This is used to recognize whether
 * a stored blob of a TileFeatureLayer should be parsed by this
 * version of the mapget library. When a TileFeatureLayer is serialized,
 * the current Version value for the map layer and and the stream protocol are
 * also stored. Upon deserialization, the Major-Minor version values
 * must match the parsed version Major-Minor value, for both the for the map layer
 * and and the stream protocol.
 */
struct Version {
    uint16_t major_ = 0;
    uint16_t minor_ = 0;
    uint16_t patch_ = 0;

    [[nodiscard]] bool isCompatible(Version const& other) const;
    [[nodiscard]] std::string toString() const;

    bool operator==(Version const& other) const;
    bool operator<(Version const& other) const;

    template<typename S>
    void serialize(S& s) {
        s.value2b(major_);
        s.value2b(minor_);
        s.value2b(patch_);
    }
};

/** Enum to represent the possible data types */
enum class IdPartDataType {I32, U32, I64, U64, UUID128, STR};

/** Enum to represent the possible layer types */
enum class LayerType {Features, Heightmap, OrthoImage, GLTF};

/** Structure to represent a part of a unique feature id composition. */
struct UniqueIdPart
{
    /** Unique identifier part */
    std::string partId_;

    /** Description of the unique identifier */
    std::string description_;

    /** Datatype of the unique identifier */
    IdPartDataType datatype_;

    /** Is the identifier synthetic or part of a map specification? */
    bool isSynthetic_ = false;

    /** Is the identifier actually optional? */
    bool isOptional_ = false;
};

/** Structure to represent the feature type info */
struct FeatureTypeInfo
{
    /** Name of the feature type */
    std::string name_;

    /**
     * List of allowed unique id compositions (each id composition is a list of id parts)
     * A single id composition must never have more than 16 parts.
     */
    std::vector<std::vector<UniqueIdPart>> uniqueIdCompositions_;
};

/**
 * Structure to represent a list of coverage flags
 * as a rectangle between a minimum and a maximum tile id.
 */
struct Coverage
{
    /** Minimum tile id */
    TileId min_;

    /** Maximum tile id. Must have same zoom level as min. */
    TileId max_;

    /**
     * Bitset indicating where the associated layer is filled.
     * Must have size (max.x() - min.x() + 1) * (max.y() - min.y() + 1).
     * Bits are stored row-major: [ y0x0..., y0xn, y1x0..., y1xn, ... ]
     * If the vector is completely empty, the rectangle is considered
     * to be fully filled.
     */
    std::vector<bool> filled_;
};

/**
 * Structure to represent the layer info
 */
struct LayerInfo
{
    /** Unique identifier of the layer. */
    std::string layerId_;

    /** Type of the layer */
    LayerType type_ = LayerType::Features;

    /** List of feature types, only relevant if this is a Feature-layer. */
    std::vector<FeatureTypeInfo> featureTypes_;

    /** List of zoom levels */
    std::vector<int> zoomLevels_;

    /**
     * List of Coverage structures.
     * Multiple coverages may exist for the same zoom level.
     */
    std::vector<Coverage> coverage_;

    /** Can this layer be read from? */
    bool canRead_ = true;

    /** Can this layer be written to? */
    bool canWrite_ = false;

    /** Version of the map layer. */
    Version version_;
};

/**
 * Structure to represent the data source info
 */
struct DataSourceInfo
{
    /** Unique identifier of the node */
    std::string nodeId_;

    /** Unique identifier of the map */
    std::string mapId_;

    /** List of layers */
    std::map<std::string, std::shared_ptr<LayerInfo>> layers_;

    /** Maximum number of parallel jobs */
    int maxParallelJobs_ = 8;

    /** Extra JSON attachment. May also be used to store style-sheets. */
    nlohmann::json extraJsonAttachment_;

    /** Used mapget protocol version */
    Version protocolVersion_;
};

}  // End of namespace mapget
