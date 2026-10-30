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

#include "mapget/model/info.h"
#include "mapget/model/memory.h"
#include "mapget/model/stringpool.h"
#include "simfil/error.h"
#include "simfil/model/model.h"
#include "simfil/model/schema.h"

namespace mapget
{

/** Owner-retaining view of one named layer in an immutable MapSchema.
 * Builder methods write to the common owner before finalization. Numeric domains
 * are graph-global; same-named feature aliases remain local to this layer.
 */
class MapSchema;
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

    using Composition = simfil::Schema::Composition;
    static constexpr auto FeatureKind = simfil::Schema::makeKind(
        StringPool::SchemaFeatureStr,
        simfil::valueTypeAffinity(simfil::ValueType::Object));
    static constexpr auto AttributeKind = simfil::Schema::makeKind(
        StringPool::SchemaAttributeStr,
        simfil::valueTypeAffinity(simfil::ValueType::Object));
    /** Typed raw bits; declarations retain exact masks, aliases and unknown bits. */
    static constexpr auto BitmaskKind = simfil::Schema::makeKind(
        StringPool::SchemaBitmaskStr,
        simfil::valueTypeAffinity(simfil::ValueType::Bitmask));

    /** Human-readable entry for a compiled JSON Schema transport branch. */
    struct Entry
    {
        simfil::SchemaId id_ = simfil::NoSchemaId;
        std::string_view key_;
        std::string_view jsonPointer_;
        std::string_view metaType_;
    };

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
        simfil::SchemaId attributeSchema_ = simfil::NoSchemaId;
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
    /** Identity definitions are owned once by the schema, including reference-only types. */
    void
    addFeatureType(FeatureTypeInfo info, simfil::SchemaId rootSchema = 0, bool produced = true);
    [[nodiscard]] std::span<FeatureTypeInfo const> featureTypeInfos() const;
    [[nodiscard]] bool producesFeatureType(std::string_view name) const;
    [[nodiscard]] FeatureTypeInfo const*
    getTypeInfo(std::string_view name, bool throwIfMissing = true) const;
    void setRootSchema(simfil::SchemaId root);
    [[nodiscard]] simfil::SchemaId rootSchema() const;

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

    /** Report this borrowing view; shared storage is accounted by MapSchema::memoryUsage(). */
    [[nodiscard]] MemoryUsageBreakdown memoryUsage() const;

    /** Add one object/array/value schema node and return its stable SchemaId. */
    [[nodiscard]] simfil::SchemaId addSchema(
        simfil::Schema::Kind kind,
        std::string key = {},
        std::string metaType = {},
        std::string jsonPointer = {});

    /** Import a declarative JSON Schema fragment into this graph, without finalizing it.
     * Local references resolve within the fragment or to previously registered schema keys.
     */
    [[nodiscard]] simfil::SchemaId addJsonSchema(nlohmann::json const& schema);

    /** Retain non-structural JSON Schema constraints and producer annotations on a domain.
     * Structural fields and mapget's transport identities cannot be overridden here.
     */
    void setJsonSchemaAnnotations(simfil::SchemaId id, nlohmann::json annotations);

    /** Register an additional lookup key for an existing schema node. */
    void registerSchemaKey(std::string key, simfil::SchemaId id);

    /** Add a direct field and optionally bind its domain. A multimap field projects repeated
     * object entries to a JSON array, while retaining its native value domain.
     */
    void addFieldSchema(
        simfil::SchemaId parent,
        std::string fieldName,
        simfil::SchemaId child = simfil::NoSchemaId,
        bool multimap = false);

    /** Add one possible element schema to an array node. */
    void addElementSchema(simfil::SchemaId parent, simfil::SchemaId child);

    /** Add an enum-like string symbol directly declared by a value node. */
    void addEnumSymbol(simfil::SchemaId schemaId, std::string symbolName);

    /** Add several enum-like string symbols directly declared by a value node. */
    void addEnumSymbols(simfil::SchemaId schemaId, std::span<const std::string> symbolNames);

