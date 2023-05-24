#pragma once

#include "simfil/simfil.h"
#include "simfil/model/model.h"
#include "simfil/model/arena.h"
#include "simfil/environment.h"

#include "fields.h"
#include "layer.h"
#include "feature.h"
#include "attrlayer.h"

namespace mapget
{

/**
 * Forward declaration for the Feature class
 */
class Feature;

/**
 * The KeyValuePairs type is a vector of pairs, where each pair
 * consists of a string_view key and a variant value that can be
 * either an int64_t or a string_view.
 */
using KeyValuePairs = std::vector<std::pair<
    std::string_view,
    std::variant<int64_t, std::string_view>>>;

/**
 * The TileFeatureLayer class represents a specific map layer
 * within a map tile. It is a container for map features.
 */
class TileFeatureLayer : public TileLayer, public simfil::ModelPool
{
    friend class Feature;
    friend class FeatureId;

public:
    /**
     * This constructor initializes a new TileFeatureLayer instance.
     * Each instance is associated with a specific TileId, nodeId, and mapId.
     * @param tileId The tile id of the new feature layer. Features in this layer
     *  should be roughly within the area indicated by the tile.
     * @param nodeId Unique id of the data source node which produced this feature.
     * @param mapId ID of the map which the layer belongs to.
     * @param layerInfo Information about the map layer this feature is associated with.
     *  Each feature in this layer must have a feature type which is also present in
     *  the layer. Therefore, feature ids from this layer can be verified to conform
     *  to one of the allowed feature id compositions for the allowed type.
     * @param fields Shared field name dictionary, which allows compressed storage
     *  of object field name strings. It is auto-filled, and one instance may be used
     *  by multiple TileFeatureLayer instances.
     */
    TileFeatureLayer(
        TileId tileId,
        std::string const& nodeId,
        std::string const& mapId,
        std::shared_ptr<LayerInfo> const& layerInfo,
        std::shared_ptr<Fields> const& fields
    );

    /**
     * Set common id prefix for all features in this layer.
     */
    void setPrefix(KeyValuePairs const& prefix);

    /** Destructor for the TileFeatureLayer class. */
    ~TileFeatureLayer();

    /**
     * Creates a new feature and insert it into this tile layer.
     * The unique identifying information, prepended with the featureIdPrefix,
     * must conform to an existing UniqueIdComposition for the feature typeId
     * within the associated layer.
     * @param typeId Specifies the type of the feature.
     * @param featureIdParts Uniquely identifying information for the feature,
     * according to the requirements of typeId.
     */
    model_ptr<Feature> newFeature(
        std::string_view const& typeId,
        KeyValuePairs const& featureIdParts);

    /**
     * Create a new feature id. Use this function to create a reference to another
     * feature. The created feature id will not use the common feature id prefix from
     * this tile feature layer.
     */
    model_ptr<FeatureId> newFeatureId(
        std::string_view const& typeId,
        KeyValuePairs const& featureIdParts);

    /**
     * Create a new named attribute, which may be inserted into an attribute layer.
     */
    model_ptr<Attribute> newAttribute(std::string_view const& name, size_t initialCapacity=8);

    /**
     * Create a new attribute layer, which may be inserted into a feature.
     */
    model_ptr<AttributeLayer> newAttributeLayer(size_t initialCapacity=8);

protected:
    /**
     * The FeatureTileColumnId enum provides identifiers for different
     * types of columns that can be associated with feature data.
     */
    enum FeatureTileColumnId : uint8_t {
        Features = FirstCustomColumnId,
        FeatureProperties,
        FeatureIds,
        Attributes,
        AttributeLayers,
        AttributeLayerLists,
        Icons
    };

    /**
     * The featureIdPrefix function returns common ID parts,
     * which are shared by all features in this layer.
     */
    std::optional<model_ptr<Object>> featureIdPrefix();

    /**
     * Create a new attribute layer collection.
     */
    model_ptr<AttributeLayerList> newAttributeLayers(size_t initialCapacity=8);

    /**
     * Node resolution functions.
     */
    model_ptr<AttributeLayer> resolveAttributeLayer(simfil::ModelNode const& n) const;
    model_ptr<AttributeLayerList> resolveAttributeLayerList(simfil::ModelNode const& n) const;
    model_ptr<Attribute> resolveAttribute(simfil::ModelNode const& n) const;
    model_ptr<Feature> resolveFeature(simfil::ModelNode const& n) const;
    model_ptr<FeatureId> resolveFeatureId(simfil::ModelNode const& n) const;

    /**
     * Generic node resolution overload
     */
    void resolve(const simfil::ModelNode &n, const ResolveFn &cb) const override;

    /**
     * Get this pool's simfil evaluation environment.
     */
    simfil::Environment& evaluationEnvironment();

    /**
     * Get a potentially cached compiled simfil expression for a simfil string.
     */
    simfil::ExprPtr const& compiledExpression(std::string_view const& expr);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
