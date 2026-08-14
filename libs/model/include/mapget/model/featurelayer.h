#pragma once

#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "tl/expected.hpp"

#include "simfil/environment.h"
#include "simfil/model/model.h"
#include "simfil/model/nodes.h"
#include "simfil/model/string-pool.h"
#include "simfil/simfil.h"

#include "attrlayer.h"
#include "attrpoint.h"
#include "feature.h"
#include "featuremodellayer.h"
#include "geojson-import.h"
#include "geometry.h"
#include "layer.h"
#include "layerschema.h"
#include "nlohmann/json.hpp"
#include "pointnode.h"
#include "relation.h"
#include "sourcedatareference.h"
#include "sourceinfo.h"
#include "stringpool.h"

namespace mapget
{

struct FeatureLayerSelector;

/**
 * The TileFeatureLayer class represents a specific map layer
 * within a map tile. It is a container for map features.
 * You can iterate over all contained features using `for (auto&& feature : tileFeatureLayer)`.
 */
class TileFeatureLayer : public TileFeatureModelLayerBase
{
    template <class, class, class>
    friend class MergedArrayView;
    friend class Feature;
    friend class Relation;
    friend class RelationReference;
    friend class AttrPoint;
    friend class AttrPointArray;
    friend class AttrPointSequence;
    friend class AttrPointSequenceReference;
    friend class Attribute;
    friend class AttributeLayer;
    friend class AttributeLayerList;
    friend class Validity;
    template <typename Target>
    friend model_ptr<Target>
    resolveInternal(simfil::res::tag<Target>, TileFeatureLayer const&, simfil::ModelNode const&);

public:
    // Keep ModelPool::resolve<T> overloads visible alongside the override below.
    using TileFeatureModelLayerBase::resolve;
    using Ptr = std::shared_ptr<TileFeatureLayer>;
    static constexpr std::string_view GLB_ATTACHMENT_MIME_TYPE = "model/gltf-binary";

    struct CloneCacheKey
    {
        TileFeatureLayer const* model_ = nullptr;
        uint32_t address_ = 0;

        [[nodiscard]] bool operator==(CloneCacheKey const& other) const = default;
    };

    struct CloneCacheKeyHash
    {
        [[nodiscard]] size_t operator()(CloneCacheKey const& key) const noexcept
        {
            auto const modelHash = std::hash<TileFeatureLayer const*>{}(key.model_);
            auto const addressHash = std::hash<uint32_t>{}(key.address_);
            return modelHash ^ (addressHash + 0x9e3779b9U + (modelHash << 6U) + (modelHash >> 2U));
        }
    };

    using CloneCache = std::unordered_map<CloneCacheKey, simfil::ModelNode::Ptr, CloneCacheKeyHash>;

    /**
     * This constructor initializes a new TileFeatureLayer instance.
     * Each instance is associated with a specific TileId, stringPoolId, and mapId.
     * @param tileId The tile id of the new feature layer. Features in this layer
     *  should be roughly within the area indicated by the tile.
     * @param stringPoolId Unique id of the data source node which produced this feature.
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
        std::string const& stringPoolId,
        std::string const& mapId,
        std::shared_ptr<LayerInfo> const& layerInfo,
        std::shared_ptr<simfil::StringPool> const& strings);

    /**
     * Constructor which parses a TileFeatureLayer from a binary byte buffer.
     * @param input The binary bytes to parse.
     * @param layerInfoResolveFun Function which will be called to retrieve
     *  a layerInfo object for the layer name stored for the tile.
     * @param stringPoolGetter Function which will be called to retrieve
     *  a string pool for the node name of the tile.
     */
    TileFeatureLayer(
        const std::vector<uint8_t>& input,
        LayerInfoResolveFun const& layerInfoResolveFun,
        StringPoolResolveFun const& stringPoolGetter);

