#pragma once

#include <span>
#include <string_view>

#include "simfil/environment.h"
#include "simfil/model/nodes.h"
#include "simfil/model/string-pool.h"
#include "simfil/simfil.h"

#include "stringpool.h"
#include "layer.h"
#include "sourceinfo.h"
#include "feature.h"
#include "attrlayer.h"
#include "relation.h"
#include "geometry.h"
#include "sourcedatareference.h"
#include "pointnode.h"

namespace mapget
{

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
    friend class Attribute;
    friend class AttributeLayer;
    friend class AttributeLayerList;
    friend class Geometry;
    friend class GeometryCollection;
    friend class PointNode;
    friend class PointBufferNode;
    friend class PolygonNode;
    friend class MeshNode;
    friend class MeshTriangleCollectionNode;
    friend class LinearRingNode;
    friend class SourceDataReferenceCollection;
    friend class SourceDataReferenceItem;
    friend class Validity;

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
     * @param strings Shared string dictionary, which allows compressed storage
     *  of object field name strings. It is auto-filled, and one instance may be used
     *  by multiple TileFeatureLayer instances.
     */
    TileFeatureLayer(
        TileId tileId,
        std::string const& nodeId,
        std::string const& mapId,
        std::shared_ptr<LayerInfo> const& layerInfo,
        std::shared_ptr<simfil::StringPool> const& strings);

    /**
     * Constructor which parses a TileFeatureLayer from a binary stream.
     * @param inputStream The binary stream to parse.
     * @param layerInfoResolveFun Function which will be called to retrieve
     *  a layerInfo object for the layer name stored for the tile.
     * @param stringPoolGetter Function which will be called to retrieve
     *  a string pool for the node name of the tile.
     */
    TileFeatureLayer(
        std::istream& inputStream,
        LayerInfoResolveFun const& layerInfoResolveFun,
        StringPoolResolveFun const& stringPoolGetter
    );

    /**
     * Get/Set common id prefix for all features in this layer.
     * Note: The prefix MUST be set before any feature is added to the tile.
     */
    void setIdPrefix(KeyValueViewPairs const& prefix);
    model_ptr<Object> getIdPrefix();

    /** Destructor for the TileFeatureLayer class. */
    ~TileFeatureLayer() override;

    /**
     * Creates a new feature and insert it into this tile layer.
     * The featureIdParts (which do not include the getIdPrefix of the layer)
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
     * Create a new named attribute, which may be inserted into an attribute layer.
     */
    model_ptr<Attribute> newAttribute(std::string_view const& name, size_t initialCapacity=8);

    /**
     * Create a new attribute layer, which may be inserted into a feature.
     */
    model_ptr<AttributeLayer> newAttributeLayer(size_t initialCapacity=8);

    /**
     * Create a new geometry collection.
     */
    model_ptr<GeometryCollection> newGeometryCollection(size_t initialCapacity=1);

    /**
     * Create a new geometry.
     */
    model_ptr<Geometry> newGeometry(GeomType geomType, size_t initialCapacity=1);

    /**
     * Create a new geometry view.
     */
    model_ptr<Geometry> newGeometryView(GeomType geomType, uint32_t offset, uint32_t size, const model_ptr<Geometry>& base);

    /**
     * Create a new list of qualified source-data references.
     */
    model_ptr<SourceDataReferenceCollection> newSourceDataReferenceCollection(std::span<QualifiedSourceDataReference> list);

    /**
     * Create a new validity.
     */
    model_ptr<Validity> newValidity();

    /**
     * Create a new validity collection.
     */
    model_ptr<MultiValidity> newValidityCollection(size_t initialCapacity = 1);

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

    /** Convert to (Geo-) JSON. */
    nlohmann::json toJson() const override;

    /** Access number of stored features */
    size_t size() const;

    /** Access feature at index i */
    model_ptr<Feature> at(size_t i) const;

    /** Access feature through its id. */
    model_ptr<Feature> find(std::string_view const& featureId) const;
    model_ptr<Feature> find(std::string_view const& type, KeyValueViewPairs const& queryIdParts) const;
    model_ptr<Feature> find(std::string_view const& type, KeyValuePairs const& queryIdParts) const;

    /** Shared pointer type */
    using Ptr = std::shared_ptr<TileFeatureLayer>;

    /**
     * Evaluate a (potentially cached) simfil query on this pool
     *
     * @param query         Simfil query
     * @param node          Model root node to query
     * @param anyMode       Auto-wrap expression in `any(...)`
     * @param autoWildcard  Auto expand constant expressions to `** = <expr>`
     */
    struct QueryResult {
        // The list of values resulting from the query evaluation.
        std::vector<simfil::Value> values;

        // A map of traces for debugging or understanding query execution,
        // where the key is a string identifier and the value is a trace object.
        std::map<std::string, simfil::Trace> traces;

        // Diagnostics information, such as warnings or errors,
        // generated during query evaluation.
        simfil::Diagnostics diagnostics;
    };
    QueryResult evaluate(std::string_view query, ModelNode const& node, bool anyMode = true, bool autoWildcard = true);
    QueryResult evaluate(std::string_view query, bool anyMode = true, bool autoWildcard = true);

