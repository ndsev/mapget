#pragma once

#include <compare>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <tl/expected.hpp>

#include "mapget/model/featureschema.h"
#include "mapget/model/info.h"
#include "mapget/model/memory.h"
#include "mapget/model/stringpool.h"
#include "simfil/error.h"
#include "simfil/evaluation.h"
#include "simfil/model/model.h"
#include "simfil/model/schema-type.h"

namespace mapget
{
class MapSchema;

/** Owner-retaining view of one named layer in an immutable MapSchema.
 * Builder methods write to the common owner before finalization. Type IDs
 * are graph-global; same-named feature aliases remain local to this layer.
 */
class LayerSchema : public simfil::ModelNodeBase
{
public:
    /** Source-owned forward relation family; incoming possibilities are derived from produced
     * owners. */
    struct RelationDeclaration
    {
        enum class Locality { Unknown, SameContainer, MayCrossContainer };
        std::string ownerType_;
        std::string name_;
        std::vector<std::string> targetTypes_;

        /** Producer confirms complete conversion coverage for this declared family. */
        bool complete_ = false;

        /** Producer routing fact, never inferred from the target type alone. */
        Locality locality_ = Locality::Unknown;

        /** Match a typed target and stored forward relation family. */
        [[nodiscard]] bool
        permitsIncoming(std::string_view owner, std::string_view name, std::string_view target)
            const;
    };

    /** Read producer relation contracts without treating referenced types as feature owners. */
    [[nodiscard]] std::vector<RelationDeclaration> relationDeclarations() const;
    [[nodiscard]] std::vector<RelationDeclaration> relationsTo(std::string_view targetType) const;

    /** Distinguish an explicitly empty relation index from absent producer metadata. */
    [[nodiscard]] bool hasRelationDeclarations() const;

    /** Publish native forward contracts, including an explicitly empty inventory. */
    void setRelationDeclarations(std::vector<RelationDeclaration> declarations);

    using Composition = simfil::SchemaType::Composition;
    static constexpr auto FeatureKind = simfil::SchemaType::makeKind(
        StringPool::SchemaFeatureStr,
        simfil::valueTypeAffinity(simfil::ValueType::Object));
    static constexpr auto AttributeKind = simfil::SchemaType::makeKind(
        StringPool::SchemaAttributeStr,
        simfil::valueTypeAffinity(simfil::ValueType::Object));

    /** Typed raw bits; declarations retain exact masks, aliases and unknown bits. */
    static constexpr auto BitmaskKind = simfil::SchemaType::makeKind(
        StringPool::SchemaBitmaskStr,
        simfil::valueTypeAffinity(simfil::ValueType::Bitmask));

    /** Owner classification for a schema path starting at a known root. */
    enum class PathOwnerKind {
        Unknown,
        Feature,
        Attribute,
    };

    /** Concrete attribute context owning a schema path. */
    struct AttributePathOwner
    {
        std::string featureType_;
        std::string attributeLayerName_;
        std::string attributeName_;
        simfil::TypeId attributeSchema_ = simfil::NoTypeId;

        /** Owning feature layer; feature type names can repeat across layers. */
        std::string featureLayer_;
    };

    /** Result returned by ownerForPath for downstream scope decisions. */
    struct PathOwner
    {
        PathOwnerKind kind_ = PathOwnerKind::Unknown;
        AttributePathOwner attribute_;
    };

    /** One path segment expressed before binding schema fields to a StringPool. */
    struct NamedPathSegment
    {
        simfil::SchemaPathSegment::Kind kind_ = simfil::SchemaPathSegment::Kind::Field;
        std::string field_;

        auto operator<=>(NamedPathSegment const&) const = default;
    };

    using NamedSchemaPath = std::vector<NamedPathSegment>;

    /** User-facing scope request before schema normalization chooses concrete execution. */
    enum class SearchQueryRequestedScope {
        Feature,
        Attribute,
        Auto,
    };

    /** Concrete scope passed to search evaluation after normalization. */
    enum class SearchQueryConcreteScope {
        Feature,
        Attribute,
    };