    /**
     * Get/Set common id prefix for all features in this layer.
     * Note: The prefix MUST be set before any feature is added to the tile.
     */
    void setIdPrefix(KeyValueViewPairs const& prefix);
    model_ptr<Object> getIdPrefix();
    model_ptr<Object> getIdPrefix() const override;

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
    model_ptr<Feature>
    newFeature(std::string_view const& typeId, KeyValueViewPairs const& featureIdParts);

    /**
     * Create a new feature id. Use this function to create a reference to another
     * feature. The created feature id will not use the common feature id prefix from
     * this tile feature layer, since the reference may be to a feature stored in a
     * different tile or map. When `externalMapId` is set to another map, the detached
     * reference keeps that map id for binary serialization and JSON export.
     */
    model_ptr<FeatureId> newFeatureId(
        std::string_view const& typeId,
        KeyValueViewPairs const& featureIdParts,
        std::optional<std::string_view> externalMapId = std::nullopt) override;

    /**
     * Create a new relation. Use this function to create a named reference to another
     * feature, which may also have optional source/target validity geometry.
     * Relations must be stored in the feature's special relations-list.
     */
    model_ptr<Relation>
    newRelation(std::string_view const& name, model_ptr<FeatureId> const& target);

    /**
     * Create a lightweight reference to a canonical relation. The reference can
     * be stored in attributes while the actual relation remains owned by the
     * feature's top-level relation list.
     */
    model_ptr<RelationReference> newRelationReference(model_ptr<Relation> const& relation);

    /**
     * Create a shared sequence over one canonical feature geometry.
     * The geometry must already be attached to the supplied feature.
     */
    model_ptr<AttrPointSequence> newAttrPointSequence(
        model_ptr<Feature> const& feature,
        model_ptr<Geometry> const& geometry);

    /** Return the number of shared attribute-point sequences in this tile. */
    [[nodiscard]] uint32_t numAttrPointSequences() const;

    /** Return one shared attribute-point sequence by its stable tile-local index. */
    [[nodiscard]] model_ptr<AttrPointSequence> attrPointSequenceAt(uint32_t index) const;

    /**
     * Create a new named attribute, which may be inserted into an attribute layer.
     */
    model_ptr<Attribute>
    newAttribute(std::string_view const& name, size_t initialCapacity = 8, bool fixedSize = false);

    /**
     * Create a new attribute layer, which may be inserted into a feature.
     */
    model_ptr<AttributeLayer> newAttributeLayer(size_t initialCapacity = 8, bool fixedSize = false);

    /**
     * Create a new geometry collection.
     */
    model_ptr<GeometryCollection>
    newGeometryCollection(size_t initialCapacity = 2, bool fixedSize = false) override;

    /**
     * Create a new geometry.
     */
    model_ptr<Geometry>
    newGeometry(GeomType geomType, size_t initialCapacity = 2, bool fixedSize = false) override;

    /**
     * Create a new geometry view.
     */
    model_ptr<Geometry> newGeometryView(
        GeomType geomType,
        uint32_t offset,
        uint32_t size,
        const model_ptr<Geometry>& base) override;

    /**
     * Create a new list of qualified source-data references.
     */
    model_ptr<SourceDataReferenceCollection>
    newSourceDataReferenceCollection(std::span<QualifiedSourceDataReference> list) override;

    /**
     * Create a new validity.
     */
    model_ptr<Validity> newValidity();

    /**
     * Create a new validity collection.
     */
    model_ptr<MultiValidity>
    newValidityCollection(size_t initialCapacity = 2, bool fixedSize = false);

    /**
     * Upgrade one compact simple-validity occurrence in-place to full storage.
     */
    simfil::ModelNodeAddress materializeSimpleValidity(
        simfil::ModelNodeAddress simpleAddress,
        simfil::ArrayIndex ownerMembers,
        uint32_t ownerElementIndex,
        Validity::Direction direction);

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
    tl::expected<void, simfil::Error> write(std::ostream& outputStream) override;

    /** Convert to (Geo-) JSON. */
    nlohmann::json toJson() const override;

    /** Validate every emitted feature against the JSON Schema attached to this layer's LayerInfo.
     */
    void validateSchema() const;

