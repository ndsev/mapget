#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
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

/** Strict creation rejected a duplicate; producers may recover at a record boundary. */
class DuplicateFeatureIdError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct FeatureLayerSelector;
class SimfilExpressionCache;

/**
 * The PartitionFeatureLayer class represents a specific map layer within a tile or
 * object partition. It is a container for complete map features and their model storage.
 * You can iterate over all contained features using `for (auto&& feature : tileFeatureLayer)`.
 *
 * Every returned node address belongs to this layer; retain the layer or its
 * model_ptr views while using feature, geometry or attribute nodes.
 */
class PartitionFeatureLayer : public PartitionFeatureModelLayerBase
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
    friend model_ptr<Target> resolveInternal(
        simfil::res::tag<Target>,
        PartitionFeatureLayer const&,
        simfil::ModelNode const&);

public:
    // Keep ModelPool::resolve<T> overloads visible alongside the override below.
    using PartitionFeatureModelLayerBase::resolve;
    using Ptr = std::shared_ptr<PartitionFeatureLayer>;
    static constexpr std::string_view GLB_ATTACHMENT_MIME_TYPE = "model/gltf-binary";

    /** Identify one source node clone within an optional destination-feature scope. */
    struct CloneCacheKey
    {
        PartitionFeatureLayer const* model_ = nullptr;
        uint32_t address_ = 0;
        uint32_t targetFeatureAddress_ = 0;

        [[nodiscard]] bool operator==(CloneCacheKey const& other) const = default;
    };

    /** Hash source-node and destination-feature identity for clone reuse. */
    struct CloneCacheKeyHash
    {
        [[nodiscard]] size_t operator()(CloneCacheKey const& key) const noexcept
        {
            auto const modelHash = std::hash<PartitionFeatureLayer const*>{}(key.model_);
            auto const addressHash = std::hash<uint32_t>{}(key.address_);
            auto const targetHash = std::hash<uint32_t>{}(key.targetFeatureAddress_);
            auto const sourceHash =
                modelHash ^ (addressHash + 0x9e3779b9U + (modelHash << 6U) + (modelHash >> 2U));
            return sourceHash ^
                (targetHash + 0x9e3779b9U + (sourceHash << 6U) + (sourceHash >> 2U));
        }
    };

    /** Reuse clones within one destination layer and a live source set.
     * Keys borrow source addresses and omit destination-layer identity; do not
     * reuse across destination layers or after source address lifetimes end.
     */
    using CloneCache = std::unordered_map<CloneCacheKey, simfil::ModelNode::Ptr, CloneCacheKeyHash>;

    /**
     * This constructor initializes a new PartitionFeatureLayer instance.
     * The partition kind must match layerInfo, whose finalized schema is retained.
     * @param tileId Tagged tile or object partition owning the features. For a spatial
     *  tile, features should lie roughly within its area; this is not a strict geometry bound.
     * @param stringPoolId Producer dictionary namespace, distinct from catalog sourceId.
     * @param mapId ID of the map which the layer belongs to.
     * @param layerInfo Information about the map layer this feature is associated with.
     *  Each feature in this layer must have a feature type which is also present in
     *  the layer. Therefore, feature ids from this layer can be verified to conform
     *  to one of the allowed feature id compositions for the feature type.
     * @param strings Shared string dictionary, which allows compressed storage
     *  of object field name strings. It is auto-filled, and one instance may be used
     *  by multiple PartitionFeatureLayer instances.
     */
    PartitionFeatureLayer(
        PartitionId tileId,
        std::string const& stringPoolId,
        std::string const& mapId,
        std::shared_ptr<LayerInfo> const& layerInfo,
        std::shared_ptr<simfil::StringPool> const& strings);

    /**
     * Constructor which parses a PartitionFeatureLayer from a binary byte buffer.
     * Checks index structure; call validate() after shared construction to check
     * feature-ID uniqueness and index consistency against stored ID values.
     * @param input The binary bytes to parse.
     * @param layerInfoResolveFun Function which will be called to retrieve
     *  a layerInfo object for the layer name stored for the tile.
     * @param stringPoolGetter Function which will be called to retrieve
     *  a string pool for the node name of the tile.
     */
    PartitionFeatureLayer(
        const std::vector<uint8_t>& input,
        LayerInfoResolveFun const& layerInfoResolveFun,
        StringPoolResolveFun const& stringPoolGetter,
        SchemaResolveFun const& schemas = {});

    /**
     * Get/Set common id prefix for all features in this layer.
     * Note: The prefix MUST be set before any feature is added to the tile.
     */
    void setIdPrefix(KeyValueViewPairs const& prefix);
    model_ptr<Object> getIdPrefix();
    model_ptr<Object> getIdPrefix() const override;

    /** Destructor for the PartitionFeatureLayer class. */
    ~PartitionFeatureLayer() override;

    /**
     * Creates a new feature and insert it into this tile layer.
     * The featureIdParts (which do not include the getIdPrefix of the layer)
     * must conform to an existing UniqueIdComposition for the feature typeId
     * within the associated layer, or a runtime error will be raised. Concrete
     * features must use the primary composition; secondary IDs are references.
     * An existing feature with the same type and required primary ID parts
     * causes an error before storage is changed. Optional parts do not distinguish IDs.
     * @param typeId Specifies the type of the feature.
     * @param featureIdParts Uniquely identifying information for the feature,
     * according to the requirements of typeId. Do not include the tile feature
     * prefix. If empty, an error will be thrown.
     */
    model_ptr<Feature>
    newFeature(std::string_view const& typeId, KeyValueViewPairs const& featureIdParts);

    /**
     * Create a new relation. Use this function to create a named reference to another
     * feature, which may also have optional source/target validity geometry.
     * target must be nonnull and belong to this layer; its address is retained.
     * Attach the result with Feature::addRelation to assign its feature-local ordinal.
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
     * Create a shared sequence over a line with at least two shape points.
     * Both nodes must belong to this layer, and the geometry must already be
     * attached to the supplied feature. Invalid ownership or shape throws.
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
     * Create a point-range view over a nonnull geometry in this same layer.
     * Bounds and GLTF geometries are unsupported. Offsets accumulate through
     * nested views; range validation occurs when the point buffer is resolved.
     */
    model_ptr<Geometry> newGeometryView(
        GeomType geomType,
        uint32_t offset,
        uint32_t size,
        const model_ptr<Geometry>& base) override;

    /**
     * Create a new list of qualified source-data references by copying the supplied
     * items. Their string IDs must already belong to this layer's dictionary.
     * The compact address supports at most 15 entries and a 20-bit start index;
     * callers must keep both limits when appending to the shared reference arena.
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
     * for-loops to iterate over all features in a PartitionFeatureLayer.
     */
    struct Iterator
    {
        Iterator(PartitionFeatureLayer const& layer, size_t i) : layer_(layer), i_(i) {}

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
        PartitionFeatureLayer const& layer_;
        size_t i_ = 0;
    };

    /**
     * begin()/end() support range-based for-loops to iterate over all
     * features in a PartitionFeatureLayer.
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

    /** Check model storage, feature-ID uniqueness, and index consistency after construction. */
    [[nodiscard]] std::vector<std::string> checkForErrors() const override;

    /** Return the layer schema attached through this tile's LayerInfo, if available. */
    [[nodiscard]] simfil::model_ptr<LayerSchema> layerSchema() const;

    /** Translate a fresh, exclusively owned native model into a composed schema.
     * Keeps payload addresses/identities intact; invalidates compiled query state.
     * Metadata must describe the same layer and a composition containing this graph.
     */
    void rebindSchema(std::shared_ptr<LayerInfo> const& info);

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

    /** Report payload-size estimates, arena savings and geometry/validity usage.
     * Counts inspect stored/reachable records; totals are not an exact serialized
     * stream length or a census of all detached geometry allocations.
     */
    [[nodiscard]] nlohmann::json serializationSizeStats() const;

    /** Report retained feature-model capacity without counting shared metadata. */
    [[nodiscard]] MemoryUsageBreakdown memoryUsage() const override;

    /** Access number of stored features */
    size_t size() const;

    /** Access total number of geometry vertices across this tile. */
    [[nodiscard]] uint64_t numVertices() const;

    /** Access the absolute origin used to decode anchor-relative vertices. */
    [[nodiscard]] Point geometryAnchor() const override;

    /** Set the origin before writing vertices; existing offsets are not re-encoded. */
    void setGeometryAnchor(Point const& anchor);

    /** Access feature at index i */
    model_ptr<Feature> at(size_t i) const;

    /** Find a feature by primary identity, ignoring optional ID parts. Parts must follow
     * composition order and include the layer prefix. The string overload returns
     * null on parse failure; an unknown explicit type may throw during lookup.
     */
    model_ptr<Feature> find(std::string_view const& featureId) const;
    model_ptr<Feature>
    find(std::string_view const& type, KeyValueViewPairs const& queryIdParts) const;
    model_ptr<Feature> find(std::string_view const& type, KeyValuePairs const& queryIdParts) const;

    /** Apply one portable selector to this already-loaded tile. */
    [[nodiscard]] tl::expected<std::vector<model_ptr<Feature>>, simfil::Error>
    find(
        FeatureLayerSelector const& selector,
        std::function<bool()> const& cancellationCheck = {}) const;

    /**
     * Apply several portable selectors while sharing tile-wide SIMFIL setup.
     *
     * Results retain selector order. A cancelled traversal returns an equally
     * sized collection of empty results; callers providing a cancellation check
     * must inspect their own state before consuming it.
     * A request-owned expression cache may be supplied to share compilation
     * with selector evaluation on other tiles.
     */
    [[nodiscard]] tl::expected<std::vector<std::vector<model_ptr<Feature>>>, simfil::Error> find(
        std::span<FeatureLayerSelector const> selectors,
        std::function<bool()> const& cancellationCheck = {},
        SimfilExpressionCache* expressionCache = nullptr) const;

    /**
     * Evaluation output with result values, named trace counters and diagnostics.
     * Values can retain model nodes; keep their owners alive while consuming them.
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

    /**
     * Evaluate a potentially cached simfil query on the supplied model node.
     * This layer serializes access to its shared evaluator.
     *
     * @param query Simfil query.
     * @param node Model root node to query.
     * @param anyMode Auto-wrap the expression in any(...).
     * @param autoWildcard Compatibility flag for schema-backed shorthand rewrites.
     *  It has no effect without a node schema.
     */
    tl::expected<PartitionFeatureLayer::QueryResult, simfil::Error> evaluate(
        std::string_view query,
        ModelNode const& node,
        bool anyMode = true,
        bool autoWildcard = true);

    /** Evaluate against the first root only; an empty layer returns the root lookup error. */
    tl::expected<PartitionFeatureLayer::QueryResult, simfil::Error>
    evaluate(std::string_view query, bool anyMode = true, bool autoWildcard = true);

    /**
     * Complete the query at byte offset point using this node's schema/data.
     * A private dictionary copy prevents query text from changing streamed IDs.
     */
    tl::expected<std::vector<simfil::CompletionCandidate>, simfil::Error> complete(
        std::string_view query,
        int point,
        ModelNode const& node,
        simfil::CompletionOptions const& opts);

    /**
     * Format the supplied evaluation diagnostics after ensuring the query
     * compiles against the first root's schema. This does not evaluate the query.
     */
    tl::expected<std::vector<simfil::Diagnostics::Message>, simfil::Error> collectQueryDiagnostics(
        std::string_view query,
        const simfil::Diagnostics& diag,
        bool anyMode = true);

    /**
     * Change the string pool of this model to a different one.
     * The new pool may acquire missing names. Requires exclusive access: query
     * state is invalidated, and custom columns are remapped after generic storage.
     * Failure does not roll back earlier dictionary/storage changes.
     */
    tl::expected<void, simfil::Error>
    setStrings(std::shared_ptr<simfil::StringPool> const& newPool) override;

    /**
     * Create a copy of otherFeature in this layer with the given type
     * and full primary id-parts. Existing identities receive attributes/layers
     * and relations; already attached cloned geometry addresses are not duplicated.
     * The destination schema must admit source types (same artifact or a composed
     * contributor). A populated properties container cannot change concrete type.
     * Node-copy limitations and partial-failure behavior below also apply.
     */
    void clone(
        CloneCache& clonedModelNodes,
        PartitionFeatureLayer::Ptr const& otherLayer,
        Feature const& otherFeature,
        std::string_view const& type,
        KeyValueViewPairs idParts);

    /**
     * Create a copy of otherNode (which lives in otherLayer) in this layer.
     * The required cache preserves shared nodes within this destination. Geometry
     * views are materialized; whole Feature/FeatureProperties and standalone helper
     * views are unsupported. Validity copies currently omit featureId/geometryName
     * selectors. Failures can leave partial destination/cache state; discard both
     * before retrying an operation that requires an all-or-nothing result.
     */
    simfil::ModelNode::Ptr clone(
        CloneCache& clonedModelNodes,
        PartitionFeatureLayer::Ptr const& otherLayer,
        simfil::ModelNode::Ptr const& otherNode);

    using ColumnId = PartitionFeatureModelLayerBase::ColumnId;

