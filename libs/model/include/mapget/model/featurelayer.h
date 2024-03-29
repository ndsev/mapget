#pragma once

#include "simfil/simfil.h"
#include "simfil/model/model.h"
#include "simfil/model/arena.h"
#include "simfil/environment.h"

#include "fields.h"
#include "layer.h"
#include "feature.h"
#include "attrlayer.h"
#include "relation.h"

namespace mapget
{

/**
 * Callback type for a function which returns a field name cache instance
 * for a given node identifier.
 */
using FieldNameResolveFun = std::function<std::shared_ptr<Fields>(std::string_view const&)>;

/**
 * The TileFeatureLayer class represents a specific map layer
 * within a map tile. It is a container for map features.
 * You can iterate over all contained features using `for (auto&& feature : tileFeatureLayer)`.
 */
class TileFeatureLayer : public TileLayer, public simfil::ModelPool
{
    friend class Feature;
    friend class FeatureId;
    friend class Relation;

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
     *  to one of the allowed feature id compositions for the feature type.
     * @param fields Shared field name dictionary, which allows compressed storage
     *  of object field name strings. It is auto-filled, and one instance may be used
     *  by multiple TileFeatureLayer instances.
     */
    TileFeatureLayer(
        TileId tileId,
        std::string const& nodeId,
        std::string const& mapId,
        std::shared_ptr<LayerInfo> const& layerInfo,
        std::shared_ptr<simfil::Fields> const& fields
    );

    /**
     * Constructor which parses a TileFeatureLayer from a binary stream.
     * @param inputStream The binary stream to parse.
     * @param layerInfoResolveFun Function which will be called to retrieve
     *  a layerInfo object for the layer name stored for the tile.
     * @param fieldNameResolveFun Function which will be called to retrieve
     *  a Fields dictionary object for the node name for the tile.
     */
    TileFeatureLayer(
        std::istream& inputStream,
        LayerInfoResolveFun const& layerInfoResolveFun,
        FieldNameResolveFun const& fieldNameResolveFun
    );

    /**
     * Set common id prefix for all features in this layer.
     * Note: The prefix MUST be set before any feature is added to the tile.
     */
    void setPrefix(KeyValueViewPairs const& prefix);

    /** Destructor for the TileFeatureLayer class. */
    ~TileFeatureLayer() override;

    /**
     * Creates a new feature and insert it into this tile layer.
     * The featureIdParts (which do not include the featureIdPrefix of the layer)
     * must conform to an existing UniqueIdComposition for the feature typeId
     * within the associated layer, or a runtime error will be raised.
     * @param typeId Specifies the type of the feature.
     * @param featureIdParts Uniquely identifying information for the feature,
     * according to the requirements of typeId. Do not include the tile feature
     * prefix. If empty, an error will be thrown.
     */
    model_ptr<Feature> newFeature(
        std::string_view const& typeId, KeyValueViewPairs const& featureIdParts);

    /**
     * Create a new feature id. Use this function to create a reference to another
     * feature. The created feature id will not use the common feature id prefix from
     * this tile feature layer, since the reference may be to a feature stored in a
     * different tile.
     */
    model_ptr<FeatureId> newFeatureId(
        std::string_view const& typeId, KeyValueViewPairs const& featureIdParts);

    /**
     * Create a new relation. Use this function to create a named reference to another
     * feature, which may also have optional source/target validity geometry.
     * Relations must be stored in the feature's special relations-list.
     */
    model_ptr<Relation> newRelation(
        std::string_view const& name,
        model_ptr<FeatureId> const& target);

    /**
     * Validate that a unique id composition exists that matches this feature id,
     * The field values must match the limitations of the IdPartDataType, and
     * The order of values in KeyValuePairs must be the same as in the composition!
     * @param typeId Feature type id, throws error if the type was not registered.
     * @param featureIdParts Uniquely identifying information for the feature.
     * @param validateForNewFeature True if the id should be evaluated with this tile's prefix prepended.
     */
    bool validFeatureId(
        const std::string_view& typeId,
        KeyValueViewPairs const& featureIdParts,
        bool validateForNewFeature);

    /**
     * Create a new named attribute, which may be inserted into an attribute layer.
     */
    model_ptr<Attribute> newAttribute(std::string_view const& name, size_t initialCapacity=8);

    /**
     * Create a new attribute layer, which may be inserted into a feature.
     */
    model_ptr<AttributeLayer> newAttributeLayer(size_t initialCapacity=8);

    /**
     * Return type for begin() and end() methods to support range-based
     * for-loops to iterate over all features in a TileFeatureLayer.
     */
    struct Iterator
    {
        Iterator(TileFeatureLayer const& layer, size_t i) : layer_(layer), i_(i) {}
        model_ptr<Feature> operator*() { return layer_.at(i_); }
        Iterator& operator++()
        {
            ++i_;
            return *this;
        }
        bool operator==(const Iterator& other) const
        {
            return &layer_ == &other.layer_ && i_ == other.i_;
        }
        bool operator!=(const Iterator& other) const { return !(*this == other); }
        using iterator_category = std::input_iterator_tag;
        using value_type = model_ptr<Feature>;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;
    private:
        TileFeatureLayer const& layer_;
        size_t i_ = 0;
    };

    /**
     * begin()/end() support range-based for-loops to iterate over all
     * features in a TileFeatureLayer.
     */
    Iterator begin() const;
    Iterator end() const;

    /** (De-)Serialization */
    void write(std::ostream& outputStream) override;

    /** Convert to GeoJSON geometry collection. */
    nlohmann::json toGeoJson() const;

    /** Access number of stored features */
    size_t size() const;

    /** Access feature at index i */
    model_ptr<Feature> at(size_t i) const;

    /** Access feature through its id. */
    model_ptr<Feature> find(std::string_view const& type, KeyValueViewPairs const& queryIdParts) const;
    model_ptr<Feature> find(std::string_view const& type, KeyValuePairs const& queryIdParts) const;

    /** Shared pointer type */
    using Ptr = std::shared_ptr<TileFeatureLayer>;

    /**
     * Get this pool's simfil evaluation environment.
     */
    simfil::Environment& evaluationEnvironment();

    /**
     * Get a potentially cached compiled simfil expression for a simfil string.
     */
    simfil::ExprPtr const& compiledExpression(std::string_view const& expr);

    /**
     * Change the fields dict of this model to a different one.
     * Note: This will potentially create new field entries in the newDict,
     * for field names which were not there before.
     */
    void setFieldNames(std::shared_ptr<simfil::Fields> const& newDict) override;

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
        Relations,
    };

    /**
     * The featureIdPrefix function returns common ID parts,
     * which are shared by all features in this layer.
     */
    model_ptr<Object> featureIdPrefix();

    /** Get the primary id composition for the given feature type. */
    std::vector<IdPart> const& getPrimaryIdComposition(std::string_view const& type) const;

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
    model_ptr<Relation> resolveRelation(simfil::ModelNode const& n) const;

    /**
     * Generic node resolution overload.
     */
    void resolve(const simfil::ModelNode &n, const ResolveFn &cb) const override;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