    /** Return the layer schema attached through this tile's LayerInfo, if available. */
    [[nodiscard]] std::shared_ptr<LayerSchema const> layerSchema() const;

    /** Return a schema entry by schema key, or by feature type name as a convenience. */
    [[nodiscard]] LayerSchema::Entry const* getSchema(std::string_view typeName) const;

    /** Import a mapget-flavoured or best-effort GeoJSON feature collection into this tile. */
    void fromJson(nlohmann::json const& json, GeoJsonImportOptions const& options = {});

    /**
     * Inspect or replace the optional name of the tile-level GLB attachment.
     *
     * Attachment bytes are transferred independently through the datasource
     * attachment API and are never serialized into this model.
     * `GeomType::GltfNodeIndex` refers to nodes inside the named GLB.
     */
    [[nodiscard]] std::optional<std::string> const& glbAttachmentName() const;
    void setGlbAttachmentName(std::optional<std::string> name);

    /** Report serialized size stats for feature-layer data and model-pool columns. */
    [[nodiscard]] nlohmann::json serializationSizeStats() const;

    /** Report retained feature-model capacity without counting shared metadata. */
    [[nodiscard]] MemoryUsageBreakdown memoryUsage() const override;

    /** Access number of stored features */
    size_t size() const;

    /** Access total number of geometry vertices across this tile. */
    [[nodiscard]] uint64_t numVertices() const;

    /** Access layer-wide geometry anchor used for anchor-relative vertex encoding. */
    [[nodiscard]] Point geometryAnchor() const override;
    void setGeometryAnchor(Point const& anchor);

    /** Access feature at index i */
    model_ptr<Feature> at(size_t i) const;

    /** Access feature through its id. */
    model_ptr<Feature> find(std::string_view const& featureId) const;
    model_ptr<Feature>
    find(std::string_view const& type, KeyValueViewPairs const& queryIdParts) const;
    model_ptr<Feature> find(std::string_view const& type, KeyValuePairs const& queryIdParts) const;

    /** Apply a portable exact or filtered selector to this already-loaded tile. */
    [[nodiscard]] tl::expected<std::vector<model_ptr<Feature>>, simfil::Error>
    find(FeatureLayerSelector const& selector) const;

    /**
     * Evaluate a (potentially cached) simfil query on this pool
     *
     * @param query         Simfil query
     * @param node          Model root node to query
     * @param anyMode       Auto-wrap expression in `any(...)`
     * @param autoWildcard  Deprecated compatibility flag. When a node schema is
     *                      available, this enables schema-backed shorthand
     *                      rewrites. Without schema metadata it has no effect.
     */
    struct QueryResult
    {
        // The list of values resulting from the query evaluation.
        std::vector<simfil::Value> values;

        // A map of traces for debugging or understanding query execution,
        // where the key is a string identifier and the value is a trace object.
        std::map<std::string, simfil::Trace> traces;

        // Diagnostics information, such as warnings or errors,
        // generated during query evaluation.
        simfil::Diagnostics diagnostics;
    };
    tl::expected<TileFeatureLayer::QueryResult, simfil::Error> evaluate(
        std::string_view query,
        ModelNode const& node,
        bool anyMode = true,
        bool autoWildcard = true);

    tl::expected<TileFeatureLayer::QueryResult, simfil::Error>
    evaluate(std::string_view query, bool anyMode = true, bool autoWildcard = true);

    /**
     * Get auto-completion candidates at `point` of a query.
     */
    tl::expected<std::vector<simfil::CompletionCandidate>, simfil::Error> complete(
        std::string_view query,
        int point,
        ModelNode const& node,
        simfil::CompletionOptions const& opts);

    /**
     * Collect query diagnostics for an evaluated query.
     * If the query has not yet been evaluated it gets compiled.
     */
    tl::expected<std::vector<simfil::Diagnostics::Message>, simfil::Error> collectQueryDiagnostics(
        std::string_view query,
        const simfil::Diagnostics& diag,
        bool anyMode = true);