    /** Attach an owning non-string enum literal; string symbols use addEnumSymbol. */
    void addEnumValue(simfil::SchemaId schemaId, simfil::ScalarValueType value);

    /** Retain an explicit logical operator instead of merging its alternatives. */
    void setComposition(simfil::SchemaId schemaId, Composition composition);
    /** Add one alternative at the same value position, not an array element. */
    void addAlternative(simfil::SchemaId schemaId, simfil::SchemaId alternative);
    /** Set a concrete producer type name independently of its structural kind. */
    void setTypeName(simfil::SchemaId schemaId, std::string name);
    /** Record undeclared object-member support; direct schemas default to closed. */
    void setOpen(simfil::SchemaId schemaId, bool open);
    /** Record nullability without conflating it with field presence. */
    void setNullable(simfil::SchemaId schemaId, std::optional<bool> nullable);
    /** Record required/optional presence on the parent-field edge. */
    void
    setFieldRequired(simfil::SchemaId schemaId, std::string field, std::optional<bool> required);

    /** Attach zserio type metadata used by completion/result-coloring consumers. */
    void setZserioType(simfil::SchemaId schemaId, std::string zserioType);

    /** Attach mapget attribute metadata and ownership for one concrete Attribute node. */
    void setAttributeMetadata(
        simfil::SchemaId schemaId,
        AttributePathOwner owner,
        std::string attributeType,
        std::string zserioType = {});

    /** Copy exact numeric semantics; input views need not outlive this call. */
    void setNumericDomain(simfil::SchemaId id, simfil::Schema::NumericDomain const& domain);
    void addNamedValue(simfil::SchemaId id, simfil::Schema::NamedValue const& value);
    void setBitWidth(simfil::SchemaId id, uint8_t width);
    [[nodiscard]] std::optional<simfil::Schema::NumericDomain>
    numericDomain(simfil::SchemaId id) const;
    void forEachNamedValue(
        simfil::SchemaId id,
        std::function<void(simfil::Schema::NamedValue const&)> const& fn) const;
    [[nodiscard]] uint8_t bitWidth(simfil::SchemaId id) const;

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

    /** Return an entry by exact key, or by feature type name as a fallback. */
    [[nodiscard]] Entry const* getSchema(std::string_view keyOrFeatureType) const;

    /** Resolve a schema key to the serialized SchemaId domain. */
    [[nodiscard]] simfil::SchemaId schemaId(std::string_view key) const;

    /** Check a transported descriptor identity without materializing its JSON Schema. */
    [[nodiscard]] bool hasSchema(simfil::SchemaId id) const;

    /** Return the packed domain kind, or Unknown when the identity is unavailable. */
    [[nodiscard]] simfil::Schema::Kind kind(simfil::SchemaId schemaId) const;

    /** Return the concrete type name, or empty when producer metadata is absent. */
    [[nodiscard]] std::string_view typeName(simfil::SchemaId schemaId) const;
    /** Return the explicit combiner, independently of its coarse affinity mask. */
    [[nodiscard]] Composition composition(simfil::SchemaId schemaId) const;
    /** Return alternative domains at the same value position. */
    [[nodiscard]] std::span<simfil::SchemaId const> alternatives(simfil::SchemaId schemaId) const;
    /** Return non-string enum literals; strings retain their canonical symbol index. */
    [[nodiscard]] std::span<simfil::ScalarValueType const>
    enumValues(simfil::SchemaId schemaId) const;
    /** Whether undeclared members are permitted; unknown domains remain conservative. */
    [[nodiscard]] bool open(simfil::SchemaId schemaId) const;
    /** Return known nullability, not field-presence information. */
    [[nodiscard]] std::optional<bool> nullable(simfil::SchemaId schemaId) const;
    /** Return known presence requirements for one direct field. */
    [[nodiscard]] std::optional<bool>
    fieldRequired(simfil::SchemaId schemaId, std::string_view field) const;
    /** A finalized absence proof; incomplete/open graphs must not prune data. */
    [[nodiscard]] bool reachabilityComplete(simfil::SchemaId schemaId) const;

