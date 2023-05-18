#pragma once

#include "simfil/model/model.h"

#include "fields.h"
#include "layer.h"
#include "feature.h"

namespace mapget
{

/// Forward declaration for the Feature class
class Feature;

/// The KeyValuePairs type is a vector of pairs, where each pair
/// consists of a string_view key and a variant value that can be
/// either an int64_t or a string_view.
using KeyValuePairs = std::vector<std::pair<
    std::string_view,
    std::variant<int64_t, std::string_view>>>;

/**
 * The TileFeatureLayer class represents a specific map layer
 * within a map tile. It is a container for map features.
 */
class TileFeatureLayer : public TileLayer, public simfil::ModelPool
{
public:
    /**
     * This constructor initializes a new TileFeatureLayer instance.
     * Each instance is associated with a specific TileId, nodeId, and mapId.
     * The layerInfo shared_ptr parameter provides additional information about
     * the layer, while the featureIdPrefix_ and fields parameters are used to
     * construct features within this layer.
     */
    TileFeatureLayer(
        TileId tileId,
        std::string const& nodeId,
        std::string const& mapId,
        std::shared_ptr<LayerInfo> const& layerInfo,
        KeyValuePairs const& featureIdPrefix_,
        std::shared_ptr<Fields> fields
    );

    /** Destructor for the TileFeatureLayer class. */
    ~TileFeatureLayer();

    /**
     * The newFeature function creates a new feature within the layer.
     * The typeId string_view parameter specifies the type of the feature,
     * while the featureId parameter provides identifying information for the feature.
     * The unique identifying information, prepended with the featureIdPrefix,
     * must conform to an existing UniqueIdComposition for the feature typeId
     * within the associated layer.
     */
    simfil::shared_model_ptr<Feature> newFeature(
        std::string_view const& typeId,
        KeyValuePairs const& featureId);

    /**
     * The featureIdPrefix function returns common ID parts,
     * which are shared by all features in this layer.
     */
    model_ptr<Object> featureIdPrefix();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    /**
     * The FeatureTileColumnId enum provides identifiers for different
     * types of columns that can be associated with feature data.
     */
    enum FeatureTileColumnId : uint8_t {
        Features = FirstCustomColumnId,
        FeatureProperties,
        FeatureIds,
        AttributeLayers,
        Attributes,
        Icons
    };
};
}