    /**
     * Change the string pool of this model to a different one.
     * Note: This will potentially create new string entries in the newDict,
     * for field names which were not there before.
     */
    tl::expected<void, simfil::Error>
    setStrings(std::shared_ptr<simfil::StringPool> const& newPool) override;

    /**
     * Create a copy of otherFeature in this layer with the given type
     * and id-parts. If a feature with that ID already exists in this layer,
     * the attributes/relations/geometries from otherFeature will simply
     * be appended to the existing feature.
     */
    void clone(
        CloneCache& clonedModelNodes,
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
        CloneCache& clonedModelNodes,
        TileFeatureLayer::Ptr const& otherLayer,
        simfil::ModelNode::Ptr const& otherNode);

    using ColumnId = TileFeatureModelLayerBase::ColumnId;

protected:
    /** Get the primary id composition for the given feature type. */
    std::vector<IdPart> const& getPrimaryIdComposition(std::string_view const& type) const;

    /** Resolve schema IDs used by mapget's custom feature-model nodes. */
    [[nodiscard]] simfil::SchemaId featureSchemaId(std::string_view featureType) const;
    [[nodiscard]] simfil::SchemaId featurePropertiesSchemaId(std::string_view featureType) const;
    [[nodiscard]] simfil::SchemaId attributeLayerMapSchemaId(std::string_view featureType) const;
    [[nodiscard]] simfil::SchemaId schemaIdForKey(std::string_view key) const;
    [[nodiscard]] simfil::SchemaId childSchemaId(
        simfil::SchemaId parent,
        std::string_view fieldName,
        std::optional<simfil::Schema::Kind> preferredKind = std::nullopt) const;
    [[nodiscard]] simfil::SchemaId childSchemaId(
        simfil::SchemaId parent,
        simfil::StringId field,
        std::optional<simfil::Schema::Kind> preferredKind = std::nullopt) const;
    void applyObjectSchema(simfil::Object& object, simfil::SchemaId schemaId) const;
    void applyArraySchema(simfil::Array& array, simfil::SchemaId schemaId) const;

    /**
     * Create a new attribute layer collection.
     */
    model_ptr<AttributeLayerList>
    newAttributeLayers(size_t initialCapacity = 8, bool fixedSize = false);

    /**
     * Generic node resolution overload.
     */
    tl::expected<void, simfil::Error>
    resolve(const simfil::ModelNode& n, const ResolveFn& cb) const override;

    [[nodiscard]] model_ptr<FeatureId>
    resolveFeatureIdNode(simfil::ModelNode const& node) const override;
    [[nodiscard]] model_ptr<PointNode>
    resolvePointNode(simfil::ModelNode const& node) const override;

    [[nodiscard]] Feature::ComplexData const* featureComplexDataOrNull(uint32_t featureIndex) const;
    [[nodiscard]] Feature::ComplexData* featureComplexDataOrNull(uint32_t featureIndex);
    Feature::ComplexData& ensureFeatureComplexData(uint32_t featureIndex);

    /** Return persisted data for one inserted attribute point. */
    [[nodiscard]] AttrPoint::Data const& attrPointData(uint32_t index) const;

    /** Return persisted data for one shared attribute-point sequence. */
    [[nodiscard]] AttrPointSequence::Data const& attrPointSequenceData(uint32_t index) const;

    /** Return mutable persisted data for one shared attribute-point sequence. */
    [[nodiscard]] AttrPointSequence::Data& attrPointSequenceData(uint32_t index);

    /** Append one point to a sequence while preserving compact contiguous storage. */
    model_ptr<AttrPoint> appendAttrPoint(
        uint32_t sequenceIndex,
        uint32_t logicalIndex,
        Point const& point,
        model_ptr<SourceDataReferenceCollection> const& sourceData);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Primary template for ADL-based resolve hooks (specialized in featurelayer.cpp).
template <typename Target>
simfil::model_ptr<Target> resolveInternal(
    simfil::res::tag<Target>,
    TileFeatureLayer const& model,
    simfil::ModelNode const& node);

}  // namespace mapget
