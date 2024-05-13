#pragma once

#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>
#include "sfl/small_vector.hpp"
#include <variant>
#include "tileid.h"

namespace mapget
{

/**
 * The KeyValue(View)Pairs type is a vector of pairs, where each pair
 * consists of a string_view key and a variant value that can be
 * either an int64_t or a string_view. It is used as the interface-
 * type for feature id parts. By using sfl::small_vector instead of
 * std::vector, it is kept on the stack.
 */
using KeyValueViewPairs =
    sfl::small_vector<std::pair<std::string_view, std::variant<int64_t, std::string_view>>, 16>;
using KeyValuePairs =
    sfl::small_vector<std::pair<std::string, std::variant<int64_t, std::string>>, 16>;

/** Convert KeyValuePairs to KeyValuePairsView */
KeyValueViewPairs castToKeyValueView(KeyValuePairs const& kvp);

/**
 * Version Definition - This is used to recognize whether a stored blob of a
 * TileFeatureLayer should be parsed by this version of the mapget library.
 * When a TileFeatureLayer is serialized, the current Version value for the
 * map layer and the stream protocol are also stored. Upon deserialization,
 * the Major-Minor version values must match the parsed version Major-Minor
 * values, for both the map layer and the stream protocol.
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

    /** Create Version from JSON. */
    static Version fromJson(const nlohmann::json& j);

    /** Serialize Version to JSON. */
    [[nodiscard]] nlohmann::json toJson() const;
};

/** Enum to represent the possible data types */
enum class IdPartDataType {I32, U32, I64, U64, UUID128, STR};
NLOHMANN_JSON_SERIALIZE_ENUM(
    IdPartDataType,
    {
        {IdPartDataType::I32, "I32"},
        {IdPartDataType::U32, "U32"},
        {IdPartDataType::I64, "I64"},
        {IdPartDataType::U64, "U64"},
        {IdPartDataType::UUID128, "UUID128"},
        {IdPartDataType::STR, "STR"},
    })

/** Enum to represent the possible layer types */
enum class LayerType {Features, Heightmap, OrthoImage, GLTF};
NLOHMANN_JSON_SERIALIZE_ENUM(
    LayerType,
    {
        {LayerType::Features, "Features"},
        {LayerType::Heightmap, "Heightmap"},
        {LayerType::OrthoImage, "OrthoImage"},
        {LayerType::GLTF, "GLTF"},
    })

/** Structure to represent a part of a feature id composition. */
struct IdPart
{
    /** Label/identifier for this ID part.
     * Unique under all ID parts of a feature. */
    std::string idPartLabel_;

    /** Description of the identifier. */
    std::string description_;

    /** Data type of the identifier. */
    IdPartDataType datatype_;

    /** Is the identifier synthetic or part of a map specification? */
    bool isSynthetic_ = false;

    /** Is the identifier optional in feature queries? */
    bool isOptional_ = false;

    /** Create IdPart from JSON. */
    static IdPart fromJson(const nlohmann::json& j);

    /** Serialize IdPart to JSON. */
    [[nodiscard]] nlohmann::json toJson() const;

    /**
     * Check whether the given value satisfies the constraints of this
     * IdPart specification. The value will be converted to an integer if provided
     * as a string, but not vice versa. Returns true and the correct converted value
     * written into val if the validation succeeds, false otherwise.
     */
    bool validate(std::variant<int64_t, std::string_view>& val, std::string* error = nullptr) const;
    bool validate(std::variant<int64_t, std::string>& val, std::string* error = nullptr) const;

    /**
     * Check that starting from a given index, the parts of an id composition
     * match the featureIdParts segment from start for the given length.
     */
    static bool idPartsMatchComposition(
        std::vector<IdPart> const& candidateComposition,
        uint32_t compositionMatchStartIdx,
        KeyValueViewPairs const& featureIdParts,
        size_t matchLength,
        bool requireCompositionEnd);
};

/** Structure to represent the feature type info */
struct FeatureTypeInfo
{
    /** Name of the feature type */
    std::string name_;

    /**
     * List of allowed unique id compositions (each id composition is a list of id parts)
     * A single id composition must never have more than 16 parts.
     * The first unique id composition in the list is the primary, which must be
     * used by all features. Secondary compositions may only be used by references/relations.
     */
    std::vector<std::vector<IdPart>> uniqueIdCompositions_;

    /**
     * Deserializes a FeatureTypeInfo object from JSON.
     *
     * The JSON is expected to have the following structure:
     *
     * {
     *   "name": <string>,                  // Mandatory: The name of the feature type.
     *   "uniqueIdCompositions": [          // Mandatory: A list of unique ID compositions.
     *     [                                // A list of UniqueIdPart objects, each having the following structure:
     *       {
     *         "partId": <string>,          // Mandatory: The ID of the unique ID part.
     *         "description": <string>,     // Optional: The description of the unique ID part.
     *         "datatype": <IdPartDataType>,// Optional: The data type of the unique ID part, must be one of the enum values from IdPartDataType. Defaults to I64.
     *         TODO why "datatype"  and not "dataType" to match other fields?
     *         "isSynthetic": <bool>,       // Optional: A flag indicating if the unique ID part is synthetic. Defaults to false.
     *         "isOptional": <bool>         // Optional: A flag indicating if the unique ID part is optional. Defaults to false.
     *       },
     *       ...
     *     ],
     *     ...
     *   ]
     * }
     *
     * @param j The JSON to deserialize.
     * @return The deserialized FeatureTypeInfo object.
     * @throws std::runtime_error if any mandatory field is missing.
     */
    static FeatureTypeInfo fromJson(const nlohmann::json& j);

    /** Serialize FeatureTypeInfo to JSON. */
    [[nodiscard]] nlohmann::json toJson() const;
};