protected:
    /** Describe a feature clone whose local references must follow a remapped ID. */
    struct CloneContext
    {
        model_ptr<FeatureId> sourceFeatureId_;
        model_ptr<Feature> targetFeature_;
    };

    /** Clone one node while propagating an optional feature-remapping context. */
    simfil::ModelNode::Ptr cloneNode(
        CloneContext const& context,
        CloneCache& clonedModelNodes,
        PartitionFeatureLayer::Ptr const& otherLayer,
        simfil::ModelNode::Ptr const& otherNode);

    /** Translate a copied producer type into this layer's composed graph. */
    simfil::SchemaType remapSchemaType(simfil::SchemaType const& reference) const;

    /** Get the primary id composition for the given feature type. */
    FeatureIdentity::Composition getPrimaryIdComposition(std::string_view const& type) const;

    /** Resolve schema IDs used by mapget's custom feature-model nodes. */
    [[nodiscard]] simfil::TypeId featureSchemaId(std::string_view featureType) const;
    [[nodiscard]] simfil::TypeId featurePropertiesSchemaId(std::string_view featureType) const;
    [[nodiscard]] simfil::TypeId attributeLayerMapSchemaId(std::string_view featureType) const;
    [[nodiscard]] simfil::TypeId schemaIdForKey(std::string_view key) const;
    [[nodiscard]] simfil::TypeId childSchemaId(
        simfil::TypeId parent,
        std::string_view fieldName,
        std::optional<simfil::SchemaType::Kind> preferredKind = std::nullopt) const;
    [[nodiscard]] simfil::TypeId childSchemaId(
        simfil::TypeId parent,
        simfil::StringId field,
        std::optional<simfil::SchemaType::Kind> preferredKind = std::nullopt) const;
    void applyObjectSchema(simfil::Object& object, simfil::TypeId schemaId) const;
    void applyArraySchema(simfil::Array& array, simfil::TypeId schemaId) const;

    /** Preserve a concrete binding when the containing declaration explicitly admits it. */
    [[nodiscard]] simfil::TypeId
    checkedContainerSchema(simfil::TypeId concrete, simfil::TypeId declared, simfil::ValueType type)
        const;

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
    PartitionFeatureLayer const& model,
    simfil::ModelNode const& node);

}  // namespace mapget