    /** Schema-backed query normalization result for one feature layer. */
    struct SearchQueryNormalization
    {
        std::string originalQuery_;
        std::string normalizedQuery_;
        SearchQueryRequestedScope requestedScope_ = SearchQueryRequestedScope::Feature;
        SearchQueryConcreteScope concreteScope_ = SearchQueryConcreteScope::Feature;
        std::vector<AttributePathOwner> attributeScopes_;
        size_t attributeScopeCandidateCount_ = 0;
        bool rewriteSuppressed_ = false;
        std::string rewriteSuppressionReason_;
        std::vector<std::string> matchedFeatureTypes_;
        std::string compiledAstDebug_;
    };

    /** One explicit meaning of confirmed search input, before any tile request.
     * expression_ is the ordinary, editable Simfil query offered to the user.
     * query_ evaluates that expression in scope_'s concrete root. Attribute
     * owners describe this preparation's styling/compilation context; they
     * are not restrictions to persist when the user adds another map.
     */
    struct SearchInterpretation
    {
        std::string id_;
        std::string label_;
        std::string kind_;
        std::string symbol_;
        std::string expression_;
        std::string query_;
        SearchQueryConcreteScope scope_ = SearchQueryConcreteScope::Feature;
        std::vector<AttributePathOwner> attributeScopes_;
        std::vector<std::string> featureTypes_;
    };

    /** Enumerate distinct schema meanings; the caller must choose when more than one exists. */
    [[nodiscard]] tl::expected<std::vector<SearchInterpretation>, simfil::Error>
    searchInterpretations(std::string_view query, SearchQueryRequestedScope scope) const;

    /**
     * Portable predicates for an enum token already identified by completion.
     * This skips field-path discovery and expression normalization: those belong
     * to confirmed search preparation, not to each suggested field while typing.
     */
    [[nodiscard]] std::vector<SearchInterpretation>
    enumSearchInterpretations(std::string const& symbol, SearchQueryRequestedScope scope) const;

    using Ptr = simfil::model_ptr<LayerSchema>;
    using ModelType = MapSchema;

    explicit LayerSchema(simfil::detail::mp_key key);
    LayerSchema(
        simfil::ModelConstPtr owner,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);

    /** Identity is the exact LayerInfo::layerId_, not a display alias. */
    [[nodiscard]] std::string_view name() const;
    [[nodiscard]] LayerType layerType() const;
    [[nodiscard]] std::shared_ptr<MapSchema const> mapSchema() const;

    /** Register a produced feature root and its map-wide identity. */
    void addFeatureType(FeatureTypeInfo info, simfil::TypeId rootSchema = 0);

    /** Declare production using an identity already registered on the owning map. */
    void addFeatureType(std::string_view name, simfil::TypeId rootSchema = 0);

    /** Build the native Feature envelope and identity fields once. Supplied properties
     * and layer-map objects remain extensible; reserved members cannot be redefined.
     * Zero arguments create empty containers with canonical lookup keys. */
    [[nodiscard]] FeatureSchemaType newFeature(
        FeatureTypeInfo info,
        simfil::SchemaType properties = {},
        simfil::SchemaType attributeLayers = {});

    /** Create a produced root using an identity already declared on MapSchema. */
    [[nodiscard]] FeatureSchemaType newFeature(
        std::string_view name,
        simfil::SchemaType properties = {},
        simfil::SchemaType attributeLayers = {});

    /** Atomically declare an Attribute's container edge and named owner without cloning it.
     * Existing exact edges are reused; incompatible targets and thin legacy roots fail. */
    void bindAttribute(
        simfil::SchemaType type,
        std::string_view featureType,
        std::string_view layer,
        std::string_view name);

    /** Test this layer's production membership, independently of map-wide identity lookup. */
    [[nodiscard]] bool producesFeatureType(std::string_view name) const;

    /** Borrow any map-wide identity; this does not assert that this layer produces it. */
    [[nodiscard]] FeatureIdentity
    getTypeInfo(std::string_view name, bool throwIfMissing = true) const;

    /** Project native identity and forward relations without expanding types.
     * Optional reserve runs per visited entry, before copying text/containers, with a conservative
     * JSON byte allowance. It may throw to enforce caller work/time/size limits; zero bytes still
     * indicate traversal work. Borrowed declarations never require a whole-inventory copy.
     */
    [[nodiscard]] nlohmann::json
    featureTypeContract(std::string_view name, std::function<void(size_t)> const& reserve = {})
        const;
    void setRootSchema(simfil::TypeId root);
    [[nodiscard]] simfil::TypeId rootSchema() const;