/**
 * Structure to represent a list of coverage flags
 * as a rectangle between a minimum and a maximum tile id.
 */
struct Coverage
{
    /** Minimum tile id (north-west AABB corner). */
    TileId min_;

    /** Maximum tile id (south-east AABB corner). Must have same zoom level as min. */
    TileId max_;

    /**
     * Bitset indicating where the associated layer is filled.
     * Must have size (max.x() - min.x() + 1) * (max.y() - min.y() + 1).
     * Bits are stored row-major: [ y0x0..., y0xn, y1x0..., y1xn, ... ]
     * If the vector is completely empty, the rectangle is considered
     * to be fully filled.
     */
    std::vector<bool> filled_;

    /**
     * Deserializes a Coverage object from JSON.
     *
     * The JSON is expected to have the following structure:
     *
     * {
     *   "min": <uint64_t>,                // Mandatory: The minimum tile ID.
     *   "max": <uint64_t>,                // Mandatory: The maximum tile ID.
     *   "filled": [<bool>...]             // Optional: A list of boolean values indicating filled tiles. Defaults to an empty list.
     * }
     *
     * Alternatively, an single integer is interpreted as a Coverage
     * where min=max, and `filled` is empty.
     *
     * @param j The JSON to deserialize.
     * @return The deserialized Coverage object.
     * @throws std::runtime_error if any mandatory field is missing.
     */
    static Coverage fromJson(const nlohmann::json& j);

    /** Serialize Coverage to JSON. */
    [[nodiscard]] nlohmann::json toJson() const;
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

    /** Utility function to get some feature type info by name. */
    FeatureTypeInfo const* getTypeInfo(std::string_view const& sv, bool throwIfMissing=true);

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

    /**
     * Validate that a unique id composition exists that matches this feature id.
     * The field values must match the limitations of the IdPartDataType, and
     * The order of values in KeyValuePairs must be the same as in the composition!
     * @param typeId Feature type id, throws error if the type was not registered.
     * @param featureIdParts Uniquely identifying information for the feature.
     * @param validateForNewFeature True if the id should be evaluated with this tile's prefix prepended.
     */
    bool validFeatureId(
        const std::string_view& typeId,
        KeyValueViewPairs const& featureIdParts,
        bool validateForNewFeature,
        uint32_t compositionMatchStartIndex = 0);

    /** Create LayerInfo from JSON. */
    static std::shared_ptr<LayerInfo> fromJson(const nlohmann::json& j, std::string const& layerId="");

    /** Serialize LayerInfo to JSON. */
    [[nodiscard]] nlohmann::json toJson() const;
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

    /** Declare the datasource as an add-on to other datasources for the same map. */
    bool isAddOn_ = false;

    /** Extra JSON attachment. May also be used to store style-sheets. */
    nlohmann::json extraJsonAttachment_;

    /** Used mapget protocol version */
    Version protocolVersion_;

    /** Get the layer, or a runtime error, if no such layer exists. */
    [[nodiscard]] std::shared_ptr<LayerInfo> getLayer(std::string const& layerId, bool throwIfMissing=true) const;

    /**
     * Deserializes a DataSourceInfo object from JSON.
     *
     * The JSON is expected to have the following structure:
     *
     * {
     *   "mapId": <string>,                   // Mandatory: A unique identifier for the map.
     *   "layers": {                          // Mandatory: A dictionary mapping layer names to LayerInfo objects.
     *     <layerId>: {
     *       "type": <LayerType>,             // Optional: The type of the layer. Defaults to "Features".
     *       "featureTypes": [<FeatureTypeInfo>...], // Mandatory: A list of feature type information.
     *       "zoomLevels": [<int>...],        // Optional: A list of zoom levels. Defaults to empty list.
     *       "coverage": [<Coverage>...],     // Optional: A list of coverage objects. Defaults to empty list.
     *       "canRead": <bool>,               // Optional: Whether the layer can be read. Defaults to true.
     *       "canWrite": <bool>,              // Optional: Whether the layer can be written. Defaults to false.
     *       "version": {                     // Optional: The version of the layer.
     *         "major": <int>,                // Mandatory: The major version number.
     *         "minor": <int>,                // Mandatory: The minor version number.
     *         "patch": <int>                 // Mandatory: The patch version number.
     *       }
     *     },
     *     ...
     *   },
     *   "maxParallelJobs": <int>,            // Optional: The maximum number of parallel jobs allowed. Defaults to 8.
     *   "extraJsonAttachment": <JSON object>,// Optional: Any extra JSON data attached. Defaults to an empty object.
     *   "protocolVersion": {                 // Optional: The version of the protocol. Defaults to the current protocol version.
     *     "major": <int>,                    // Mandatory: The major version number.
     *     "minor": <int>,                    // Mandatory: The minor version number.
     *     "patch": <int>                     // Mandatory: The patch version number.
     *   },
     *   "nodeId": <string>,                  // Optional: A UUID for the node. If not provided, a random UUID will be generated.
     *                                        // Note: Only provide this if you have a good reason.
     *   "addOn": <bool>                     // Optional: Declare the datasource as add-on.
     * }
     *
     * Each LayerType, FeatureTypeInfo, and Coverage object has its own specific JSON structure,
     * as defined by the corresponding class's fromJson() method.
     *
     * @param j The JSON to deserialize.
     * @return The deserialized DataSourceInfo object.
     * @throws std::runtime_error if any mandatory field is missing.
     */
    static DataSourceInfo fromJson(const nlohmann::json& j);

    /** Serialize DataSourceInfo to JSON. */
    [[nodiscard]] nlohmann::json toJson() const;
};

}  // End of namespace mapget