    /** Return true if the schema can contain the field directly or through descendants. */
    [[nodiscard]] bool canHaveField(simfil::SchemaId schemaId, std::string_view fieldName) const;

    /** Return true if the schema can contain the enum-like string symbol through descendants. */
    [[nodiscard]] bool
    canHaveEnumSymbol(simfil::SchemaId schemaId, std::string_view symbolName) const;

    /** Return field names directly declared by this schema node. */
    [[nodiscard]] std::span<const std::string_view> directFields(simfil::SchemaId schemaId) const;

    /** Return field names reachable from this schema node. */
    [[nodiscard]] std::span<const std::string_view> nestedFields(simfil::SchemaId schemaId) const;

    /** Return enum-like string symbols reachable from this schema node. */
    [[nodiscard]] std::span<const std::string_view>
    nestedEnumSymbols(simfil::SchemaId schemaId) const;

    /** Return enum-like string symbols declared directly by this schema node. */
    [[nodiscard]] std::span<const std::string_view>
    directEnumSymbols(simfil::SchemaId schemaId) const;

    /** Return the mapget attribute type-code for an Attribute schema, if any. */
    [[nodiscard]] std::string_view attributeTypeCode(simfil::SchemaId schemaId) const;

    /** Return enum/zserio type names attached to a constant-like completion candidate. */
    [[nodiscard]] std::vector<std::string>
    constantTypeNames(simfil::SchemaId schemaId, std::string_view symbolName) const;

    /** Visit direct fields and their possible child schemas. */
    void forEachDirectField(
        simfil::SchemaId schemaId,
        const std::function<void(std::string_view, std::span<const simfil::SchemaId>)>& fn) const;

    /** Visit possible array element schemas. */
    void
    forEachElementSchema(simfil::SchemaId schemaId, const std::function<void(simfil::SchemaId)>& fn)
        const;

    /** Resolve the Feature object schema for a concrete mapget feature type. */
    [[nodiscard]] simfil::SchemaId featureSchema(std::string_view featureType) const;

    /**
     * Return the typed attribute-query root prepared by finalize(). It preserves
     * payload/alias metadata and adds runtime overlay fields, including $feature.
     * No model samples or mutations of a published graph are required.
     */
    [[nodiscard]] simfil::SchemaId
    attributeQuerySchema(std::string_view featureType, simfil::SchemaId attributeSchema) const;

    /** Resolve the Feature.properties object schema for a concrete feature type. */
    [[nodiscard]] simfil::SchemaId featurePropertiesSchema(std::string_view featureType) const;

    /** Resolve the Feature.properties.layer object schema for a concrete feature type. */
    [[nodiscard]] simfil::SchemaId attributeLayerMapSchema(std::string_view featureType) const;

    /** Resolve a child schema from a parent object/array schema and field name. */
    [[nodiscard]] simfil::SchemaId childSchema(
        simfil::SchemaId parent,
        std::string_view fieldName,
        std::optional<simfil::Schema::Kind> preferredKind = std::nullopt) const;

    /** Classify whether a path belongs to the feature itself or to an attribute branch. */
    [[nodiscard]] PathOwner ownerForPath(
        std::string_view featureType,
        simfil::SchemaId rootSchema,
        std::span<const std::string> fieldPath) const;

    /** Return scalar value paths addressed by an attribute type-code shorthand. */
    [[nodiscard]] std::vector<NamedSchemaPath>
    scalarFieldPathsForAttribute(simfil::SchemaId rootSchema, std::string_view attributeTypeCode)
        const;

    /** Return all feature types represented by Feature schema roots in this layer schema. */
    [[nodiscard]] std::vector<std::string> featureTypes() const;

    /** Return all searchable attribute contexts represented by this layer schema. */
    [[nodiscard]] std::vector<AttributePathOwner> attributeScopes() const;

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
    MapSchema& mutableOwner();
};

simfil::model_ptr<LayerSchema>
resolveInternal(simfil::res::tag<LayerSchema>, MapSchema const&, simfil::ModelNode const&);

}  // namespace mapget