    /**
     * Get auto-completion candidates at `point` of a query.
     */
    std::vector<simfil::CompletionCandidate> complete(std::string_view query, int point, ModelNode const& node, simfil::CompletionOptions const& opts);

    /**
     * Collect query diagnostics for an evaluated query.
     * If the query has not yet been evaluated, an empty list is returned.
     */
    std::vector<simfil::Diagnostics::Message> collectQueryDiagnostics(std::string_view query, const simfil::Diagnostics& diagnostics);

    /**
     * Change the string pool of this model to a different one.
     * Note: This will potentially create new string entries in the newDict,
     * for field names which were not there before.
     */
    void setStrings(std::shared_ptr<simfil::StringPool> const& newPool) override;

    /**
     * Create a copy of otherFeature in this layer with the given type
     * and id-parts. If a feature with that ID already exists in this layer,
     * the attributes/relations/geometries from otherFeature will simply
     * be appended to the existing feature.
     */
    void clone(
        std::unordered_map<uint32_t, simfil::ModelNode::Ptr>& clonedModelNodes,
        TileFeatureLayer::Ptr const& otherLayer,
        Feature const& otherFeature,
        std::string_view const& type,
        KeyValueViewPairs idParts);

    /**
     * Create a copy of otherNode (which lives in otherLayer) in this layer.
     * The clonedModelNodes dict may be provided to avoid repeated copies
     * of nodes which are referenced multiple times.
     */
    simfil::ModelNode::Ptr clone(
        std::unordered_map<uint32_t, simfil::ModelNode::Ptr>& clonedModelNodes,
        TileFeatureLayer::Ptr const& otherLayer,
        simfil::ModelNode::Ptr const& otherNode);

    /**
     * Node resolution functions.
     */
    model_ptr<AttributeLayer> resolveAttributeLayer(simfil::ModelNode const& n) const;
    model_ptr<AttributeLayerList> resolveAttributeLayerList(simfil::ModelNode const& n) const;
    model_ptr<Attribute> resolveAttribute(simfil::ModelNode const& n) const;
    model_ptr<Feature> resolveFeature(simfil::ModelNode const& n) const;
    model_ptr<FeatureId> resolveFeatureId(simfil::ModelNode const& n) const;
    model_ptr<Relation> resolveRelation(simfil::ModelNode const& n) const;
    model_ptr<PointNode> resolvePoint(const simfil::ModelNode& n) const;
    model_ptr<PointBufferNode> resolvePointBuffer(const simfil::ModelNode& n) const;
    model_ptr<Geometry> resolveGeometry(simfil::ModelNode const& n) const;
    model_ptr<GeometryCollection> resolveGeometryCollection(simfil::ModelNode const& n) const;
    model_ptr<MeshNode> resolveMesh(simfil::ModelNode const& n) const;
    model_ptr<MeshTriangleCollectionNode> resolveMeshTriangleCollection(simfil::ModelNode const& n) const;
    model_ptr<LinearRingNode> resolveMeshTriangleLinearRing(simfil::ModelNode const& n) const;
    model_ptr<PolygonNode> resolvePolygon(simfil::ModelNode const& n) const;
    model_ptr<LinearRingNode> resolveLinearRing(simfil::ModelNode const& n) const;
    model_ptr<SourceDataReferenceCollection> resolveSourceDataReferenceCollection(simfil::ModelNode const& n) const;
    model_ptr<SourceDataReferenceItem> resolveSourceDataReferenceItem(simfil::ModelNode const& n) const;
    model_ptr<PointNode> resolveValidityPoint(const simfil::ModelNode& n) const;
    model_ptr<Validity> resolveValidity(simfil::ModelNode const& n) const;
    model_ptr<MultiValidity> resolveValidityCollection(simfil::ModelNode const& n) const;

    /**
     * The ColumnId enum provides identifiers for different
     * types of columns that can be associated with feature data.
     */
    struct ColumnId { enum : uint8_t {
        Features = FirstCustomColumnId,
        FeatureProperties,
        FeatureIds,
        Attributes,
        AttributeLayers,
        AttributeLayerLists,
        Relations,
        Points,
        PointBuffers,
        Geometries,
        GeometryCollections,
        Mesh,
        MeshTriangleCollection,
        MeshTriangleLinearRing, // LinearRing with fixed size 3
        Polygon,
        LinearRing,
        SourceDataReferenceCollections,
        SourceDataReferences,
        Validities,
        ValidityPoints,
        ValidityCollections,
    }; };
    
protected:

    /** Get the primary id composition for the given feature type. */
    std::vector<IdPart> const& getPrimaryIdComposition(std::string_view const& type) const;

    /**
     * Create a new attribute layer collection.
     */
    model_ptr<AttributeLayerList> newAttributeLayers(size_t initialCapacity=8);

    /**
     * Generic node resolution overload.
     */
    void resolve(const simfil::ModelNode &n, const ResolveFn &cb) const override;

    Geometry::Storage& vertexBufferStorage();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