    /** Borrow the layer's canonical root; retain the layer or map owner during traversal. */
    [[nodiscard]] simfil::SchemaType rootType() const;

    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t index) const override;
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t index) const override;
    [[nodiscard]] simfil::ModelNode::Ptr get(simfil::StringId const& field) const override;
    bool iterate(IterCallback const& callback) const override;

    /** Convenience factory returning nullptr for a missing/null JSON Schema transport payload. */
    [[nodiscard]] static Ptr fromJsonSchema(nlohmann::json schema);

    /** Serialize this typed schema to its JSON Schema transport representation. */
    [[nodiscard]] nlohmann::json toJsonSchema() const;

    /** Report this view only; shared storage is accounted by MapSchema::memoryUsage(). */
    [[nodiscard]] MemoryUsageBreakdown memoryUsage() const;

    /** Add one object/array/value schema node and return its stable TypeId. */
    [[nodiscard]] simfil::TypeId addSchema(
        simfil::SchemaType::Kind kind,
        std::string key = {},
        std::string metaType = {},
        std::string jsonPointer = {});

    /** Import a declarative JSON Schema fragment into this graph, without finalizing it.
     * Local references resolve within the fragment or to previously registered schema keys.
     */
    [[nodiscard]] simfil::TypeId addJsonSchema(nlohmann::json const& schema);

    /** Retain non-structural JSON Schema constraints and producer annotations on a type.
     * Structural fields and mapget's transport identities cannot be overridden here.
     */
    void setJsonSchemaAnnotations(simfil::TypeId id, nlohmann::json annotations);

    /** Register an additional lookup key for an existing schema node. */
    void registerSchemaKey(std::string key, simfil::TypeId id);

    /** Attach zserio type metadata used by completion/result-coloring consumers. */
    void setZserioType(simfil::TypeId schemaId, std::string zserioType);

    /** Attach mapget attribute metadata and ownership for one concrete Attribute node. */
    void setAttributeMetadata(
        simfil::TypeId schemaId,
        AttributePathOwner owner,
        std::string attributeType,
        std::string zserioType = {});

    /** Finalize the entire owning MapSchema after direct construction. */
    void finalize();

    /** Return the deterministic key used by feature schema annotations. */
    [[nodiscard]] static std::string featureKey(std::string_view featureType);

    /** Return the deterministic key used by Feature.properties annotations. */
    [[nodiscard]] static std::string featurePropertiesKey(std::string_view featureType);

    /** Return the deterministic key used by Feature.properties.layer annotations. */
    [[nodiscard]] static std::string attributeLayerMapKey(std::string_view featureType);

    /** Return the deterministic key used by AttributeContainer schema annotations. */
    [[nodiscard]] static std::string
    attributeContainerKey(std::string_view featureType, std::string_view attributeLayerName);

    /** Return the deterministic key used by concrete Attribute schema annotations. */
    [[nodiscard]] static std::string attributeKey(
        std::string_view featureType,
        std::string_view attributeLayerName,
        std::string_view attributeTypeCode);

    /** Identify one imported Attribute definition in its concrete owner context.
     * sourceDefinitionKey is its document-local JSON pointer (e.g. /definitions/Foo),
     * not a physical datasource/version convention. All four parts are required. */
    [[nodiscard]] static std::string attributeKey(
        std::string_view featureType,
        std::string_view attributeLayerName,
        std::string_view attributeTypeCode,
        std::string_view sourceDefinitionKey);

    /** Resolve a local key, then a map-wide key; zero means neither namespace contains it. */
    [[nodiscard]] simfil::TypeId schemaId(std::string_view key) const;

    /** Return the mapget attribute type-code for an Attribute schema, if any. */
    [[nodiscard]] std::string_view attributeTypeCode(simfil::TypeId schemaId) const;

    /** Return enum/zserio type names attached to a constant-like completion candidate. */
    [[nodiscard]] std::vector<std::string>
    constantTypeNames(simfil::TypeId schemaId, std::string_view symbolName) const;

    /** Resolve the Feature object schema for a concrete mapget feature type. */
    [[nodiscard]] simfil::TypeId featureSchema(std::string_view featureType) const;

    /**
     * Return the typed attribute-query root prepared by finalize(). It preserves
     * payload/alias metadata and adds runtime overlay fields, including $feature.
     * No model samples or mutations of a published graph are required.
     */
    [[nodiscard]] simfil::TypeId
    attributeQuerySchema(std::string_view featureType, simfil::TypeId attributeSchema) const;

    /** Resolve the Feature.properties object schema for a concrete feature type. */
    [[nodiscard]] simfil::TypeId featurePropertiesSchema(std::string_view featureType) const;

    /** Resolve the Feature.properties.layer object schema for a concrete feature type. */
    [[nodiscard]] simfil::TypeId attributeLayerMapSchema(std::string_view featureType) const;

    /** Resolve a field through object, array-element and alternative schemas.
     * The first matching field occurrence wins. preferredKind selects among that
     * occurrence's types, falling back to its first nonzero type; zero means absent.
     */
    [[nodiscard]] simfil::TypeId childSchema(
        simfil::TypeId parent,
        std::string_view fieldName,
        std::optional<simfil::SchemaType::Kind> preferredKind = std::nullopt) const;

    /** Classify whether a path belongs to the feature itself or to an attribute branch. */
    [[nodiscard]] PathOwner ownerForPath(
        std::string_view featureType,
        simfil::TypeId rootSchema,
        std::span<const std::string> fieldPath) const;

    /** Find declared fields, type names and symbols below selected roots, without expanding data.
     * Requires a finalized owner and a nonempty search term of at most 128 bytes.
     * Optional featureTypes must name the produced feature owning each root; otherwise paths
     * are relative to root IDs. Results rank distinct types and retain representative contexts,
     * not every possible path through a shared graph. Lookup-only context edges are not followed.
     * Caller limits cover work, time, cancellation, result count and serialization stack depth;
     * maxBytes bounds retained candidate/output text before copying. The JSON result contains
     * items, complete/reason, discovery statistics and bounded rootMatches for caller navigation.
     * summary reports consumed work; no datasource authority or transport policy lives here.
     */
    [[nodiscard]] nlohmann::json find(
        std::string_view text,
        std::span<simfil::TypeId const> roots,
        std::span<std::string const> featureTypes,
        simfil::EvaluationOptions const& limits,
        size_t maxBytes,
        simfil::EvaluationSummary* summary = nullptr) const;

    /** Return sorted, unique paths for an attribute name or type-code shorthand.
     * Each matching payload chooses its first scalar, skipping infrastructure
     * metadata. Cycles are ignored; traversal/depth budget exhaustion throws.
     */
    [[nodiscard]] std::vector<NamedSchemaPath>
    scalarFieldPathsForAttribute(simfil::TypeId rootSchema, std::string_view attributeTypeCode)
        const;

    /** Copy names explicitly declared as produced by this layer, in lexical order. */
    [[nodiscard]] std::vector<std::string> featureTypes() const;

    /** Return all searchable attribute contexts represented by this layer schema. */
    [[nodiscard]] std::vector<AttributePathOwner> attributeScopes() const;

    /** Named use sites of one physical Attribute, or the current feature's query overlay. */
    [[nodiscard]] std::vector<AttributePathOwner> attributeOwners(simfil::TypeId type) const;

    /**
     * Normalize a feature-search query for this layer's schema.
     *
     * This is the shared backend/frontend query post-processing entry point:
     * it compiles the query through SIMFIL's schema rewrite engine, consumes
     * AST-derived referencedSchemaPaths, chooses feature vs. attribute scope,
     * and emits guarded attribute-root query branches when needed. It does not
     * infer rewrites from broad source-token scans.
     */
    [[nodiscard]] tl::expected<SearchQueryNormalization, simfil::Error>
    normalizeSearchQuery(std::string_view query, SearchQueryRequestedScope requestedScope) const;

private:
    friend class MapSchema;
    class Search;
    /** Shared symbol discovery; enum completion omits attribute/field alternatives. */
    [[nodiscard]] std::vector<SearchInterpretation> symbolInterpretations(
        std::string const& symbol,
        SearchQueryRequestedScope scope,
        bool includeFields) const;
    MapSchema& mutableOwner();
};

simfil::model_ptr<LayerSchema>
resolveInternal(simfil::res::tag<LayerSchema>, MapSchema const&, simfil::ModelNode const&);

}  // namespace mapget
